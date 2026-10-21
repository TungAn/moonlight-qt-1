#pragma once

#include "SDL_compat.h"
#include <memory>
#include <string>
#include <vector>

// Gamescope publishes input ownership on its root Xwayland server, independently
// of SDL's per-window focus. The observer never touches controllers or Limelight.
class GamescopeInputFocus
{
public:
    static constexpr uint64_t Known = 1;
    static constexpr uint64_t Allowed = 2;
    static constexpr uint64_t Pending = 4;
    static constexpr unsigned GenerationShift = 3;

    GamescopeInputFocus(Uint32 wakeEvent, uint32_t appId,
                       const std::vector<std::string>& displays);
    ~GamescopeInputFocus();

    // Pass zero for polling callers (Qt menus), or an SDL wake event (streaming).
    static std::unique_ptr<GamescopeInputFocus> createForCurrentProcess(Uint32 wakeEvent);

    // One atomic snapshot includes every suspension, even an open/close burst
    // completed before the session thread gets to process the wake-up event.
    uint64_t snapshot();
    static bool permits(uint64_t state)
    {
        return (state & Allowed) || !(state & (Known | Pending));
    }

private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};
