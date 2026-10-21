#include "streaming/input/controllerinputstate.h"
#include <cassert>
#include <climits>

int main()
{
    ControllerInputOwnership ownership;
    assert(!ownership.accepts(1));
    ownership.running = ownership.focused = ownership.visible = true;
    assert(ownership.update(10));
    assert(!ownership.accepts(10));
    assert(ownership.accepts(11));
    assert(!ownership.update(11));
    ownership.focused = false;
    assert(ownership.update(20));
    assert(!ownership.accepts(21));
    assert(!ownership.update(22));
    ownership.focused = true;
    assert(ownership.update(30));
    assert(!ownership.accepts(15)); // queued pre-suspension input
    assert(!ownership.accepts(25)); // queued menu input
    assert(ownership.accepts(31));
    ownership.minimized = true;
    assert(ownership.update(35));
    ownership.visible = false;
    assert(!ownership.update(36));
    ownership.minimized = false;
    assert(!ownership.update(40)); // restoring a hidden window is insufficient
    ownership.backgroundAllowed = true;
    assert(ownership.update(50)); // desktop background workflow includes minimized
    ownership.gamingMode = true;
    assert(ownership.update(60)); // Gaming Mode must respect ownership
    assert(!ownership.active);
    ownership.visible = true;
    assert(ownership.update(70));
    ownership.compositorAllowed = false;
    assert(ownership.update(80));
    assert(!ownership.accepts(81)); // Steam menu with SDL focus unchanged
    ownership.compositorAllowed = true;
    assert(ownership.update(UINT_MAX - 2));
    assert(ownership.accepts(1)); // SDL tick wrap
    assert(!ownership.accepts(UINT_MAX - 3));
    ownership.running = false;
    assert(ownership.update(2));
    assert(!ownership.accepts(3));

    ControllerInputRearm rearm;
    rearm.buttons = (1u << 0) | (1u << 31);
    assert(!rearm.button(0, true));
    assert(rearm.button(1, true)); // unrelated controls stay usable
    assert(!rearm.button(0, false));
    assert(rearm.button(0, true));
    assert(!rearm.button(31, true));
    assert(!rearm.button(32, true));
    rearm.axes = (1u << 0) | (1u << 4);
    assert(!rearm.axis(0, -32768, false));
    assert(!rearm.axis(0, 8000, false));
    assert(rearm.axis(0, 32000, false));
    assert(!rearm.axis(4, 32767, true));
    assert(!rearm.axis(4, 1024, true));
    assert(rearm.axis(4, 32767, true));
    assert(!rearm.axis(8, 0, false));
}
