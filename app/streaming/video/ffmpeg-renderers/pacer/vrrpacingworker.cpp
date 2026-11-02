#include "vrrpacingworker.h"

#include <algorithm>
#include <chrono>
#include <system_error>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define VRR_CPU_RELAX() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define VRR_CPU_RELAX() __asm__ __volatile__("yield")
#else
#define VRR_CPU_RELAX() std::this_thread::yield()
#endif

namespace Vrr {

namespace {

// Sleep until this close to a deadline, then poll: an OS sleep can overshoot
// by tens of microseconds, and the remaining poll is bounded and short.
constexpr uint64_t kCoarseWakeUs = 1000;
constexpr uint64_t kFineWakeUs = 150;
constexpr uint64_t kJerkUs = 2000;

}

PacingWorker::PacingWorker(const SessionConfig& config, IFramePresenter* presenter, ClockFn clock) :
    m_Config(config),
    m_Presenter(presenter),
    m_Clock(clock),
    m_Controller(config)
{
}

PacingWorker::~PacingWorker()
{
    stop();
}

bool PacingWorker::start(std::function<void()> threadSetup)
{
    std::lock_guard<std::mutex> guard(m_Lock);
    if (m_Started) {
        return true;
    }
    m_ThreadSetup = std::move(threadSetup);
    try {
        m_Thread = std::thread(&PacingWorker::run, this);
    }
    catch (const std::system_error&) {
        return false;
    }
    m_Started = true;
    return true;
}

void PacingWorker::stop()
{
    {
        std::lock_guard<std::mutex> guard(m_Lock);
        m_Stopping = true;
    }
    m_Wake.notify_all();
    if (m_Thread.joinable()) {
        if (m_StopStall) {
            std::unique_lock<std::mutex> lock(m_Lock);
            if (!m_Wake.wait_for(lock, std::chrono::microseconds(m_StopStallUs), [this] { return m_Exited; })) {
                const Call call = m_Call.load();
                lock.unlock();
                m_StopStall(callName(call), m_StopStallUs);
            }
        }
        m_Thread.join();
    }
    flush();
}

void PacingWorker::setStopStallReporter(StopStallFn fn, uint64_t afterUs)
{
    m_StopStall = fn;
    m_StopStallUs = afterUs;
}

const char* PacingWorker::callName(Call call)
{
    switch (call) {
    case Call::Wait:
        return "wait";
    case Call::Prepare:
        return "prepare";
    case Call::Present:
        return "present";
    case Call::ThreadExit:
        return "thread exit";
    }
    return "unknown";
}

void PacingWorker::submit(const FrameTiming& timing, void* payload)
{
    Entry evicted;
    bool rejected = false;
    {
        std::lock_guard<std::mutex> guard(m_Lock);
        if (m_Stopping || !m_Started) {
            rejected = true;
        }
        else {
            if (m_Count == m_Queue.size()) {
                // Sustained pressure sheds the oldest waiting frame instead
                // of accumulating latency.
                evicted = m_Queue[m_Head];
                m_Queue[m_Head] = Entry{};
                m_Head = (m_Head + 1) % m_Queue.size();
                --m_Count;
                ++m_Stats.queueDrops;
            }
            m_Queue[(m_Head + m_Count) % m_Queue.size()] = Entry{timing, payload};
            ++m_Count;
            ++m_Stats.submitted;
            m_Stats.queueDepth = uint32_t(m_Count);
            m_Stats.maxQueueDepth = (std::max)(m_Stats.maxQueueDepth, uint32_t(m_Count));
        }
    }
    if (rejected) {
        m_Presenter->vrrRelease(payload);
        return;
    }
    m_Wake.notify_one();
    if (evicted.payload) {
        m_Presenter->vrrRelease(evicted.payload);
    }
}

void PacingWorker::flush()
{
    std::array<Entry, kQueuedFrames> dropped{};
    size_t count = 0;
    {
        std::lock_guard<std::mutex> guard(m_Lock);
        while (m_Count) {
            dropped[count++] = m_Queue[m_Head];
            m_Queue[m_Head] = Entry{};
            m_Head = (m_Head + 1) % m_Queue.size();
            --m_Count;
        }
        m_Stats.queueDepth = 0;
    }
    for (size_t i = 0; i < count; ++i) {
        release(dropped[i]);
    }
}

PacingWorker::Stats PacingWorker::stats() const
{
    std::lock_guard<std::mutex> guard(m_Lock);
    return m_Stats;
}

void PacingWorker::release(Entry& entry)
{
    if (entry.payload) {
        m_Presenter->vrrRelease(entry.payload);
        entry.payload = nullptr;
    }
}

bool PacingWorker::popNext(Entry& entry)
{
    std::array<Entry, kQueuedFrames> stale{};
    size_t staleCount = 0;
    {
        std::unique_lock<std::mutex> lock(m_Lock);
        m_Wake.wait(lock, [this] { return m_Stopping || m_Count != 0; });
        if (m_Stopping) {
            return false;
        }
        // Replace queued frames that are too old to be useful while a newer
        // frame waits behind them. The sole frame is always kept: after a
        // host stall it is still the newest image.
        const uint64_t now = m_Clock();
        while (m_Count > 1) {
            const Entry& front = m_Queue[m_Head];
            const Entry& next = m_Queue[(m_Head + 1) % m_Queue.size()];
            uint64_t successorIntervalUs = 0;
            if (front.timing.timestampValid && next.timing.timestampValid &&
                next.timing.frameNumber == front.timing.frameNumber + 1) {
                const uint32_t ticks = next.timing.rtpTimestamp - front.timing.rtpTimestamp;
                if (ticks && ticks <= kRtpClockHz) {
                    successorIntervalUs = (uint64_t(ticks) * kMicrosecondsPerSecond + kRtpClockHz - 1) / kRtpClockHz;
                }
            }
            const uint64_t age = now > front.timing.readyUs ? now - front.timing.readyUs : 0;
            if (age <= m_Controller.staleHorizonUs(successorIntervalUs)) {
                break;
            }
            stale[staleCount++] = front;
            m_Queue[m_Head] = Entry{};
            m_Head = (m_Head + 1) % m_Queue.size();
            --m_Count;
            ++m_Stats.staleDrops;
        }
        entry = m_Queue[m_Head];
        m_Queue[m_Head] = Entry{};
        m_Head = (m_Head + 1) % m_Queue.size();
        --m_Count;
        m_Stats.queueDepth = uint32_t(m_Count);
    }
    for (size_t i = 0; i < staleCount; ++i) {
        release(stale[i]);
    }
    return true;
}

bool PacingWorker::waitUntil(uint64_t deadlineUs)
{
    for (;;) {
        const uint64_t now = m_Clock();
        if (now >= deadlineUs) {
            return true;
        }
        const uint64_t remaining = deadlineUs - now;
        if (remaining > kCoarseWakeUs + 500) {
            std::unique_lock<std::mutex> lock(m_Lock);
            if (m_Wake.wait_for(lock, std::chrono::microseconds(remaining - kCoarseWakeUs),
                                [this] { return m_Stopping; })) {
                return false;
            }
            continue;
        }
        {
            std::lock_guard<std::mutex> guard(m_Lock);
            if (m_Stopping) {
                return false;
            }
        }
        if (remaining > kFineWakeUs + 50) {
            std::this_thread::sleep_for(std::chrono::microseconds(remaining - kFineWakeUs));
            continue;
        }
        while (m_Clock() < deadlineUs) {
            VRR_CPU_RELAX();
        }
        return true;
    }
}

void PacingWorker::recordInterval(uint64_t submitUs, uint64_t intendedUs)
{
    if (m_LastSubmitUs && submitUs > m_LastSubmitUs && intendedUs > m_LastIntendedUs) {
        const uint64_t actual = submitUs - m_LastSubmitUs;
        const uint64_t intended = intendedUs - m_LastIntendedUs;
        const uint64_t error = (std::max)(actual, intended) - (std::min)(actual, intended);
        ++m_Stats.intervals;
        m_Stats.intervalJerks += error > kJerkUs ? 1 : 0;
    }
    m_LastSubmitUs = submitUs;
    m_LastIntendedUs = intendedUs;
}

void PacingWorker::run()
{
    if (m_ThreadSetup) {
        m_ThreadSetup();
    }
    m_Presenter->vrrThreadStarted();

    Entry entry;
    while (popNext(entry)) {
        Decision decision = m_Controller.schedule(entry.timing, m_Clock());

        if (!waitUntil(decision.renderStartUs)) {
            release(entry);
            break;
        }
        const uint64_t preparationStart = m_Clock();
        m_Call.store(Call::Prepare, std::memory_order_relaxed);
        const PrepareResult prepared = m_Presenter->vrrPrepare(entry.payload, decision.latched);
        m_Call.store(Call::Wait, std::memory_order_relaxed);
        const uint64_t preparationEnd = m_Clock();
        if (prepared.sourceReusable || !prepared.prepared) {
            release(entry);
        }
        if (!prepared.prepared) {
            Submission failed;
            failed.cancelled = true;
            m_Controller.noteSubmission(failed);
            std::lock_guard<std::mutex> guard(m_Lock);
            ++m_Stats.failedPreparations;
            continue;
        }

        // A prepared frame is always presented, even late: replacing it would
        // spend another preparation on the next frame, which then arrives
        // later still. Stale frames are skipped before preparation instead.

        if (!waitUntil(decision.targetUs)) {
            m_Presenter->vrrCancel();
            release(entry);
            break;
        }
        // The spacing floor is re-read: a native latch needs none.
        const uint64_t floorUs = m_Controller.earliestSubmissionUs();
        if (floorUs > m_Clock() && !waitUntil(floorUs)) {
            m_Presenter->vrrCancel();
            release(entry);
            break;
        }

        const uint64_t submitUs = m_Clock();
        m_Call.store(Call::Present, std::memory_order_relaxed);
        const PresentResult presented = m_Presenter->vrrPresent(decision.latched);
        m_Call.store(Call::Wait, std::memory_order_relaxed);
        const uint64_t presentEnd = m_Clock();
        release(entry);

        Submission submission;
        submission.presented = presented.presented;
        submission.cancelled = !presented.presented;
        submission.submitUs = submitUs;
        submission.preparationUs = std::max<uint64_t>(1, preparationEnd - preparationStart);
        submission.readyUs = preparationEnd;
        m_Controller.noteSubmission(submission);

        const TimingController::Stats timing = m_Controller.stats();
        std::lock_guard<std::mutex> guard(m_Lock);
        m_Stats.timing = timing;
        if (presented.presented) {
            ++m_Stats.presented;
            m_Stats.totalArrivalToPresentUs += submitUs > entry.timing.readyUs ? submitUs - entry.timing.readyUs : 0;
            m_Stats.totalPreparationUs += preparationEnd - preparationStart;
            m_Stats.totalPresentCallUs += presentEnd - submitUs;
            const uint64_t error = submitUs > decision.targetUs ? submitUs - decision.targetUs : decision.targetUs - submitUs;
            m_Stats.totalTargetErrorUs += error;
            m_Stats.maxTargetErrorUs = (std::max)(m_Stats.maxTargetErrorUs, error);
            if (entry.timing.frameNumber >= 0 && entry.timing.frameNumber != m_LastFrameNumber + 1) {
                m_LastSubmitUs = 0;
            }
            m_LastFrameNumber = entry.timing.frameNumber;
            recordInterval(submitUs, uint64_t(int64_t(decision.sourceTimeUs) + decision.smoothingUs));
        }
        else {
            ++m_Stats.cancelled;
            m_LastSubmitUs = 0;
        }
    }

    m_Call.store(Call::ThreadExit, std::memory_order_relaxed);
    m_Presenter->vrrThreadStopping();
    {
        std::lock_guard<std::mutex> guard(m_Lock);
        m_Exited = true;
    }
    m_Wake.notify_all();
}

}
