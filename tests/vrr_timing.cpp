// Deterministic tests of the VRR rate policy and timing controller.
#include "settings/vrrtimingoptions.h"
#include "streaming/vrrratepolicy.h"
#include "streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace Vrr;

namespace {

struct Rng {
    uint64_t state = 0x9e3779b97f4a7c15ULL;
    // Uniform in [0, range).
    uint64_t next(uint64_t range)
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return range ? state % range : 0;
    }
};

SessionConfig config(int refreshHz, int streamHz, int mode = 1, bool reduceJudder = false,
                     PresentProtection protection = PresentProtection::Native)
{
    SessionConfig c;
    c.displayRefreshHz = refreshHz;
    c.streamRateHz = streamHz;
    c.latencyMode = mode;
    c.timing = VrrTimingOptions{}.resolved(mode);
    c.reduceJudder = reduceJudder;
    c.protection = protection;
    return c;
}

// Models the pacing worker: frames are scheduled in arrival order once the
// previous frame was presented, prepared for `preparationUs`, then submitted
// at the later of their target and the spacing floor. Queued frames past the
// stale horizon are replaced when a newer one waits, as the worker does.
struct Sim {
    explicit Sim(const SessionConfig& c) : controller(c) {}

    struct Frame {
        int64_t number;
        uint32_t rtp;
        uint64_t arrivalUs;
        uint64_t preparationUs = 1000;
        uint32_t lostPackets = 0;
    };

    struct Present {
        int64_t number;
        uint64_t submitUs;
        uint64_t arrivalUs;
        Decision decision;
    };

    void run(const std::vector<Frame>& frames)
    {
        for (size_t i = 0; i < frames.size(); ++i) {
            const Frame& f = frames[i];
            now = std::max(now, f.arrivalUs);
            if (i + 1 < frames.size() && frames[i + 1].arrivalUs <= now &&
                now - f.arrivalUs > controller.staleHorizonUs(0)) {
                ++staleDrops;
                continue;
            }
            FrameTiming timing;
            timing.frameNumber = f.number;
            timing.rtpTimestamp = f.rtp;
            timing.timestampValid = true;
            timing.readyUs = f.arrivalUs;
            timing.receiveUs = f.arrivalUs;
            timing.lostPackets = f.lostPackets;
            const Decision d = controller.schedule(timing, now);
            const uint64_t start = std::max(now, d.renderStartUs);
            const uint64_t ready = start + f.preparationUs;
            uint64_t submit = std::max(ready, d.targetUs);
            submit = std::max(submit, controller.earliestSubmissionUs());
            Submission s;
            s.presented = true;
            s.submitUs = submit;
            s.preparationUs = f.preparationUs;
            s.readyUs = ready;
            controller.noteSubmission(s);
            presents.push_back({f.number, submit, f.arrivalUs, d});
            now = submit + presentCallUs;
        }
    }

    // Presented intervals between adjacent frames from index `from`.
    std::vector<double> intervalsMs(size_t from = 0) const
    {
        std::vector<double> out;
        for (size_t i = std::max<size_t>(from, 1); i < presents.size(); ++i) {
            if (presents[i].number == presents[i - 1].number + 1) {
                out.push_back(double(presents[i].submitUs - presents[i - 1].submitUs) / 1000.0);
            }
        }
        return out;
    }

    TimingController controller;
    uint64_t now = 1000000;
    std::vector<Present> presents;
    uint64_t staleDrops = 0;
    uint64_t presentCallUs = 50;
};

double stdev(const std::vector<double>& v)
{
    double mean = 0;
    for (double x : v) mean += x;
    mean /= double(v.size());
    double var = 0;
    for (double x : v) var += (x - mean) * (x - mean);
    return std::sqrt(var / double(v.size()));
}

double minimum(const std::vector<double>& v)
{
    double m = 1e9;
    for (double x : v) m = std::min(m, x);
    return m;
}

uint32_t rtpAt(uint64_t sourceUs)
{
    return uint32_t((sourceUs * 90000 + 500000) / 1000000);
}

// A source at `periodUs` whose frames arrive after a base latency plus
// uniform jitter.
std::vector<Sim::Frame> stream(int count, uint64_t periodUs, uint64_t jitterUs, uint64_t baseUs = 5000,
                               uint64_t startUs = 1000000, uint32_t rtpBase = 123456)
{
    Rng rng;
    std::vector<Sim::Frame> frames;
    for (int i = 0; i < count; ++i) {
        const uint64_t source = uint64_t(i) * periodUs;
        Sim::Frame f;
        f.number = i + 1;
        f.rtp = rtpBase + rtpAt(source);
        f.arrivalUs = startUs + source + baseUs + rng.next(jitterUs + 1);
        frames.push_back(f);
    }
    // Arrivals are in order.
    for (size_t i = 1; i < frames.size(); ++i) {
        frames[i].arrivalUs = std::max(frames[i].arrivalUs, frames[i - 1].arrivalUs + 1);
    }
    return frames;
}

void testRatePolicy()
{
    assert(VrrRatePolicy::vrrRateForRefresh(120) == 116);
    assert(VrrRatePolicy::vrrRateForRefresh(144) == 138);
    assert(VrrRatePolicy::vrrRateForRefresh(165) == 157);
    assert(VrrRatePolicy::vrrRateForRefresh(240) == 224);
    assert(VrrRatePolicy::vrrRateForRefresh(60) == 59);
    assert(VrrRatePolicy::lowLatencyRateForRefresh(120) == 100);
    assert(VrrRatePolicy::lowLatencyRateForRefresh(144) == 120);
    assert(VrrRatePolicy::lowLatencyRateForRefresh(165) == 135);
    assert(VrrRatePolicy::vrrRateForRefresh(0) == 0);
    assert(VrrRatePolicy::lowLatencyRateForRefresh(1) == 0);
    assert(VrrRatePolicy::hasAdaptiveHeadroom(120, 120));
    assert(!VrrRatePolicy::hasAdaptiveHeadroom(121, 120));
    assert(!VrrRatePolicy::hasAdaptiveHeadroom(60, 0));

    // VRR disabled: fixed and native rates plus the saved custom value.
    auto plain = VrrRatePolicy::buildChoices({144, 60, 0}, 75, false);
    std::vector<int> rates;
    for (const auto& c : plain) rates.push_back(c.fps);
    assert((rates == std::vector<int>{30, 60, 75, 144}));
    assert(plain[2].kind == VrrFpsChoiceKind::Custom);

    // VRR enabled: calculated rates join, a native rate keeps its role, and a
    // calculated rate equal to another display's native rate stays native.
    auto vrr = VrrRatePolicy::buildChoices({144, 120}, 120, true);
    rates.clear();
    for (const auto& c : vrr) rates.push_back(c.fps);
    assert((rates == std::vector<int>{30, 60, 100, 116, 120, 138, 144}));
    for (const auto& c : vrr) {
        if (c.fps == 120 || c.fps == 144) assert(c.kind == VrrFpsChoiceKind::Fixed);
        if (c.fps == 116 || c.fps == 138) assert(c.kind == VrrFpsChoiceKind::Vrr);
        if (c.fps == 100) assert(c.kind == VrrFpsChoiceKind::LowLatencyVrr);
    }
}

void testTimingOptions()
{
    const VrrTimingOptions smooth = VrrTimingOptions::preset(0);
    assert(smooth.bufferPerMille == 4000 && smooth.targetHundredths == 9999 &&
           smooth.historySeconds == 300 && smooth.toleranceUs == 250);
    assert(VrrTimingOptions{}.resolved(2) == VrrTimingOptions::preset(2));
    const VrrTimingOptions custom = VrrTimingOptions{100, 1, 1000, 1310}.resolved(1);
    assert(custom.bufferPerMille == 250 && custom.targetHundredths == 9000 &&
           custom.historySeconds == 300 && custom.toleranceUs == 1250);
}

// Decode-completion jitter must not reach the display: a 60 FPS source with
// 4 ms of arrival jitter presents at even intervals.
void testJitterDoesNotReachTheDisplay()
{
    Sim sim(config(120, 60));
    sim.run(stream(1200, 16667, 4000));
    const auto intervals = sim.intervalsMs(200);
    assert(intervals.size() > 900);
    assert(stdev(intervals) < 0.25);
    assert(sim.staleDrops == 0);
    const auto stats = sim.controller.stats();
    assert(stats.playoutDelayUs <= stats.maximumDelayUs);
    // Present-on-arrival would have kept the jitter.
    std::vector<double> arrivals;
    const auto frames = stream(1200, 16667, 4000);
    for (size_t i = 201; i < frames.size(); ++i)
        arrivals.push_back(double(frames[i].arrivalUs - frames[i - 1].arrivalUs) / 1000.0);
    assert(stdev(arrivals) > 1.0);
}

// The buffer grows for jitter beyond its start delay (bounded by the preset)
// and releases slowly once delivery is clean again.
void testBufferGrowsAndReleases()
{
    Sim sim(config(120, 120, 1));
    auto frames = stream(120 * 20, 8333, 12000);
    sim.run(frames);
    const auto grown = sim.controller.stats();
    assert(grown.playoutDelayUs <= grown.maximumDelayUs);
    assert(grown.maximumDelayUs <= 8400);
    const uint64_t peak = grown.playoutDelayUs;
    assert(peak > 4000);

    // Clean delivery for 40 s: release below the peak, never below 1 ms.
    const uint64_t start = frames.back().arrivalUs + 8333;
    auto clean = stream(120 * 40, 8333, 0, 5000, start - 5000, frames.back().rtp + 750);
    for (auto& f : clean) f.number += frames.back().number;
    sim.run(clean);
    const auto released = sim.controller.stats();
    assert(released.playoutDelayUs < peak);
    assert(released.playoutDelayUs >= 1000);
}

// Lowest latency never buffers more than half a source frame.
void testLowLatencyBound()
{
    Sim sim(config(120, 60, 2));
    sim.run(stream(60 * 20, 16667, 14000));
    const auto stats = sim.controller.stats();
    assert(stats.maximumDelayUs <= 16667 / 2 + 1);
    assert(stats.playoutDelayUs <= stats.maximumDelayUs);
}

// One late frame: it is shown when ready, following frames return to their
// own slots without a burst of compressed presents, and the delay is not
// inflated permanently.
void testLateFrameRecoveryWithoutBurst()
{
    const uint64_t displayUs = 8333;
    for (PresentProtection protection : {PresentProtection::Native, PresentProtection::SoftwareFloor}) {
        Sim sim(config(120, 60, 1, false, protection));
        auto frames = stream(600, 16667, 500);
        frames[300].arrivalUs += 14000;
        for (size_t i = 301; i < frames.size(); ++i)
            frames[i].arrivalUs = std::max(frames[i].arrivalUs, frames[i - 1].arrivalUs + 1);
        sim.run(frames);
        for (size_t i = 1; i < sim.presents.size(); ++i) {
            const uint64_t gap = sim.presents[i].submitUs - sim.presents[i - 1].submitUs;
            // No two presents inside one display period, latched or floored.
            assert(gap + 1 >= displayUs || sim.presents[i].decision.latched);
        }
        const auto intervals = sim.intervalsMs(320);
        assert(stdev(intervals) < 0.25);
        assert(minimum(intervals) > 16.0);
    }
}

// A host stall (no capture for 250 ms): the first frame after it is shown at
// once, the next frames are not compressed, and the stall does not teach the
// buffer.
void testHostStall()
{
    Sim sim(config(120, 60));
    auto frames = stream(600, 16667, 1000);
    for (size_t i = 300; i < frames.size(); ++i) {
        frames[i].arrivalUs += 250000;
        frames[i].rtp += 250000 * 9 / 100;
    }
    sim.run(frames);
    const auto before = sim.presents[290].decision.playoutDelayUs;
    const auto after = sim.presents.back().decision.playoutDelayUs;
    assert(after <= before + 500);
    size_t resumed = 0;
    while (sim.presents[resumed].number < 301) ++resumed;
    assert(sim.presents[resumed].submitUs - sim.presents[resumed].arrivalUs < 30000);
    for (size_t i = resumed + 1; i < resumed + 10; ++i) {
        assert(sim.presents[i].submitUs - sim.presents[i - 1].submitUs >= 8333);
    }
}

// The path latency steps up by 40 ms and stays there: within a few seconds
// frames are scheduled on their slots again instead of on arrival.
void testLatencyStep()
{
    Sim sim(config(120, 60));
    auto frames = stream(900, 16667, 3000);
    for (size_t i = 300; i < frames.size(); ++i) frames[i].arrivalUs += 40000;
    sim.run(frames);
    const auto settled = sim.intervalsMs(600);
    assert(stdev(settled) < 0.25);
    assert(sim.presents.back().submitUs - sim.presents.back().arrivalUs < 20000);
}

// The source rate is learned from RTP, not assumed from the negotiated rate.
void testSourceRateFollowsTheHost()
{
    Sim sim(config(144, 144));
    auto frames = stream(144, 6944, 300);
    // The game drops to 90 FPS.
    auto slow = stream(400, 11111, 300, 5000, frames.back().arrivalUs + 11111 - 5000,
                       frames.back().rtp + 1000);
    for (auto& f : slow) f.number += frames.back().number;
    frames.insert(frames.end(), slow.begin(), slow.end());
    sim.run(frames);
    const auto period = sim.controller.sourcePeriodUs();
    assert(period > 10800 && period < 11400);
    const auto intervals = sim.intervalsMs(300);
    assert(std::fabs(intervals[intervals.size() / 2] - 11.111) < 0.3);
}

// A stream at the refresh rate stays sustainable: natively protected presents
// latch instead of being held by a software floor, so nothing is dropped.
void testStreamAtRefresh()
{
    Sim native(config(120, 120, 1, false, PresentProtection::Native));
    native.run(stream(1200, 8333, 1500));
    assert(native.staleDrops == 0);
    assert(native.presents.size() == 1200);

    // With a software floor (tearing presentation) spacing below one display
    // period plus guard is impossible: latency stays bounded by dropping.
    Sim floored(config(120, 120, 1, false, PresentProtection::SoftwareFloor));
    floored.run(stream(1200, 8333, 1500));
    uint64_t worstAge = 0;
    for (size_t i = 600; i < floored.presents.size(); ++i)
        worstAge = std::max(worstAge, floored.presents[i].submitUs - floored.presents[i].arrivalUs);
    assert(worstAge < 45000);
    for (size_t i = 1; i < floored.presents.size(); ++i)
        assert(floored.presents[i].submitUs - floored.presents[i - 1].submitUs >= 8333);
}

// Preparation (decode and render) close to the source period, as with
// PyroWave 4:4:4 at 2880x1920 on an integrated GPU. Frames are prepared after
// leaving the queue, so an on-time frame's age at presentation includes its
// preparation: it must not be judged stale. A prepared frame is always
// presented; when the pipeline cannot keep up, frames are skipped before
// preparation, close to the throughput deficit and with bounded latency.
void testHeavyPreparation()
{
    auto run = [](uint64_t presentCallUs) {
        Sim sim(config(120, 116));
        sim.presentCallUs = presentCallUs;
        auto frames = stream(4000, 8621, 1000);
        Rng rng;
        for (auto& f : frames) f.preparationUs = 6500 + rng.next(2001);
        sim.run(frames);
        return sim;
    };

    // Preparation 7.5 ms plus a 0.8 ms present call fit the 8.62 ms period.
    const Sim keeps = run(800);
    assert(keeps.staleDrops * 1000 < 4000);
    size_t late = 0;
    for (const auto& p : keeps.presents) late += p.submitUs > p.decision.targetUs + 1000;
    assert(late * 100 < keeps.presents.size());

    // 7.5 + 1.6 ms exceeds the period by about 5 %: drops stay near that.
    const Sim behind = run(1600);
    assert(behind.staleDrops * 100 < 4000 * 9);
    uint64_t worstAge = 0;
    for (size_t i = 1000; i < behind.presents.size(); ++i)
        worstAge = std::max(worstAge, behind.presents[i].submitUs - behind.presents[i].arrivalUs);
    assert(worstAge < behind.controller.staleHorizonUs(0) + 8500 + 1600);
}

// Reduce judder: an alternating 7/13 ms source (100 FPS average) presents
// more evenly with smoothing than without, within its 6 ms allowance.
void testReduceJudder()
{
    auto alternating = [](int count) {
        std::vector<Sim::Frame> frames;
        uint64_t source = 0;
        for (int i = 0; i < count; ++i) {
            Sim::Frame f;
            f.number = i + 1;
            f.rtp = 5000 + rtpAt(source);
            f.arrivalUs = 1000000 + source + 4000;
            frames.push_back(f);
            source += i % 2 ? 13000 : 7000;
        }
        return frames;
    };
    Sim raw(config(144, 120, 1, false));
    raw.run(alternating(2000));
    Sim smooth(config(144, 120, 1, true));
    smooth.run(alternating(2000));
    const double rawJerk = stdev(raw.intervalsMs(500));
    const double smoothJerk = stdev(smooth.intervalsMs(500));
    assert(rawJerk > 2.5);
    assert(smoothJerk < rawJerk * 0.5);
    for (size_t i = 500; i < smooth.presents.size(); ++i)
        assert(smooth.presents[i].decision.smoothingUs <= CadenceSmoother::kMaxLagUs);
}

// Timestamps wrap at 32 bits without a rebase or a discontinuity.
void testRtpWrap()
{
    Sim sim(config(120, 60));
    auto frames = stream(600, 16667, 1000, 5000, 1000000, 0xFFFFFFFFu - 90000 * 3);
    sim.run(frames);
    assert(sim.controller.stats().rebases == 0);
    const auto intervals = sim.intervalsMs(100);
    assert(stdev(intervals) < 0.25);
}

// A reconnect restarts frame numbers and RTP: the timeline rebases instead of
// scheduling far into the future or the past.
void testReconnectRebases()
{
    Sim sim(config(120, 60));
    auto first = stream(300, 16667, 1000);
    sim.run(first);
    auto second = stream(300, 16667, 1000, 5000, first.back().arrivalUs + 50000, 900);
    sim.run(second);
    assert(sim.controller.stats().rebases >= 1);
    for (size_t i = first.size(); i < sim.presents.size(); ++i) {
        const auto& p = sim.presents[i];
        assert(p.submitUs - p.arrivalUs < 40000);
    }
    const auto intervals = sim.intervalsMs(first.size() + 60);
    assert(stdev(intervals) < 0.25);
}

// PyroWave frames that lost packets present on schedule but never grow the buffer.
void testLossDoesNotTeachTheBuffer()
{
    Sim sim(config(120, 120, 0));
    auto frames = stream(120 * 20, 8333, 300);
    Rng rng;
    for (auto& f : frames) {
        if (rng.next(4) == 0) {
            f.arrivalUs += 9000;
            f.lostPackets = 3;
        }
    }
    for (size_t i = 1; i < frames.size(); ++i)
        frames[i].arrivalUs = std::max(frames[i].arrivalUs, frames[i - 1].arrivalUs + 1);
    sim.run(frames);
    const auto stats = sim.controller.stats();
    assert(stats.playoutDelayUs <= 8333);
}

// V-Sync is required for VRR, so the present mode never tears unless the user
// explicitly allowed it; Mailbox is latched natively, FIFO uses the floor.
void testSynchronizedPresentMode()
{
    PresentChoice c = chooseSynchronizedPresentMode(true, true, false);
    assert(c.mode == PresentMode::Mailbox && c.protection == PresentProtection::Native);
    c = chooseSynchronizedPresentMode(false, true, false);
    assert(c.mode == PresentMode::Fifo && c.protection == PresentProtection::SoftwareFloor);
    c = chooseSynchronizedPresentMode(false, false, false);
    assert(c.mode == PresentMode::Fifo && c.protection == PresentProtection::SoftwareFloor);
    c = chooseSynchronizedPresentMode(true, true, true);
    assert(c.mode == PresentMode::Immediate && c.protection == PresentProtection::SoftwareFloor);
    c = chooseSynchronizedPresentMode(true, false, true);
    assert(c.mode == PresentMode::Mailbox && c.protection == PresentProtection::Native);
}

}

int main()
{
    testRatePolicy();
    testTimingOptions();
    testJitterDoesNotReachTheDisplay();
    testBufferGrowsAndReleases();
    testLowLatencyBound();
    testLateFrameRecoveryWithoutBurst();
    testHostStall();
    testLatencyStep();
    testSourceRateFollowsTheHost();
    testStreamAtRefresh();
    testHeavyPreparation();
    testReduceJudder();
    testRtpWrap();
    testReconnectRebases();
    testLossDoesNotTeachTheBuffer();
    testSynchronizedPresentMode();
    std::puts("vrr-timing: all tests passed");
    return 0;
}
