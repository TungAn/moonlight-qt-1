#pragma once

#include <QTimer>
#include <QEvent>
#include <QHash>

#include "SDL_compat.h"

#include "settings/streamingpreferences.h"
#include "streaming/input/controllerinputstate.h"
#include "streaming/input/gamescopeinputfocus.h"

class SdlGamepadKeyNavigation : public QObject
{
    Q_OBJECT

public:
    SdlGamepadKeyNavigation(StreamingPreferences* prefs);

    ~SdlGamepadKeyNavigation();

    Q_INVOKABLE void enable();

    Q_INVOKABLE void disable();

    Q_INVOKABLE void notifyWindowFocus(bool hasFocus);

    Q_INVOKABLE void setUiNavMode(bool settingsMode);

    Q_INVOKABLE int getConnectedGamepads();

private:
    friend class GamepadNavigationTest;
    void sendKeyClick(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier);

    void refreshInputOwnership();
    void updateInputOwnership();
    void prepareGamepad(SDL_GameController* gamepad);

    void updateTimerState();

private slots:
    void onPollingTimerFired();

private:
    StreamingPreferences* m_Prefs;
    QTimer* m_PollingTimer;
    QList<SDL_GameController*> m_Gamepads;
    bool m_Enabled;
    bool m_UiNavMode;
    bool m_FirstPoll;
    bool m_HasFocus;
    Uint32 m_LastAxisNavigationEventTime;
    bool m_ObserverInitialized;
    uint64_t m_GamescopeSnapshot;
    std::unique_ptr<GamescopeInputFocus> m_GamescopeFocus;
    ControllerInputOwnership m_InputOwnership;
    QHash<SDL_JoystickID, ControllerInputRearm> m_Rearm;
};
