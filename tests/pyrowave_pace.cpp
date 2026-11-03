// PyroWave frame-pace calibration: grading a frame-shaped probe and the pace search.
#include "backend/pyrowavepace.h"

#include <cassert>
#include <cstdio>

using namespace PyroWavePace;

// A probe of `expected` packets in which `lose` decides which sequence numbers are lost.
template<class Lose>
static Result probe(uint32_t expected, int fps, uint32_t group, Lose lose)
{
    Result result;
    result.expected = result.sent = expected;
    result.senderMs = durationMs;
    std::vector<bool> arrived(expected);
    for (uint32_t seq = 0; seq < expected; ++seq) arrived[seq] = !lose(seq);
    summarize(result, arrived, fps, group);
    return result;
}

static void grading()
{
    // 250 Mbps of 1392-byte packets for 2 s, at 60 FPS: 120 frames.
    const uint32_t expected = expectedPackets(250000, 1392);
    assert(expected == 40956);
    const uint32_t group = groupPackets(1000000, 1392, false);
    assert(group == 83);
    const auto clean = probe(expected, 60, group, [](uint32_t) { return false; });
    assert(clean.frames == 120 && clean.received == expected && clean.lossPercent == 0 && qualified(clean));

    // Losing the last packet of every frame: tiny aggregate loss, but every tail is missing.
    std::vector<bool> last(expected);
    for (uint32_t frame = 0; frame < 120; ++frame) last[uint64_t(expected) * (frame + 1) / 120 - 1] = true;
    const auto tails = probe(expected, 60, group, [&](uint32_t seq) { return bool(last[seq]); });
    assert(tails.lossPercent < 1 && tails.missingTailFramePercent() > 90 && !qualified(tails));
    assert(better(clean, tails) && !better(tails, clean));

    // One interior packet per frame is within the limits.
    std::vector<bool> early(expected);
    for (uint32_t frame = 0; frame < 120; ++frame) early[uint64_t(expected) * frame / 120 + 3] = true;
    const auto interior = probe(expected, 60, group, [&](uint32_t seq) { return bool(early[seq]); });
    assert(interior.missingTailFrames == 0 && qualified(interior));

    // A host that sent too little is not a measurement.
    Result short_ = clean;
    short_.sent = expected * 9 / 10;
    assert(!measured(short_));
}

static void searching()
{
    const auto passing = [](int limit) {
        return [limit](int pace) {
            Result r;
            r.expected = r.sent = r.received = 1000;
            r.senderMs = durationMs;
            r.frames = 120;
            const bool ok = pace <= limit;
            r.lossPercent = r.worstWindowLossPercent = r.worstFrameLossPercent = ok ? 0 : 20;
            r.missingTailFrames = ok ? 0 : 60;
            r.excessiveLossFrames = ok ? 0 : 30;
            return r;
        };
    };
    const auto never = [] { return false; };
    // The link passes: it is kept after two confirmations.
    auto atLink = search(1000000, 312500, passing(2000000), never);
    assert(atLink.paceKbps == 1000000 && atLink.qualified);
    // Loss above 650 Mbps: a 5% margin below the bracket, confirmed.
    auto bracket = search(1000000, 312500, passing(650000), never);
    assert(bracket.qualified && bracket.paceKbps <= 650000 && bracket.paceKbps >= 550000);
    // Nothing passes: an unqualified best is returned, never sent by the calibrator.
    auto none = search(1000000, 312500, passing(0), never);
    assert(!none.qualified);
    // Cancelled: nothing.
    auto cancelled = search(1000000, 312500, passing(650000), [] { return true; });
    assert(cancelled.paceKbps == 0);
    // No link: nothing.
    assert(search(0, 312500, passing(650000), never).paceKbps == 0);
}

int main()
{
    grading();
    searching();
    std::puts("pyrowave pace tests passed");
    return 0;
}
