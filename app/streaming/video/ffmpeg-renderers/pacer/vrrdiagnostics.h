#pragma once

#include "vrrpacingworker.h"

namespace Vrr {

// Performance-overlay lines for VRR presentation since `last` (updated).
// Returns the number of characters written, 0 if they do not fit.
int formatOverlay(const PacingWorker::Stats& now, PacingWorker::Stats& last, const SessionConfig& config,
                  const char* presentMode, char* output, int length);

// Overlay line for a VRR request that fell back to fixed pacing.
int formatFallback(FallbackReason reason, char* output, int length);

// Logs a worker stop that is waiting on an uninterruptible presenter call
// (PacingWorker::StopStallFn).
void logStopStall(const char* call, uint64_t waitedUs);

// One-line log summary since `last` (updated).
int formatSummary(const char* title, const PacingWorker::Stats& now, PacingWorker::Stats& last,
                  char* output, int length);

}
