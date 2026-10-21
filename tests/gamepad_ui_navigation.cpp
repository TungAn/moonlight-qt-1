#include "gui/sdlgamepadkeynavigation.h"
#include "gamescope_root_fixture.h"
#include <QGuiApplication>
#include <QKeyEvent>
#include <QWindow>
#include <iostream>

class NavigationWindow : public QWindow {
public:
    int presses = 0, releases = 0;
    int lastKey = 0;
    Qt::KeyboardModifiers lastModifiers = Qt::NoModifier;
protected:
    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
            auto* key = static_cast<QKeyEvent*>(event);
            if (event->type() == QEvent::KeyPress) {
                ++presses;
                lastKey = key->key();
                lastModifiers = key->modifiers();
            }
            else ++releases;
            return true;
        }
        return QWindow::event(event);
    }
};

class GamepadNavigationTest {
public:
    static void run(Root& root)
    {
        StreamingPreferences prefs;
        prefs.backgroundGamepad = true; // UI navigation must still require ownership.
        SdlGamepadKeyNavigation nav(&prefs);
        NavigationWindow window;
        window.resize(32, 32);
        window.show();
        window.requestActivate();
        QGuiApplication::processEvents();
        assert(QGuiApplication::focusWindow() == &window);
        assert(SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0);
        nav.enable();
        int devices[3];
        SDL_Joystick* joysticks[3];
        for (int i = 0; i < 2; ++i) {
            devices[i] = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
            assert(devices[i] >= 0);
            joysticks[i] = SDL_JoystickOpen(devices[i]);
            assert(joysticks[i]);
        }
        nav.notifyWindowFocus(true);
        nav.m_PollingTimer->stop(); // Drive the production polling slot deterministically.
        assert(nav.m_GamescopeFocus);
        auto wait = [&](bool active) {
            const Uint32 start = SDL_GetTicks();
            for (;;) {
                nav.refreshInputOwnership();
                if (nav.m_InputOwnership.active == active &&
                        (nav.m_GamescopeSnapshot & GamescopeInputFocus::Known)) return;
                assert(SDL_GetTicks() - start < 3000);
                SDL_Delay(1);
            }
        };
        auto poll = [&] { nav.onPollingTimerFired(); };
        auto button = [&](int device, int code, bool down) {
            assert(SDL_JoystickSetVirtualButton(joysticks[device], code, down) == 0);
            SDL_Delay(2); // Fresh SDL event after the resume boundary millisecond.
            poll();
            assert(window.presses == window.releases); // Never leave a Qt key held.
        };
        auto tap = [&](int device, int code, int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            const int before = window.presses;
            button(device, code, true);
            if (window.presses != before + 1 || window.lastKey != key) {
                std::cerr << "UI tap failed: device=" << device << " key=" << key
                          << " last=" << window.lastKey << " presses=" << window.presses - before
                          << " controllers=" << nav.m_Gamepads.size()
                          << " rearm=" << nav.m_Rearm[SDL_JoystickInstanceID(joysticks[device])].buttons << '\n';
            }
            assert(window.presses == before + 1 && window.lastKey == key);
            assert(window.lastModifiers == modifiers);
            button(device, code, false);
            assert(window.presses == before + 1);
        };
        auto axis = [&](int value) {
            assert(SDL_JoystickSetVirtualAxis(joysticks[0], SDL_CONTROLLER_AXIS_LEFTX, value) == 0);
            nav.m_LastAxisNavigationEventTime = SDL_GetTicks() - 151;
            poll();
        };
        wait(true);
        poll();
        assert(nav.m_Gamepads.size() == 2);
        tap(0, SDL_CONTROLLER_BUTTON_DPAD_DOWN, Qt::Key_Down); // Host/app selection.
        tap(1, SDL_CONTROLLER_BUTTON_A, Qt::Key_Return);
        button(0, SDL_CONTROLLER_BUTTON_A, true); // Held across menu opening.
        const int beforeMenu = window.presses;
        root.focus(769);
        wait(false);
        assert(nav.m_HasFocus && QGuiApplication::focusWindow() == &window);
        button(1, SDL_CONTROLLER_BUTTON_DPAD_DOWN, true);
        button(1, SDL_CONTROLLER_BUTTON_START, true);
        axis(32767);
        assert(window.presses == beforeMenu);
        root.focus(App);
        wait(true);
        poll();
        axis(32767);
        assert(window.presses == beforeMenu); // Held controls require release/centering.
        button(0, SDL_CONTROLLER_BUTTON_A, false);
        button(1, SDL_CONTROLLER_BUTTON_DPAD_DOWN, false);
        button(1, SDL_CONTROLLER_BUTTON_START, false);
        axis(0);
        tap(0, SDL_CONTROLLER_BUTTON_A, Qt::Key_Return);
        const int beforeAxis = window.presses;
        axis(32767);
        assert(window.presses == beforeAxis + 1 && window.lastKey == Qt::Key_Right);
        axis(0);

        nav.setUiNavMode(true); // Settings controls and their alternate mappings.
        tap(0, SDL_CONTROLLER_BUTTON_DPAD_UP, Qt::Key_Tab, Qt::ShiftModifier);
        tap(0, SDL_CONTROLLER_BUTTON_DPAD_DOWN, Qt::Key_Tab);
        tap(1, SDL_CONTROLLER_BUTTON_A, Qt::Key_Space);
        tap(1, SDL_CONTROLLER_BUTTON_B, Qt::Key_Escape);
        tap(0, SDL_CONTROLLER_BUTTON_START, Qt::Key_Hangup);
        prefs.swapFaceButtons = true;
        tap(0, SDL_CONTROLLER_BUTTON_A, Qt::Key_Escape);
        prefs.swapFaceButtons = false;

        // Quick Access / app switching: global owner changes without Qt focus loss.
        for (uint32_t owner : {769u, 42u}) {
            const int before = window.presses;
            root.focus(owner);
            wait(false);
            button(0, SDL_CONTROLLER_BUTTON_A, true);
            button(0, SDL_CONTROLLER_BUTTON_A, false);
            root.focus(App);
            wait(true);
            poll();
            assert(window.presses == before);
            tap(0, SDL_CONTROLLER_BUTTON_A, Qt::Key_Space);
        }
        const int beforeStale = window.presses;
        SDL_Event stale = {};
        stale.type = SDL_CONTROLLERBUTTONDOWN;
        stale.cbutton.timestamp = nav.m_InputOwnership.resumeTime;
        stale.cbutton.which = SDL_JoystickInstanceID(joysticks[0]);
        stale.cbutton.button = SDL_CONTROLLER_BUTTON_A;
        assert(SDL_PeepEvents(&stale, 1, SDL_ADDEVENT, 0, 0) == 1);
        poll();
        assert(window.presses == beforeStale);

        // Preserve an overlay boundary even if both changes finish between UI polls.
        const uint64_t generation = nav.m_GamescopeFocus->snapshot() >> GamescopeInputFocus::GenerationShift;
        root.focus(769);
        root.focus(App);
        const Uint32 start = SDL_GetTicks();
        while ((nav.m_GamescopeFocus->snapshot() >> GamescopeInputFocus::GenerationShift) == generation ||
                !(nav.m_GamescopeFocus->snapshot() & GamescopeInputFocus::Allowed)) {
            assert(SDL_GetTicks() - start < 3000);
            SDL_Delay(1);
        }
        poll();
        assert(window.presses == beforeStale);

        root.focus(769);
        wait(false);
        devices[2] = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
            SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        joysticks[2] = SDL_JoystickOpen(devices[2]);
        assert(joysticks[2]);
        poll();
        assert(nav.m_Gamepads.size() == 3);
        SDL_JoystickClose(joysticks[2]);
        SDL_JoystickDetachVirtual(devices[2]);
        poll();
        assert(nav.m_Gamepads.size() == 2);
        root.focus(App);
        wait(true);
        nav.notifyWindowFocus(false);
        const int beforeUnfocused = window.presses;
        button(0, SDL_CONTROLLER_BUTTON_A, true);
        button(0, SDL_CONTROLLER_BUTTON_A, false);
        assert(window.presses == beforeUnfocused);
        assert(SDL_JoystickSetVirtualAxis(joysticks[0], SDL_CONTROLLER_AXIS_LEFTX, 32767) == 0);
        // The timer is stopped: deliberately do not update SDL's cached axes.
        nav.notifyWindowFocus(true);
        nav.m_PollingTimer->stop();
        poll();
        axis(32767);
        assert(window.presses == beforeUnfocused);
        axis(0);
        tap(0, SDL_CONTROLLER_BUTTON_A, Qt::Key_Space);

        // Match the real handoff: completely release SDL controllers/joysticks,
        // then rediscover them. Virtual devices must be recreated after teardown.
        auto detach = [&] {
            for (int i = 1; i >= 0; --i) {
                SDL_JoystickClose(joysticks[i]);
                SDL_JoystickDetachVirtual(devices[i]);
            }
        };
        detach();
        nav.disable();
        assert(!nav.m_GamescopeFocus && nav.m_Gamepads.isEmpty());
        assert(SDL_WasInit(SDL_INIT_JOYSTICK) == 0);
        nav.enable();
        nav.m_PollingTimer->stop();
        for (int i = 0; i < 2; ++i) {
            devices[i] = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
            assert(devices[i] >= 0);
            joysticks[i] = SDL_JoystickOpen(devices[i]);
            assert(joysticks[i]);
        }
        wait(true);
        poll();
        tap(1, SDL_CONTROLLER_BUTTON_A, Qt::Key_Space);
        root.focus(769);
        wait(false);
        detach();
        nav.disable();
        poll(); // A queued timeout after disable must be harmless.
        assert(!nav.m_GamescopeFocus && nav.m_Gamepads.isEmpty());
    }
};

int main(int argc, char** argv)
{
    assert(argc == 4);
    qputenv("QT_QPA_PLATFORM", "xcb");
    qputenv("SDL_VIDEODRIVER", "x11");
    qputenv("SteamGamepadUI", "1");
    qputenv("PYROLIGHT_GAMESCOPE_DISPLAY", argv[1]);
    QGuiApplication app(argc, argv);
    // Keep attached physical controllers out of this virtual-device fixture.
    SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x0000/0x0000", SDL_HINT_OVERRIDE);
    // Match Linux menu startup: Qt owns video; SDL initializes controllers only.
    assert(SDL_Init(SDL_INIT_TIMER) == 0);
    assert(SDL_GetCurrentVideoDriver() == nullptr);
    Root root(argv[1]);
    root.initialize(true);
    GamepadNavigationTest::run(root);
    SDL_Quit();
    std::cout << "Production Qt menu navigation ownership checks passed\n";
}
