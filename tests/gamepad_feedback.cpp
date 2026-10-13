#ifdef NDEBUG
#undef NDEBUG
#endif
#include "streaming/input/gamepadfeedback.h"
#include <cassert>
#include <memory>

namespace {
struct FakeController { int id; };
struct FakeState { FakeController* controller; };
constexpr size_t Slots = 16; // MAX_GAMEPADS
}

int main()
{
    using GamepadFeedback::slotController;

    // Exactly-sized heap array: ASan reports any read past the last slot.
    std::unique_ptr<FakeState[][Slots]> storage(new FakeState[1][Slots]());
    FakeState (&states)[Slots] = storage[0];

    // Absent controllers in range are reported as such.
    assert(slotController(states, 0) == nullptr);
    assert(slotController(states, Slots - 1) == nullptr);

    FakeController first{0}, last{15};
    states[0].controller = &first;
    states[Slots - 1].controller = &last;
    assert(slotController(states, 0) == &first);
    assert(slotController(states, Slots - 1) == &last);

    // Host-supplied numbers at or beyond the slot count never index the array.
    const uint16_t invalid[] = { Slots, Slots + 1, 0x7fff, 0xffff };
    for (uint16_t number : invalid) {
        assert(slotController(states, number) == nullptr);
    }
    return 0;
}
