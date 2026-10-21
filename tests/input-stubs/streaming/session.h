#pragma once
#include <Limelight.h>
#include "streaming/input/input.h"
struct Overlay { enum { OverlayDebug }; };
struct TestOverlayManager {
    void setOverlayState(int, bool) {}
    bool isOverlayEnabled(int) { return false; }
};
struct Session {
    static Session* get() { static Session instance; return &instance; }
    void notifyMouseEmulationMode(bool) {}
    TestOverlayManager& getOverlayManager() { return overlay; }
    TestOverlayManager overlay;
};
