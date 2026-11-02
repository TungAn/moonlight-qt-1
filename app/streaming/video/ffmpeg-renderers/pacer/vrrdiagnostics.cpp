#include "vrrdiagnostics.h"

#include "SDL_compat.h"

#include <cstdio>

namespace Vrr {

namespace {

struct Window {
    uint64_t presented, queueDrops, staleDrops, cancelled, intervals, jerks, lateTargets, latched, floorDelayed, catchUps;
    double meanTargetErrorUs, meanArrivalToPresentUs, meanPreparationUs, meanPresentCallUs;
};

Window window(const PacingWorker::Stats& now, const PacingWorker::Stats& last)
{
    Window w = {};
    w.presented = now.presented - last.presented;
    w.queueDrops = now.queueDrops - last.queueDrops;
    w.staleDrops = now.staleDrops - last.staleDrops;
    w.cancelled = (now.cancelled - last.cancelled) + (now.failedPreparations - last.failedPreparations);
    w.intervals = now.intervals - last.intervals;
    w.jerks = now.intervalJerks - last.intervalJerks;
    w.lateTargets = now.timing.lateTargets - last.timing.lateTargets;
    w.latched = now.timing.latched - last.timing.latched;
    w.floorDelayed = now.timing.floorDelayed - last.timing.floorDelayed;
    w.catchUps = now.timing.catchUps - last.timing.catchUps;
    if (w.presented) {
        w.meanTargetErrorUs = double(now.totalTargetErrorUs - last.totalTargetErrorUs) / double(w.presented);
        w.meanArrivalToPresentUs = double(now.totalArrivalToPresentUs - last.totalArrivalToPresentUs) / double(w.presented);
        w.meanPreparationUs = double(now.totalPreparationUs - last.totalPreparationUs) / double(w.presented);
        w.meanPresentCallUs = double(now.totalPresentCallUs - last.totalPresentCallUs) / double(w.presented);
    }
    return w;
}

double percent(uint64_t part, uint64_t whole)
{
    return whole ? 100.0 * double(part) / double(whole) : 0.0;
}

double sourceFps(const TimingController::Stats& t)
{
    return t.sourcePeriodUs ? 1e6 / double(t.sourcePeriodUs) : 0.0;
}

int checked(int ret, int length)
{
    return ret < 0 || ret >= length ? 0 : ret;
}

}

int formatOverlay(const PacingWorker::Stats& now, PacingWorker::Stats& last, const SessionConfig& config,
                  const char* presentMode, char* output, int length)
{
    const Window w = window(now, last);
    last = now;
    const TimingController::Stats& t = now.timing;
    return checked(snprintf(output, size_t(length),
        // Pacing is active; whether the panel scans out adaptively is not
        // observable from the client, so the overlay never claims it.
        "VRR pacing: active (%s, %d Hz display, %d FPS stream, panel adaptive sync not verified)\n"
        "VRR source: %.2f FPS | buffer %.2f/%.2f ms | queue %u/%d\n"
        "VRR intervals: quality %.2f%% | jerk >2 ms %.1f%% | target error %.2f ms\n"
        "VRR presents: late %.1f%% | latched %.0f%% | drops %llu (stale %llu) | arrival to present %.2f ms\n",
        presentMode, config.displayRefreshHz, config.streamRateHz,
        sourceFps(t), t.playoutDelayUs / 1000.0, t.maximumDelayUs / 1000.0, now.queueDepth, kQueuedFrames,
        t.intervalQualityPercent, percent(w.jerks, w.intervals), w.meanTargetErrorUs / 1000.0,
        percent(w.lateTargets, w.presented), percent(w.latched, w.presented),
        (unsigned long long)(w.queueDrops + w.staleDrops), (unsigned long long)w.staleDrops,
        w.meanArrivalToPresentUs / 1000.0), length);
}

int formatFallback(FallbackReason reason, char* output, int length)
{
    return checked(snprintf(output, size_t(length), "VRR: requested, fixed pacing (%s)\n",
                            fallbackReasonName(reason)), length);
}

void logStopStall(const char* call, uint64_t waitedUs)
{
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "VRR: stopping presentation has waited %.1f s for the worker's %s call; "
                "the window system or GPU driver is not making progress",
                double(waitedUs) / 1e6, call);
}

int formatSummary(const char* title, const PacingWorker::Stats& now, PacingWorker::Stats& last,
                  char* output, int length)
{
    const Window w = window(now, last);
    last = now;
    const TimingController::Stats& t = now.timing;
    return checked(snprintf(output, size_t(length),
        "%s: presented %llu, source %.2f FPS, buffer %.2f ms (requested %.2f, max %.2f, %s), "
        "render lead %.2f ms, judder reserve %.2f ms, interval quality %.2f%%, jerk >2 ms %.2f%%, "
        "mean target error %.3f ms, late %.2f%%, latched %.1f%%, floor-delayed %llu, catch-up %llu, "
        "queue drops %llu, stale drops %llu, cancelled %llu, max queue %u, "
        "arrival to present %.2f ms, preparation %.2f ms, present call %.2f ms, rebases %llu, rate changes %llu, "
        "phase resets %llu",
        title, (unsigned long long)w.presented, sourceFps(t),
        t.playoutDelayUs / 1000.0, t.requestedDelayUs / 1000.0, t.maximumDelayUs / 1000.0,
        IntervalBuffer::actionName(t.bufferAction),
        t.renderLeadUs / 1000.0, t.smoothingReserveUs / 1000.0, t.intervalQualityPercent,
        percent(w.jerks, w.intervals), w.meanTargetErrorUs / 1000.0,
        percent(w.lateTargets, w.presented), percent(w.latched, w.presented),
        (unsigned long long)w.floorDelayed, (unsigned long long)w.catchUps, (unsigned long long)w.queueDrops,
        (unsigned long long)w.staleDrops, (unsigned long long)w.cancelled, now.maxQueueDepth,
        w.meanArrivalToPresentUs / 1000.0, w.meanPreparationUs / 1000.0, w.meanPresentCallUs / 1000.0,
        (unsigned long long)t.rebases, (unsigned long long)t.rateChanges,
        (unsigned long long)t.phaseResets), length);
}

}
