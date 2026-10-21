#pragma once

#include <cstdint>
#include <cstdlib>

// Caller-thread policy. SDL's background-event hint controls event delivery,
// not entitlement to transmit input. Keep that decision here.
struct ControllerInputOwnership
{
    bool running = false;
    bool focused = false;
    bool visible = false;
    bool minimized = false;
    bool backgroundAllowed = false;
    bool gamingMode = false;
    bool compositorAllowed = true;
    bool active = false;
    uint32_t resumeTime = 0;

    bool update(uint32_t now)
    {
        const bool eligible = running && compositorAllowed &&
            ((backgroundAllowed && !gamingMode) || (focused && visible && !minimized));
        if (eligible == active) {
            return false;
        }
        active = eligible;
        if (active) {
            resumeTime = now;
        }
        return true;
    }

    bool accepts(uint32_t timestamp) const
    {
        // SDL timestamps wrap. Discard the boundary millisecond too: input
        // queued before a resume must never be replayed as fresh input.
        return active && int32_t(timestamp - resumeTime) > 0;
    }
};

struct ControllerInputRearm
{
    uint32_t buttons = 0;
    uint8_t axes = 0;

    bool button(unsigned button, bool pressed)
    {
        if (button >= 32) {
            return false;
        }
        const uint32_t bit = uint32_t(1) << button;
        if (!(buttons & bit)) {
            return true;
        }
        if (!pressed) {
            buttons &= ~bit;
        }
        return false;
    }

    bool axis(unsigned axis, int value, bool trigger)
    {
        if (axis >= 8) {
            return false;
        }
        const uint8_t bit = uint8_t(1) << axis;
        if (!(axes & bit)) {
            return true;
        }
        // A held stick/trigger must return to rest before a fresh movement.
        if (trigger ? value <= 1024 : std::abs(value) <= 8000) {
            axes &= ~bit;
        }
        return false;
    }
};
