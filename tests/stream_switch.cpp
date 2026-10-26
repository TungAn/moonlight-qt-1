#include "cli/streamswitchpolicy.h"
#include <cassert>

int main()
{
    using namespace StreamSwitch;
    assert(decide(0, 42, 0) == Decision::Start);
    assert(decide(42, 42, 0) == Decision::Resume);
    assert(decide(41, 42, 0) == Decision::Confirm);
    assert(decide(41, 42, 41) == Decision::Terminate);
    assert(decide(99, 42, 41) == Decision::Refuse);
    // A game ending or being started by another client during confirmation
    // cannot authorize terminating a replacement session.
    assert(decide(0, 42, 41) == Decision::Start);
    assert(decide(42, 42, 41) == Decision::Resume);
}
