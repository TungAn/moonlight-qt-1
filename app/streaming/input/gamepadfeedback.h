#pragma once

#include <stddef.h>
#include <stdint.h>

namespace GamepadFeedback {
// Feedback controller numbers come from the host. The bound is taken from the
// slot array itself, so out-of-range numbers never index it.
template <typename State, size_t N>
inline decltype(State::controller) slotController(State (&states)[N], uint16_t controllerNumber)
{
    return controllerNumber < N ? states[controllerNumber].controller : nullptr;
}
}
