#pragma once
#include <QStringList>
// Only the preferences consumed by the production input-handler constructor.
struct StreamingPreferences {
    enum CaptureSysKeysMode { CSK_OFF, CSK_FULLSCREEN, CSK_ALWAYS };
    bool multiController = true, gamepadMouse = true, swapMouseButtons = false;
    bool reverseScrollDirection = false, swapFaceButtons = false;
    bool absoluteMouseMode = false, absoluteTouchMode = false, backgroundGamepad = false;
    CaptureSysKeysMode captureSysKeysMode = CSK_OFF;
};
