// Threaded tests of the VRR pacing worker with a fake presenter.
#include "streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Vrr;

namespace {

uint64_t nowUs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

struct FakePresenter : IFramePresenter {
    std::mutex lock;
    std::vector<int> presented;
    std::vector<uint64_t> presentTimes;
    std::multiset<int> released;
    std::atomic<int> outstanding{0};
    std::atomic<int> prepareDelayUs{1000};
    bool reusable = true;
    int prepared = -1;

    PrepareResult vrrPrepare(void* payload, bool) override
    {
        std::this_thread::sleep_for(std::chrono::microseconds(prepareDelayUs.load()));
        prepared = int(reinterpret_cast<intptr_t>(payload));
        return {true, reusable};
    }
    PresentResult vrrPresent(bool) override
    {
        std::lock_guard<std::mutex> guard(lock);
        presented.push_back(prepared);
        presentTimes.push_back(nowUs());
        return {true};
    }
    void vrrCancel() override {}
    void vrrRelease(void* payload) override
    {
        std::lock_guard<std::mutex> guard(lock);
        released.insert(int(reinterpret_cast<intptr_t>(payload)));
        --outstanding;
    }
};

SessionConfig config()
{
    SessionConfig c;
    c.displayRefreshHz = 120;
    c.streamRateHz = 120;
    c.latencyMode = 1;
    c.timing = VrrTimingOptions{}.resolved(1);
    c.reduceJudder = false;
    c.protection = PresentProtection::Native;
    return c;
}

FrameTiming timing(int number, uint64_t readyUs)
{
    FrameTiming t;
    t.frameNumber = number;
    t.rtpTimestamp = uint32_t(number * 750);
    t.timestampValid = true;
    t.readyUs = readyUs;
    return t;
}

void submit(PacingWorker& worker, FakePresenter& presenter, int number)
{
    ++presenter.outstanding;
    worker.submit(timing(number, nowUs()), reinterpret_cast<void*>(intptr_t(number)));
}

// A steady 120 FPS stream is presented in order, every payload released once.
void testSteadyStream()
{
    FakePresenter presenter;
    {
        PacingWorker worker(config(), &presenter, nowUs);
        assert(worker.start());
        const auto start = std::chrono::steady_clock::now();
        for (int i = 1; i <= 240; ++i) {
            std::this_thread::sleep_until(start + std::chrono::microseconds(8333 * i));
            submit(worker, presenter, i);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const auto stats = worker.stats();
        assert(stats.maxQueueDepth <= uint32_t(kQueuedFrames));
        assert(stats.presented >= 230);
    }
    assert(presenter.outstanding == 0);
    for (size_t i = 1; i < presenter.presented.size(); ++i) {
        assert(presenter.presented[i] > presenter.presented[i - 1]);
    }
    for (int i = 1; i <= 240; ++i) {
        assert(presenter.released.count(i) == 1);
    }
}

// A stalled presenter cannot make the worker a video buffer: the queue stays
// bounded, old frames are dropped, and stop releases what remains.
void testBoundedUnderPressure()
{
    FakePresenter presenter;
    presenter.prepareDelayUs = 40000;
    PacingWorker worker(config(), &presenter, nowUs);
    assert(worker.start());
    for (int i = 1; i <= 100; ++i) {
        submit(worker, presenter, i);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const auto stats = worker.stats();
    assert(stats.maxQueueDepth <= uint32_t(kQueuedFrames));
    assert(stats.queueDrops + stats.staleDrops >= 80);
    worker.stop();
    assert(presenter.outstanding == 0);
    // Submissions after stop are released immediately.
    submit(worker, presenter, 1000);
    assert(presenter.outstanding == 0);
    std::lock_guard<std::mutex> guard(presenter.lock);
    for (int i = 1; i <= 100; ++i) {
        assert(presenter.released.count(i) == 1);
    }
}

// Presents land on their targets: submit jitter is far below the arrival
// jitter that reaches the worker.
void testPresentsFollowTargets()
{
    FakePresenter presenter;
    PacingWorker worker(config(), &presenter, nowUs);
    assert(worker.start());
    const auto start = std::chrono::steady_clock::now();
    uint64_t seed = 12345;
    for (int i = 1; i <= 360; ++i) {
        seed = seed * 6364136223846793005ULL + 1;
        const int jitter = int((seed >> 33) % 3000);
        std::this_thread::sleep_until(start + std::chrono::microseconds(8333 * i + jitter));
        submit(worker, presenter, i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    worker.stop();
    const auto stats = worker.stats();
    assert(stats.presented > 300);
    const double meanTargetErrorUs = double(stats.totalTargetErrorUs) / double(stats.presented);
    assert(meanTargetErrorUs < 500.0);
    // Interval error above 2 ms is rare once the buffer absorbs the jitter.
    assert(stats.intervalJerks * 20 < stats.intervals);
}

std::atomic<int> g_StallReports{0};
const char* g_StallCall = nullptr;

void recordStall(const char* call, uint64_t)
{
    g_StallCall = call;
    ++g_StallReports;
}

// Presenter whose present blocks like an acquire on a compositor that stopped
// making progress, until the test releases it.
struct BlockingPresenter : FakePresenter {
    std::mutex gate;
    std::condition_variable opened;
    bool open = false;
    std::atomic<bool> blocked{false};

    PresentResult vrrPresent(bool latched) override
    {
        std::unique_lock<std::mutex> lock(gate);
        blocked = true;
        opened.wait(lock, [this] { return open; });
        return FakePresenter::vrrPresent(latched);
    }
};

// Stop cannot interrupt a presenter call: it reports the stall, names the
// blocking call, and still joins the thread (never abandons it) before
// releasing every frame exactly once.
void testStopReportsBlockedPresenter()
{
    BlockingPresenter presenter;
    presenter.prepareDelayUs = 0;
    PacingWorker worker(config(), &presenter, nowUs);
    worker.setStopStallReporter(recordStall, 50000);
    assert(worker.start());
    for (int i = 1; i <= 3; ++i) {
        submit(worker, presenter, i);
    }
    while (!presenter.blocked) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::thread opener([&presenter] {
        while (g_StallReports == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::lock_guard<std::mutex> lock(presenter.gate);
        presenter.open = true;
        presenter.opened.notify_all();
    });
    worker.stop();
    opener.join();
    assert(g_StallReports == 1);
    assert(std::string(g_StallCall) == "present");
    assert(presenter.outstanding == 0);
    std::lock_guard<std::mutex> guard(presenter.lock);
    for (int i = 1; i <= 3; ++i) {
        assert(presenter.released.count(i) == 1);
    }

    // A prompt stop reports nothing.
    FakePresenter idle;
    PacingWorker quick(config(), &idle, nowUs);
    quick.setStopStallReporter(recordStall, 50000);
    assert(quick.start());
    quick.stop();
    assert(g_StallReports == 1);
}

}

int main()
{
    testSteadyStream();
    testBoundedUnderPressure();
    testPresentsFollowTargets();
    testStopReportsBlockedPresenter();
    std::puts("vrr-worker: all tests passed");
    return 0;
}
