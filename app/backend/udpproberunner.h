#pragma once
#include "udpprobe.h"
#include <QHostAddress>
#include <functional>

namespace PyroshineUdpProbe {

enum class StopReason { Completed, Saturated, Cancelled, Silent };

struct RunResult {
    std::vector<std::optional<StepSummary>> steps;
    StopReason stop = StopReason::Completed;
    int receiveBufferBytes = 0, requestedBufferBytes = 0;
};

// Receive one run from `host` (the reply's UDP port). Throws std::runtime_error
// when no probe datagram arrives. `interrupted` is polled to cancel.
RunResult receive(const StartReply& reply, const QHostAddress& host, const std::function<bool()>& interrupted);

} // namespace PyroshineUdpProbe
