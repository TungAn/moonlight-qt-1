#pragma once

#include "vrr/vrrtimingcontroller.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace Vrr {

struct PrepareResult {
    bool prepared = false;
    // All GPU reads of the payload finished: the worker releases it before
    // waiting for the target instead of holding a decoder surface.
    bool sourceReusable = false;
};

struct PresentResult {
    bool presented = false;
};

// Renderer side of VRR presentation. Payloads are opaque to the worker (a
// decoded AVFrame, or a compressed PyroWave frame that preparation decodes).
// Calls other than vrrRelease() arrive on the worker thread only.
class IFramePresenter {
public:
    virtual ~IFramePresenter() = default;
    // Render `payload` into an image ready to present; GPU work should be
    // complete (or its completion observed) when this returns.
    virtual PrepareResult vrrPrepare(void* payload, bool latched) = 0;
    // Show the prepared image now.
    virtual PresentResult vrrPresent(bool latched) = 0;
    // Abandon the prepared image.
    virtual void vrrCancel() = 0;
    // Return a payload the worker no longer needs. Any thread.
    virtual void vrrRelease(void* payload) = 0;
    // Worker thread entry and exit, e.g. for per-thread renderer cleanup.
    virtual void vrrThreadStarted() {}
    virtual void vrrThreadStopping() {}
};

// Owns VRR presentation of one stream: a bounded queue of frames, the timing
// controller, and the thread that prepares and presents each frame at its
// target. It never becomes a video buffer: at most kQueuedFrames wait, a full
// queue evicts its oldest frame, and stale frames are replaced by newer ones.
class PacingWorker {
public:
    using ClockFn = uint64_t (*)();
    // Reports a stop that has waited `waitedUs` for the worker thread, which
    // is still inside `call` ("prepare", "present", "thread exit" or "wait").
    using StopStallFn = void (*)(const char* call, uint64_t waitedUs);
    // Recoverable WSI and GPU waits return well within this bound.
    static constexpr uint64_t kStopStallUs = 2000000;

    struct Stats {
        TimingController::Stats timing;
        uint64_t submitted = 0;
        uint64_t presented = 0;
        uint64_t cancelled = 0;
        uint64_t failedPreparations = 0;
        uint64_t queueDrops = 0;
        uint64_t staleDrops = 0;
        uint32_t queueDepth = 0;
        uint32_t maxQueueDepth = 0;
        // Sums for averages over `presented`.
        uint64_t totalArrivalToPresentUs = 0;
        uint64_t totalPreparationUs = 0;
        uint64_t totalPresentCallUs = 0;
        // Absolute deviation of submit times from their targets.
        uint64_t totalTargetErrorUs = 0;
        uint64_t maxTargetErrorUs = 0;
        // Presented interval distribution against the intended source
        // interval: |actual - intended| above 2 ms.
        uint64_t intervalJerks = 0;
        uint64_t intervals = 0;
    };

    PacingWorker(const SessionConfig& config, IFramePresenter* presenter, ClockFn clock);
    ~PacingWorker();

    PacingWorker(const PacingWorker&) = delete;
    PacingWorker& operator=(const PacingWorker&) = delete;

    bool start(std::function<void()> threadSetup = nullptr);
    // Stop the thread and release every queued payload. Idempotent. Stop
    // wakes every wait of the worker itself, but a presenter call (a
    // swapchain acquire, a GPU wait) cannot be interrupted; it is joined, not
    // abandoned, because it uses renderer state. A join that takes longer
    // than `afterUs` is reported to `fn` and then still awaited.
    void stop();
    void setStopStallReporter(StopStallFn fn, uint64_t afterUs = kStopStallUs);

    // Hand a frame to the worker; it owns the payload from here on.
    void submit(const FrameTiming& timing, void* payload);
    // Drop waiting frames (for example after a decoder flush).
    void flush();

    Stats stats() const;
    const SessionConfig& config() const { return m_Config; }

private:
    struct Entry {
        FrameTiming timing;
        void* payload = nullptr;
    };

    void run();
    bool popNext(Entry& entry);
    bool waitUntil(uint64_t deadlineUs);
    void release(Entry& entry);
    void recordInterval(uint64_t submitUs, uint64_t intendedUs);

    enum class Call : uint8_t {
        Wait,
        Prepare,
        Present,
        ThreadExit,
    };
    static const char* callName(Call call);

    const SessionConfig m_Config;
    IFramePresenter* const m_Presenter;
    const ClockFn m_Clock;
    TimingController m_Controller;

    mutable std::mutex m_Lock;
    std::condition_variable m_Wake;
    std::array<Entry, kQueuedFrames> m_Queue{};
    size_t m_Head = 0;
    size_t m_Count = 0;
    bool m_Stopping = false;
    bool m_Started = false;
    bool m_Exited = false;
    std::atomic<Call> m_Call{Call::Wait};
    StopStallFn m_StopStall = nullptr;
    uint64_t m_StopStallUs = kStopStallUs;
    std::thread m_Thread;
    std::function<void()> m_ThreadSetup;

    Stats m_Stats;
    uint64_t m_LastSubmitUs = 0;
    uint64_t m_LastIntendedUs = 0;
    int64_t m_LastFrameNumber = -1;
};

}
