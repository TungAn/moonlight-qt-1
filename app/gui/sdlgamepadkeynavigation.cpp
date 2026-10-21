#include "sdlgamepadkeynavigation.h"

#include <QKeyEvent>
#include <QGuiApplication>
#include <QWindow>
#include <QPointer>

#include "settings/mappingmanager.h"

#define AXIS_NAVIGATION_REPEAT_DELAY 150

SdlGamepadKeyNavigation::SdlGamepadKeyNavigation(StreamingPreferences* prefs)
    : m_Prefs(prefs),
      m_Enabled(false),
      m_UiNavMode(false),
      m_FirstPoll(false),
      m_HasFocus(false),
      m_LastAxisNavigationEventTime(0),
      m_ObserverInitialized(false),
      m_GamescopeSnapshot(0)
{
    m_PollingTimer = new QTimer(this);
    connect(m_PollingTimer, &QTimer::timeout, this, &SdlGamepadKeyNavigation::onPollingTimerFired);
}

SdlGamepadKeyNavigation::~SdlGamepadKeyNavigation()
{
    disable();
}

void SdlGamepadKeyNavigation::enable()
{
    if (m_Enabled) {
        return;
    }

    // We have to initialize and uninitialize this in enable()/disable()
    // because we need to get out of the way of the Session class. If it
    // doesn't get to reinitialize the GC subsystem, it won't get initial
    // arrival events. Additionally, there's a race condition between
    // our QML objects being destroyed and SDL being deinitialized that
    // this solves too.
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) failed: %s",
                     SDL_GetError());
        return;
    }

    MappingManager mappingManager;
    mappingManager.applyMappings();

    // Drop all pending gamepad add events. SDL will generate these for us
    // on first init of the GC subsystem. We can't depend on them due to
    // overlapping lifetimes of SdlGamepadKeyNavigation instances, so we
    // will attach ourselves.
    //
    // NB: We use SDL_JoystickUpdate() instead of SDL_PumpEvents() because
    // the latter can do a bit more work that we want (like handling video
    // events that we intentionally do not want to process yet).
    SDL_JoystickUpdate();
    SDL_FlushEvent(SDL_CONTROLLERDEVICEADDED);

    // Open all currently attached game controllers
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            SDL_GameController* gc = SDL_GameControllerOpen(i);
            if (gc != nullptr) {
                m_Gamepads.append(gc);
            }
        }
    }

    m_Enabled = true;
    m_ObserverInitialized = false;
    m_GamescopeSnapshot = 0;
    m_InputOwnership.compositorAllowed = true;

    // Start the polling timer if the window is focused
    updateTimerState();
}

void SdlGamepadKeyNavigation::disable()
{
    if (!m_Enabled) {
        return;
    }

    m_Enabled = false;
    updateTimerState();
    m_GamescopeFocus.reset(); // Join before releasing SDL's controller subsystem.
    m_Rearm.clear();
    Q_ASSERT(!m_PollingTimer->isActive());

    while (!m_Gamepads.isEmpty()) {
        SDL_GameControllerClose(m_Gamepads[0]);
        m_Gamepads.removeAt(0);
    }

    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

void SdlGamepadKeyNavigation::notifyWindowFocus(bool hasFocus)
{
    m_HasFocus = hasFocus;
    updateTimerState();
}

void SdlGamepadKeyNavigation::onPollingTimerFired()
{
    if (!m_Enabled) return;
    SDL_Event event;

    // Update joystick state without pumping other events (see enable() comment)
    SDL_JoystickUpdate();
    refreshInputOwnership();

    // Discard any pending button events on the first poll to avoid picking up
    // stale input data from the stream session (like the quit combo).
    if (m_FirstPoll) {
        SDL_FlushEvent(SDL_CONTROLLERBUTTONDOWN);
        SDL_FlushEvent(SDL_CONTROLLERBUTTONUP);
        // Qt focus can stop this timer completely. Sample after the first
        // joystick update, not from the cache left behind before focus loss.
        if (m_InputOwnership.active) {
            for (auto gamepad : std::as_const(m_Gamepads)) prepareGamepad(gamepad);
        }
        m_FirstPoll = false;
    }

    // Peep events rather than polling to avoid calling SDL_PumpEvents()
    while (m_Enabled && SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_FIRSTEVENT, SDL_LASTEVENT) == 1) {
        switch (event.type) {
        case SDL_QUIT:
            // SDL may send us a quit event since we initialize
            // the video subsystem on startup. If we get one,
            // forward it on for Qt to take care of.
            QCoreApplication::instance()->quit();
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        {
            refreshInputOwnership();
            if (!m_InputOwnership.accepts(event.cbutton.timestamp) ||
                    !m_Rearm.contains(event.cbutton.which) ||
                    !m_Rearm[event.cbutton.which].button(event.cbutton.button,
                        event.type == SDL_CONTROLLERBUTTONDOWN) ||
                    event.type != SDL_CONTROLLERBUTTONDOWN) {
                break;
            }

            // Swap face buttons if needed
            if (m_Prefs->swapFaceButtons) {
                switch (event.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_A:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_B;
                    break;
                case SDL_CONTROLLER_BUTTON_B:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_A;
                    break;
                case SDL_CONTROLLER_BUTTON_X:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_Y;
                    break;
                case SDL_CONTROLLER_BUTTON_Y:
                    event.cbutton.button = SDL_CONTROLLER_BUTTON_X;
                    break;
                }
            }

            switch (event.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_DPAD_UP:
                if (m_UiNavMode) {
                    // Back-tab
                    sendKeyClick(Qt::Key_Tab, Qt::ShiftModifier);
                }
                else {
                    sendKeyClick(Qt::Key_Up);
                }
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
                if (m_UiNavMode) {
                    sendKeyClick(Qt::Key_Tab);
                }
                else {
                    sendKeyClick(Qt::Key_Down);
                }
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
                sendKeyClick(Qt::Key_Left);
                break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
                sendKeyClick(Qt::Key_Right);
                break;
            case SDL_CONTROLLER_BUTTON_A:
                if (m_UiNavMode) {
                    sendKeyClick(Qt::Key_Space);
                }
                else {
                    sendKeyClick(Qt::Key_Return);
                }
                break;
            case SDL_CONTROLLER_BUTTON_B:
                sendKeyClick(Qt::Key_Escape);
                break;
            case SDL_CONTROLLER_BUTTON_X:
                sendKeyClick(Qt::Key_Menu);
                break;
            case SDL_CONTROLLER_BUTTON_Y:
            case SDL_CONTROLLER_BUTTON_START:
                // HACK: We use this keycode to inform main.qml
                // to show the settings when Key_Menu is handled
                // by the control in focus.
                sendKeyClick(Qt::Key_Hangup);
                break;
            default:
                break;
            }
            break;
        }
        case SDL_CONTROLLERDEVICEADDED:
        {
            SDL_GameController* gc = SDL_GameControllerOpen(event.cdevice.which);
            if (gc != nullptr) {
                // SDL_CONTROLLERDEVICEADDED can be reported multiple times for the same
                // gamepad in rare cases, because SDL doesn't fixup the device index in
                // the SDL_CONTROLLERDEVICEADDED event if an unopened gamepad disappears
                // before we've processed the add event.
                if (!m_Gamepads.contains(gc)) {
                    m_Gamepads.append(gc);
                    prepareGamepad(gc);
                }
                else {
                    // We already have this game controller open
                    SDL_GameControllerClose(gc);
                }
            }
            break;
        }
        case SDL_CONTROLLERDEVICEREMOVED:
            for (qsizetype i = m_Gamepads.size(); i-- > 0;) {
                SDL_GameController* gamepad = m_Gamepads[i];
                if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad)) == event.cdevice.which) {
                    m_Rearm.remove(event.cdevice.which);
                    m_Gamepads.removeAt(i);
                    SDL_GameControllerClose(gamepad);
                }
            }
            break;
        }
    }

    // Handle analog sticks by polling
    for (auto gc : std::as_const(m_Gamepads)) {
        if (!m_Enabled) break;
        refreshInputOwnership();
        if (!m_InputOwnership.active) break;
        short leftX = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        short leftY = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        auto& rearm = m_Rearm[SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc))];
        if (!rearm.axis(SDL_CONTROLLER_AXIS_LEFTX, leftX, false)) leftX = 0;
        if (!rearm.axis(SDL_CONTROLLER_AXIS_LEFTY, leftY, false)) leftY = 0;
        if (SDL_GetTicks() - m_LastAxisNavigationEventTime < AXIS_NAVIGATION_REPEAT_DELAY) {
            // Do nothing
        }
        else if (leftY < -30000) {
            if (m_UiNavMode) {
                // Back-tab
                sendKeyClick(Qt::Key_Tab, Qt::ShiftModifier);
            }
            else {
                sendKeyClick(Qt::Key_Up);
            }

            m_LastAxisNavigationEventTime = SDL_GetTicks();
        }
        else if (leftY > 30000) {
            if (m_UiNavMode) {
                sendKeyClick(Qt::Key_Tab);
            }
            else {
                sendKeyClick(Qt::Key_Down);
            }

            m_LastAxisNavigationEventTime = SDL_GetTicks();
        }
        else if (leftX < -30000) {
            sendKeyClick(Qt::Key_Left);
            m_LastAxisNavigationEventTime = SDL_GetTicks();
        }
        else if (leftX > 30000) {
            sendKeyClick(Qt::Key_Right);
            m_LastAxisNavigationEventTime = SDL_GetTicks();
        }
    }
}

void SdlGamepadKeyNavigation::sendKeyClick(Qt::Key key, Qt::KeyboardModifiers modifiers)
{
    refreshInputOwnership();
    if (!m_InputOwnership.active) return;
    QGuiApplication* app = static_cast<QGuiApplication*>(QGuiApplication::instance());
    QPointer<QWindow> focusWindow = app->focusWindow();
    if (focusWindow != nullptr) {
        // Complete each accepted navigation action synchronously. A controller
        // held across an overlay must not leave a Qt control half-pressed, then
        // activate it on release after the overlay takes ownership.
        QKeyEvent press(QEvent::KeyPress, key, modifiers);
        app->sendEvent(focusWindow, &press);
        if (focusWindow) {
            QKeyEvent release(QEvent::KeyRelease, key, modifiers);
            app->sendEvent(focusWindow, &release);
        }
    }
}

void SdlGamepadKeyNavigation::prepareGamepad(SDL_GameController* gamepad)
{
    auto& rearm = m_Rearm[SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamepad))];
    rearm = {};
    for (unsigned button = 0; button < SDL_CONTROLLER_BUTTON_MAX && button < 32; ++button) {
        if (SDL_GameControllerGetButton(gamepad, SDL_GameControllerButton(button))) {
            rearm.buttons |= uint32_t(1) << button;
        }
    }
    for (unsigned axis = 0; axis <= SDL_CONTROLLER_AXIS_LEFTY; ++axis) {
        if (abs(SDL_GameControllerGetAxis(gamepad, SDL_GameControllerAxis(axis))) > 8000) {
            rearm.axes |= uint8_t(1) << axis;
        }
    }
}

void SdlGamepadKeyNavigation::updateInputOwnership()
{
    m_InputOwnership.running = m_Enabled;
    m_InputOwnership.focused = m_HasFocus;
    m_InputOwnership.visible = true; // QML passes visible && active as focus.
    if (!m_InputOwnership.update(SDL_GetTicks())) return;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Controller UI ownership: %s (window=%d known=%d pending=%d)",
                m_InputOwnership.active ? "active" : "suspended", m_HasFocus,
                !!(m_GamescopeSnapshot & GamescopeInputFocus::Known),
                !!(m_GamescopeSnapshot & GamescopeInputFocus::Pending));
    SDL_FlushEvents(SDL_CONTROLLERBUTTONDOWN, SDL_CONTROLLERBUTTONUP);
    m_LastAxisNavigationEventTime = 0;
    if (m_InputOwnership.active) {
        for (auto gamepad : std::as_const(m_Gamepads)) prepareGamepad(gamepad);
    }
}

void SdlGamepadKeyNavigation::refreshInputOwnership()
{
    if (m_GamescopeFocus) {
        const uint64_t snapshot = m_GamescopeFocus->snapshot();
        if ((snapshot >> GamescopeInputFocus::GenerationShift) !=
                (m_GamescopeSnapshot >> GamescopeInputFocus::GenerationShift)) {
            m_InputOwnership.compositorAllowed = false;
            updateInputOwnership();
        }
        m_GamescopeSnapshot = snapshot;
        m_InputOwnership.compositorAllowed = GamescopeInputFocus::permits(snapshot);
    }
    updateInputOwnership();
}

void SdlGamepadKeyNavigation::updateTimerState()
{
    if (m_Enabled && m_HasFocus && !m_ObserverInitialized) {
        m_ObserverInitialized = true;
        m_GamescopeFocus = GamescopeInputFocus::createForCurrentProcess(0);
    }
    refreshInputOwnership();
    if (m_PollingTimer->isActive() && (!m_HasFocus || !m_Enabled)) {
        m_PollingTimer->stop();
    }
    else if (!m_PollingTimer->isActive() && m_HasFocus && m_Enabled) {
        // Flush events on the first poll
        m_FirstPoll = true;

        // Poll every 50 ms for a new joystick event
        m_PollingTimer->start(50);
    }
}

void SdlGamepadKeyNavigation::setUiNavMode(bool uiNavMode)
{
    m_UiNavMode = uiNavMode;
}

int SdlGamepadKeyNavigation::getConnectedGamepads()
{
    Q_ASSERT(m_Enabled);

    int count = 0;
    int numJoysticks = SDL_NumJoysticks();
    for (int i = 0; i < numJoysticks; i++) {
        if (SDL_IsGameController(i)) {
            count++;
        }
    }

    return count;
}
