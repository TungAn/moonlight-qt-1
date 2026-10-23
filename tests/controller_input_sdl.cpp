#include <Limelight.h>
#include <QSet>
#include "streaming/input/input.h"
#include <cassert>
#include <vector>
#include <iostream>
#include <functional>

struct Packet { int slot, mask, buttons, lt, rt, lx, ly, rx, ry; };
static std::vector<Packet> packets;
static std::vector<int> mouseButtonEvents, verticalScrollEvents, horizontalScrollEvents;
static int mouseMoves, mousePresses, mouseReleases, scrolls, motions, touches, arrivals;
static int touchCancels, keysRaised;
static bool touchHeld;
// SDL virtual gamepads do not model touch hardware. Supply two pads with two
// contacts each, while keeping all button/axis/hotplug handling in real SDL.
extern "C" int SDL_GameControllerGetNumTouchpads(SDL_GameController*) { return 2; }
extern "C" int SDL_GameControllerGetNumTouchpadFingers(SDL_GameController*, int) { return 2; }
extern "C" int SDL_GameControllerGetTouchpadFinger(SDL_GameController*, int, int, Uint8* down,
                                                 float*, float*, float*)
{ *down = touchHeld ? SDL_PRESSED : SDL_RELEASED; return 0; }
extern "C" {
int LiSendMultiControllerEvent(short slot, short mask, int buttons, unsigned char lt, unsigned char rt,
                               short lx, short ly, short rx, short ry)
{ packets.push_back({slot, mask, buttons, lt, rt, lx, ly, rx, ry}); return 0; }
int LiSendMouseMoveEvent(short, short) { ++mouseMoves; return 0; }
int LiSendMouseButtonEvent(char action, int button)
{ mouseButtonEvents.push_back(action == BUTTON_ACTION_PRESS ? button : -button);
  if (action == BUTTON_ACTION_PRESS) ++mousePresses; else ++mouseReleases; return 0; }
int LiSendScrollEvent(signed char amount) { verticalScrollEvents.push_back(amount); ++scrolls; return 0; }
int LiSendHScrollEvent(signed char amount) { horizontalScrollEvents.push_back(amount); ++scrolls; return 0; }
int LiSendControllerMotionEvent(uint8_t, uint8_t, float, float, float) { ++motions; return 0; }
int LiSendControllerTouchEvent2(uint8_t, uint8_t type, uint8_t, uint32_t, float, float, float)
{ ++touches; if (type == LI_TOUCH_EVENT_CANCEL_ALL) ++touchCancels; return 0; }
int LiSendControllerBatteryEvent(uint8_t, uint8_t, uint8_t) { return 0; }
int LiSendControllerArrivalEvent(uint8_t, uint16_t, uint8_t, uint32_t, uint16_t) { ++arrivals; return 0; }
}

// Non-controller subsystems are outside this fixture. Controller input.cpp and
// gamepad.cpp, SDL device handling, timers and all packet paths are production.
namespace WMUtils { bool isRunningDesktopEnvironment() { return true; } }
void SdlInputHandler::raiseAllKeys() { ++keysRaised; }
void SdlInputHandler::updatePointerRegionLock() {}
bool SdlInputHandler::isMouseInVideoRegion(int, int, int, int) { return true; }
void SdlInputHandler::handleMouseMotionEvent(SDL_MouseMotionEvent*) {}
void SdlInputHandler::handleAbsoluteFingerEvent(SDL_TouchFingerEvent*) {}
void SdlInputHandler::handleRelativeFingerEvent(SDL_TouchFingerEvent*) {}

class ControllerInputTest {
public:
    static void runGamescope(const std::vector<std::string>& displays,
                             const std::function<void(bool)>& setOwnership)
    {
        StreamingPreferences prefs;
        SDL_Window* window = SDL_CreateWindow("stream", 0, 0, 32, 32, SDL_WINDOW_SHOWN);
        assert(window);
        {
            SdlInputHandler input(prefs, 32, 32);
            input.setWindow(window);
            input.m_ControllerInput.focused = input.m_ControllerInput.visible = true;
            if (!displays.empty()) {
                input.m_GamescopeFocus.reset(new GamescopeInputFocus(input.m_GamescopeEventType, 0, displays));
            }
            assert(input.m_GamescopeFocus);
            auto waitOwnership = [&](bool active) {
                Uint32 start = SDL_GetTicks();
                do {
                    input.refreshGamescopeInputOwnership();
                    if (input.m_ControllerInput.active == active &&
                            (input.m_GamescopeSnapshot & GamescopeInputFocus::Known)) return;
                    SDL_Delay(1);
                } while (SDL_GetTicks() - start < 3000);
                assert(false && "controller observer ownership timeout");
            };
            waitOwnership(true);
            int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
            assert(device >= 0);
            SDL_ControllerDeviceEvent added = {};
            added.type = SDL_CONTROLLERDEVICEADDED;
            added.which = device;
            input.handleControllerDeviceEvent(&added);
            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
            auto button = [&](bool down) {
                SDL_ControllerButtonEvent event = {};
                event.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
                event.state = down ? SDL_PRESSED : SDL_RELEASED;
                event.timestamp = input.m_ControllerInput.resumeTime + 1;
                event.which = input.m_GamepadState[0].jsId;
                event.button = SDL_CONTROLLER_BUTTON_A;
                input.handleControllerButtonEvent(&event);
            };
            button(true);
            assert(packets.back().buttons == A_FLAG);
            const int mask = input.m_GamepadMask;
            const int previousKeyReleases = keysRaised;
            setOwnership(false);
            waitOwnership(false);
            assert(keysRaised > previousKeyReleases);
            SDL_Event mappedInput = {};
            mappedInput.type = SDL_KEYDOWN;
            assert(!input.handleInputOwnershipEvent(mappedInput)); // Steam's on-screen keyboard
            mappedInput.type = SDL_KEYUP;
            assert(!input.handleInputOwnershipEvent(mappedInput));
            mappedInput.type = SDL_MOUSEBUTTONDOWN;
            assert(input.handleInputOwnershipEvent(mappedInput));
            mappedInput.type = SDL_USEREVENT;
            assert(!input.handleInputOwnershipEvent(mappedInput));
            assert(input.m_ControllerInput.focused); // Crucial: no SDL focus event!
            assert(packets.back().buttons == 0 && packets.back().mask == mask);
            const auto count = packets.size();
            button(true);
            assert(packets.size() == count);
            setOwnership(true);
            waitOwnership(true);
            button(true);
            assert(packets.back().buttons == A_FLAG && packets.back().mask == mask);
            const uint64_t before = input.m_GamescopeFocus->snapshot();
            setOwnership(false);
            setOwnership(true);
            Uint32 start = SDL_GetTicks();
            // Leave the session unprocessed until a full open/close has occurred.
            while ((input.m_GamescopeFocus->snapshot() >> GamescopeInputFocus::GenerationShift) ==
                       (before >> GamescopeInputFocus::GenerationShift) ||
                   !(input.m_GamescopeFocus->snapshot() & GamescopeInputFocus::Allowed)) {
                assert(SDL_GetTicks() - start < 3000);
                SDL_Delay(1);
            }
            input.refreshGamescopeInputOwnership();
            assert(input.m_ControllerInput.active && input.m_ControllerInput.focused);
            assert(packets.back().buttons == 0 && packets.back().mask == mask);
            input.stopControllerInput();
            SDL_JoystickDetachVirtual(device);
        }
        SDL_DestroyWindow(window);
    }

    static void run(bool multi, bool background, bool gamingMode)
    {
        if (gamingMode) qputenv("SteamGamepadUI", "1");
        else qunsetenv("SteamGamepadUI");
        StreamingPreferences prefs;
        prefs.multiController = multi;
        prefs.backgroundGamepad = background;
        SDL_Window* window = SDL_CreateWindow("test", 0, 0, 32, 32, SDL_WINDOW_SHOWN);
        assert(window);
        {
            SdlInputHandler input(prefs, 32, 32);
            assert(input.m_GamepadMouseEventType != SDL_USEREVENT);
            SDL_Event sessionEvent = {};
            sessionEvent.type = SDL_USEREVENT;
            assert(!input.handleGamepadMouseEvent(sessionEvent));
            assert(!input.handleInputOwnershipEvent(sessionEvent));
            assert(SDL_GetHintBoolean(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, SDL_FALSE));
            input.setWindow(window);
            auto windowEvent = [&](Uint8 kind) {
                SDL_Event event = {};
                event.type = SDL_WINDOWEVENT;
                event.window.windowID = SDL_GetWindowID(window);
                event.window.event = kind;
                input.handleInputOwnershipEvent(event);
            };
            windowEvent(SDL_WINDOWEVENT_SHOWN);
            windowEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
            assert(input.m_ControllerInput.active);

            int devices[2];
            SDL_Joystick* joysticks[2];
            for (int i = 0; i < 2; ++i) {
                devices[i] = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                                       SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
                assert(devices[i] >= 0);
                joysticks[i] = SDL_JoystickOpen(devices[i]);
                assert(joysticks[i]);
                SDL_ControllerDeviceEvent event = {};
                event.type = SDL_CONTROLLERDEVICEADDED;
                event.which = devices[i];
                input.handleControllerDeviceEvent(&event);
                assert(input.m_GamepadState[i].controller);
            }
            SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
            const int mask = input.m_GamepadMask;
            auto button = [&](int device, Uint8 b, bool down, Uint32 timestamp = Uint32(-1)) {
                SDL_ControllerButtonEvent event = {};
                event.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
                event.timestamp = timestamp == Uint32(-1) ? input.m_ControllerInput.resumeTime + 1 : timestamp;
                event.which = input.m_GamepadState[device].jsId;
                event.button = b;
                event.state = down ? SDL_PRESSED : SDL_RELEASED;
                input.handleControllerButtonEvent(&event);
            };
            auto axis = [&](int device, Uint8 a, Sint16 value) {
                SDL_ControllerAxisEvent event = {};
                event.type = SDL_CONTROLLERAXISMOTION;
                event.timestamp = input.m_ControllerInput.resumeTime + 1;
                event.which = input.m_GamepadState[device].jsId;
                event.axis = a;
                event.value = value;
                input.handleControllerAxisEvent(&event);
            };
            button(0, SDL_CONTROLLER_BUTTON_A, true);
            axis(0, SDL_CONTROLLER_AXIS_LEFTX, 25000);
            axis(1, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
            assert(packets.back().rt == 255);
            assert(packets.back().mask == mask);
            assert(input.m_GamepadState[0].buttons == A_FLAG);

            // Exercise all controller-generated mouse paths and sensor paths.
            input.m_GamepadState[0].mouseEmulationActive = true;
            for (const auto& mapping : {std::pair<Uint8, int>{SDL_CONTROLLER_BUTTON_A, BUTTON_LEFT},
                    {SDL_CONTROLLER_BUTTON_B, BUTTON_RIGHT}, {SDL_CONTROLLER_BUTTON_X, BUTTON_MIDDLE},
                    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, BUTTON_X1}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, BUTTON_X2}}) {
                button(0, mapping.first, true);
                assert(mouseButtonEvents.back() == mapping.second);
                button(0, mapping.first, false);
                assert(mouseButtonEvents.back() == -mapping.second);
                assert(input.m_GamepadState[0].mouseButtonsDown == 0);
            }
            const auto verticalBefore = verticalScrollEvents.size(), horizontalBefore = horizontalScrollEvents.size();
            for (Uint8 direction : {SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                                    SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT}) {
                button(0, direction, true);
                button(0, direction, false);
            }
            assert(verticalScrollEvents.size() == verticalBefore + 2);
            assert(verticalScrollEvents[verticalBefore] == 1 && verticalScrollEvents.back() == -1);
            assert(horizontalScrollEvents.size() == horizontalBefore + 2);
            assert(horizontalScrollEvents[horizontalBefore] == -1 && horizontalScrollEvents.back() == 1);
            button(0, SDL_CONTROLLER_BUTTON_A, true);
            assert(mousePresses > 0);
            SDL_Event tick = {};
            tick.type = input.m_GamepadMouseEventType;
            tick.common.timestamp = input.m_ControllerInput.resumeTime + 1;
            assert(input.handleGamepadMouseEvent(tick));
            assert(mouseMoves > 0);
            input.m_GamepadState[0].gyroReportPeriodMs = 1;
            SDL_ControllerSensorEvent sensor = {};
            sensor.which = input.m_GamepadState[0].jsId;
            sensor.sensor = SDL_SENSOR_GYRO;
            sensor.timestamp = input.m_ControllerInput.resumeTime + 10;
            sensor.data[0] = 1;
            input.handleControllerSensorEvent(&sensor);
            const int activeMotions = motions;
            assert(activeMotions > 0);
            SDL_ControllerTouchpadEvent touch = {};
            touch.type = SDL_CONTROLLERTOUCHPADDOWN;
            touch.which = sensor.which;
            touch.timestamp = sensor.timestamp;
            input.handleControllerTouchpadEvent(&touch);

            const int cancelsBefore = touchCancels;
            windowEvent(SDL_WINDOWEVENT_FOCUS_LOST); // same path for overlay-caused focus loss
            const bool suspended = !background || gamingMode;
            assert(input.m_ControllerInput.active != suspended);
            if (suspended) {
                assert(input.m_GamepadMask == mask);
                assert(input.m_GamepadState[0].buttons == 0);
                assert(input.m_GamepadState[0].lsX == 0);
                assert(input.m_GamepadState[1].rt == 0);
                assert(mouseReleases > 0);
                assert(touchCancels == cancelsBefore + 4); // both pads on both controllers
                for (auto it = packets.end() - 2; it != packets.end(); ++it) {
                    assert(it->mask == mask && it->buttons == 0 && it->lx == 0 && it->rt == 0);
                }
                auto count = packets.size();
                const int noMoves = mouseMoves, noPresses = mousePresses, noScrolls = scrolls;
                const int noMotions = motions, noTouches = touches;
                windowEvent(SDL_WINDOWEVENT_FOCUS_LOST);
                button(0, SDL_CONTROLLER_BUTTON_A, true);
                button(0, SDL_CONTROLLER_BUTTON_DPAD_UP, true);
                axis(1, SDL_CONTROLLER_AXIS_LEFTX, 32767);
                input.handleControllerSensorEvent(&sensor);
                input.handleControllerTouchpadEvent(&touch);
                input.handleGamepadMouseEvent(tick);
                assert(packets.size() == count);
                assert(mouseMoves == noMoves && mousePresses == noPresses && scrolls == noScrolls);
                assert(motions == noMotions && touches == noTouches);

                // Physical controls held at resume are blocked until release.
                SDL_JoystickSetVirtualButton(joysticks[0], SDL_CONTROLLER_BUTTON_A, SDL_PRESSED);
                SDL_JoystickSetVirtualAxis(joysticks[0], SDL_CONTROLLER_AXIS_LEFTX, 32767);
                SDL_JoystickUpdate();
                touchHeld = true;
                windowEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
                assert(input.m_ControllerInput.active);
                assert(input.m_GamepadState[0].rearm.buttons & (1u << SDL_CONTROLLER_BUTTON_A));
                assert(input.m_GamepadState[0].rearm.axes & (1u << SDL_CONTROLLER_AXIS_LEFTX));
                int touchCount = touches;
                touch.timestamp = input.m_ControllerInput.resumeTime + 1;
                input.handleControllerTouchpadEvent(&touch);
                assert(touches == touchCount);
                touchHeld = false;
                touch.type = SDL_CONTROLLERTOUCHPADUP;
                input.handleControllerTouchpadEvent(&touch);
                assert(touches == touchCount); // release rearms, then a new contact is allowed
                touch.type = SDL_CONTROLLERTOUCHPADDOWN;
                input.handleControllerTouchpadEvent(&touch);
                assert(touches == touchCount + 1);
                button(0, SDL_CONTROLLER_BUTTON_A, true);
                axis(0, SDL_CONTROLLER_AXIS_LEFTX, 32767);
                assert(packets.size() == count);
                button(0, SDL_CONTROLLER_BUTTON_A, false);
                axis(0, SDL_CONTROLLER_AXIS_LEFTX, 0);
                input.m_GamepadState[0].mouseEmulationActive = false;
                button(0, SDL_CONTROLLER_BUTTON_A, true);
                assert(packets.back().buttons == A_FLAG);
                button(0, SDL_CONTROLLER_BUTTON_B, true, input.m_ControllerInput.resumeTime);
                assert(packets.back().buttons == A_FLAG); // stale boundary event

                // Axis batching must not steal events across a focus boundary.
                SDL_FlushEvents(SDL_FIRSTEVENT, SDL_LASTEVENT);
                SDL_Delay(2);
                SDL_Event queued = {};
                queued.type = SDL_WINDOWEVENT;
                queued.window.windowID = SDL_GetWindowID(window);
                queued.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
                SDL_PushEvent(&queued);
                queued = {};
                queued.type = SDL_CONTROLLERAXISMOTION;
                queued.caxis.which = input.m_GamepadState[0].jsId;
                queued.caxis.axis = SDL_CONTROLLER_AXIS_RIGHTX;
                queued.caxis.value = 30000;
                SDL_PushEvent(&queued);
                axis(0, SDL_CONTROLLER_AXIS_LEFTX, 25000);
                assert(input.m_GamepadState[0].rsX == 0);
                assert(SDL_PeepEvents(&queued, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) == 1);
                assert(queued.type == SDL_WINDOWEVENT);
                input.handleInputOwnershipEvent(queued);
                assert(!SDL_HasEvent(SDL_CONTROLLERAXISMOTION));
                count = packets.size();
                // Inject old input after resume, including the timer wake-up.
                windowEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
                tick.common.timestamp = input.m_ControllerInput.resumeTime;
                input.handleGamepadMouseEvent(tick);
                assert(packets.size() == count);
            }

            // Repeated transitions and real SDL removal/arrival during suspension.
            windowEvent(SDL_WINDOWEVENT_FOCUS_LOST);
            SDL_ControllerDeviceEvent removal = {};
            removal.type = SDL_CONTROLLERDEVICEREMOVED;
            removal.which = input.m_GamepadState[1].jsId;
            assert(SDL_JoystickDetachVirtual(devices[1]) == 0);
            SDL_JoystickUpdate();
            input.handleControllerDeviceEvent(&removal);
            SDL_JoystickClose(joysticks[1]);
            assert(!input.m_GamepadState[1].controller);
            assert(input.m_GamepadMask == (multi ? (mask & ~(1 << 1)) : mask));
            SDL_ControllerDeviceEvent addition = {};
            addition.type = SDL_CONTROLLERDEVICEADDED;
            devices[1] = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                                   SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
            assert(devices[1] >= 0);
            joysticks[1] = SDL_JoystickOpen(devices[1]);
            assert(joysticks[1]);
            addition.which = devices[1];
            input.handleControllerDeviceEvent(&addition);
            assert(input.m_GamepadState[1].controller);
            assert(input.m_GamepadMask == mask);
            for (int i = 0; i < 10; ++i) {
                windowEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
                windowEvent(SDL_WINDOWEVENT_FOCUS_LOST);
            }
            // A real timer only queues work. It may not transmit on its thread.
            windowEvent(SDL_WINDOWEVENT_FOCUS_GAINED);
            input.m_GamepadState[0].mouseEmulationActive = true;
            input.m_GamepadState[0].lsX = 32767;
            input.updateGamepadMouseTimer();
            assert(input.m_GamepadMouseTimer);
            const int movesBeforeTick = mouseMoves;
            SDL_Delay(70);
            assert(mouseMoves == movesBeforeTick);
            assert(SDL_HasEvent(input.m_GamepadMouseEventType));
            windowEvent(SDL_WINDOWEVENT_FOCUS_LOST);
            if (!background || gamingMode) {
                assert(!input.m_GamepadMouseTimer);
                assert(!SDL_HasEvent(input.m_GamepadMouseEventType));
            }
            input.stopControllerInput();
            const auto count = packets.size();
            input.stopControllerInput();
            button(0, SDL_CONTROLLER_BUTTON_X, true);
            assert(packets.size() == count);
            for (int i = 1; i >= 0; --i) {
                SDL_JoystickClose(joysticks[i]);
                SDL_JoystickDetachVirtual(devices[i]);
            }
        }
        SDL_DestroyWindow(window);
    }
};

void runGamescopeControllerInputTest(const std::vector<std::string>& displays,
                                     const std::function<void(bool)>& setOwnership)
{
    ControllerInputTest::runGamescope(displays, setOwnership);
}

#ifndef CONTROLLER_INPUT_FIXTURE_ONLY
int main()
{
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0);
    SDL_version version;
    SDL_GetVersion(&version);
    std::cout << "SDL runtime " << int(version.major) << '.' << int(version.minor) << '.' << int(version.patch) << '\n';
    for (bool multi : {false, true}) {
        ControllerInputTest::run(multi, false, false);
        ControllerInputTest::run(multi, true, false);
#ifdef Q_OS_LINUX
        ControllerInputTest::run(multi, true, true);
#endif
    }
    SDL_Quit();
    std::cout << "Production SDL controller ownership and packet checks passed\n";
}

#endif
