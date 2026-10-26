#pragma once

namespace StreamSwitch {
enum class Decision { Start, Resume, Confirm, Terminate, Refuse };

// Confirmation authorizes one application ID, never whichever game is current
// when an asynchronous host poll completes. The server checks the ID again.
inline Decision decide(int current, int target, int confirmed)
{
    if (current == 0) return Decision::Start;
    if (current == target) return Decision::Resume;
    if (confirmed == 0) return Decision::Confirm;
    return current == confirmed ? Decision::Terminate : Decision::Refuse;
}
}
