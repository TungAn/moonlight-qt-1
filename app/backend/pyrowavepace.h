#pragma once

// PyroWave frame-pace calibration against hosts that serve Vibeshine's
// frame-shaped UDP probe (`/pyrowave-udp-probe` with PyroWaveUdpProbeBurstVersion
// and PyroWaveUdpHandshakeVersion 1: Vibeshine and Rubylight). The host sends a
// probe's packets as video frames, each frame back-to-back in 1 ms groups at a
// given pace. The fastest pace this client receives without losing frame tails
// is sent in the ANNOUNCE as x-ss-video[0].pyrowavePaceMbps, and the host paces
// its PyroWave frames at it instead of its own default.
//
// The limits, grading and search are those of Nonary's moonlight-qt
// (app/streaming/video/pyrowave/pyrowavelinkpolicy.h, GPL-3.0) and Nightjar's
// PyroWavePace, so all three clients calibrate the same way.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace PyroWavePace {

constexpr int durationMs = 2000;
constexpr int windowMs = 100;
constexpr int windowCount = durationMs / windowMs;
constexpr int minimumKbps = 5000;
// Bytes the host charges per probe packet beyond its packet size: RTP,
// encryption, UDP, IPv6 and Ethernet, as for a stream packet.
constexpr int wireOverhead = 134;
// Probe packets carry the 32-byte token, the sequence number and padding.
constexpr int probeExtraBytes = 48;
constexpr int tokenLength = 32;

// PyroWave turns lost detail into blur, so under 2% loss is usable; one bad
// 100 ms window above 5% is bunched, visible loss. Packets the host could not
// send count as lost; below 98% sent, the host itself is overloaded.
constexpr double lossLimitPercent = 2.0;
constexpr double windowLossLimitPercent = 5.0;
constexpr double minimumSentShare = 0.98;
constexpr double frameLossLimitPercent = 10.0;
constexpr double tailFrameLimitPercent = 2.0;
constexpr int stepKbps = 50000;

struct Result {
    uint32_t expected = 0;
    uint32_t sent = 0;
    uint32_t received = 0;
    double senderMs = 0;
    double lossPercent = 100;
    double worstWindowLossPercent = 100;
    uint32_t frames = 0;
    uint32_t damagedFrames = 0;
    uint32_t excessiveLossFrames = 0;
    uint32_t missingTailFrames = 0;
    double worstFrameLossPercent = 100;
    uint32_t hostSendRetries = 0;
    int hostLastSendError = 0;

    double excessiveLossFramePercent() const { return frames ? 100.0 * excessiveLossFrames / frames : 100; }
    double missingTailFramePercent() const { return frames ? 100.0 * missingTailFrames / frames : 100; }
    bool hostSentEnough() const {
        return expected > 0 && sent <= expected && sent >= expected * minimumSentShare;
    }
};

// Packets the host sends in a probe of this rate.
inline uint32_t expectedPackets(int kbps, int packetSize)
{
    return uint32_t(uint64_t(kbps) * durationMs / (8 * uint64_t(packetSize + wireOverhead)));
}

// Packets per 1 ms send group at this pace, with the host's UDP, IP and Ethernet charge.
inline uint32_t groupPackets(int paceKbps, int packetSize, bool ipv6)
{
    const int wireBytes = packetSize + probeExtraBytes + 8 + (ipv6 ? 40 : 20) + 38;
    return (std::max)(1, paceKbps / 8 / wireBytes);
}

// Grades a probe from which sequence numbers arrived. Frames are the host's
// sequence partitions, so reordering never moves a packet to another frame,
// and an absent frame or final tail still counts in full.
inline void summarize(Result& result, const std::vector<bool>& arrived, int fps, uint32_t group)
{
    result.frames = result.damagedFrames = result.excessiveLossFrames = result.missingTailFrames = 0;
    result.worstFrameLossPercent = 100;
    const auto has = [&](uint32_t seq) { return seq < arrived.size() && arrived[seq]; };
    if (fps > 0 && result.expected > 0) {
        const uint32_t frames = (std::max)(uint32_t(1), uint32_t(uint64_t(fps) * durationMs / 1000));
        group = (std::max)(uint32_t(1), group);
        result.worstFrameLossPercent = 0;
        for (uint32_t frame = 0; frame < frames; ++frame) {
            const uint32_t begin = uint32_t(uint64_t(result.expected) * frame / frames);
            const uint32_t end = uint32_t(uint64_t(result.expected) * (frame + 1) / frames);
            if (begin == end) continue;
            // The tail is the frame's final 1 ms send group.
            const uint32_t tailBegin = begin + (end - begin - 1) / group * group;
            uint32_t missing = 0;
            bool missingTail = false;
            for (uint32_t seq = begin; seq < end; ++seq) {
                if (!has(seq)) {
                    ++missing;
                    missingTail |= seq >= tailBegin;
                }
            }
            const double loss = 100.0 * missing / (end - begin);
            ++result.frames;
            result.damagedFrames += missing != 0;
            result.excessiveLossFrames += loss > frameLossLimitPercent;
            result.missingTailFrames += missingTail;
            result.worstFrameLossPercent = (std::max)(result.worstFrameLossPercent, loss);
        }
    }
    result.received = 0;
    result.worstWindowLossPercent = 0;
    for (int window = 0; window < windowCount; ++window) {
        const uint32_t begin = uint32_t(uint64_t(result.expected) * window / windowCount);
        const uint32_t end = uint32_t(uint64_t(result.expected) * (window + 1) / windowCount);
        uint32_t count = 0;
        for (uint32_t seq = begin; seq < end; ++seq) count += has(seq);
        result.received += count;
        if (end > begin) result.worstWindowLossPercent = (std::max)(result.worstWindowLossPercent,
            100.0 * (end - begin - count) / (end - begin));
    }
    result.lossPercent = result.expected ? 100.0 * (result.expected - result.received) / result.expected : 100;
}

// Only loss and the host's sending duration grade a frame-shaped probe: its
// delivery timing is bursty on purpose.
inline bool measured(const Result& r)
{
    return r.hostSentEnough() && r.received > 0 && r.received <= r.sent &&
           std::isfinite(r.senderMs) && r.senderMs >= durationMs * 0.98 && r.senderMs <= durationMs * 1.02 &&
           r.frames > 0 && std::isfinite(r.worstFrameLossPercent);
}

inline bool qualified(const Result& r)
{
    return measured(r) && r.lossPercent < lossLimitPercent && r.worstWindowLossPercent < windowLossLimitPercent &&
           r.excessiveLossFrames == 0 && r.worstFrameLossPercent <= frameLossLimitPercent &&
           r.missingTailFramePercent() < tailFrameLimitPercent;
}

// Ranks fallback measurements by violated limits, then tail loss, severe frame
// damage, worst frame damage and total loss.
inline bool better(const Result& a, const Result& b)
{
    const auto violations = [](const Result& r) {
        return int(r.lossPercent >= lossLimitPercent) + int(r.worstWindowLossPercent >= windowLossLimitPercent) +
               int(r.excessiveLossFrames != 0 || r.worstFrameLossPercent > frameLossLimitPercent) +
               int(r.missingTailFramePercent() >= tailFrameLimitPercent);
    };
    if (violations(a) != violations(b)) return violations(a) < violations(b);
    if (a.missingTailFramePercent() != b.missingTailFramePercent())
        return a.missingTailFramePercent() < b.missingTailFramePercent();
    if (a.excessiveLossFramePercent() != b.excessiveLossFramePercent())
        return a.excessiveLossFramePercent() < b.excessiveLossFramePercent();
    if (a.worstFrameLossPercent != b.worstFrameLossPercent)
        return a.worstFrameLossPercent < b.worstFrameLossPercent;
    if (a.lossPercent != b.lossPercent) return a.lossPercent < b.lossPercent;
    return a.worstWindowLossPercent < b.worstWindowLossPercent;
}

struct Outcome {
    int paceKbps = 0;       // Zero: not determined; the host keeps its default.
    bool qualified = false; // Two fresh confirmations passed every loss limit.
    Result probe;           // The measurement behind paceKbps.
};

// The highest pace between floorKbps (a frame must still fit its interval) and
// the link. The link is tried first. Below it, a bisected pass leaves a 5%
// margin that must pass twice afresh, stepping down on failure; then every
// remaining 50 Mbps step is tried, since loss need not be monotonic. Only then
// is the best unqualified measurement returned, measured twice more.
template<class Probe, class Cancelled>
Outcome search(int linkKbps, int floorKbps, Probe probe, Cancelled cancelled)
{
    const auto stepDown = [](double kbps) { return int(kbps / stepKbps) * stepKbps; };
    const int cap = stepDown(linkKbps);
    if (cap < stepKbps) return {};
    const int floor = (std::min)((std::max)(int(std::ceil(double(floorKbps) / stepKbps)) * stepKbps, int(stepKbps)), cap);
    std::vector<Outcome> measurements;
    const auto measure = [&](int pace) {
        const Result result = probe(pace);
        const auto previous = std::find_if(measurements.begin(), measurements.end(),
            [pace](const Outcome& r) { return r.paceKbps == pace; });
        // Keep the worse observation at each pace, so a lucky first run cannot win the fallback.
        if (previous == measurements.end()) measurements.push_back({pace, false, result});
        else if (!measured(result) || (measured(previous->probe) && better(previous->probe, result)))
            previous->probe = result;
        return result;
    };
    const auto passes = [&](int pace) { return qualified(measure(pace)); };
    const auto confirmed = [&](int pace) -> Outcome {
        Result last;
        for (int run = 0; run < 2; ++run) {
            if (cancelled()) return {};
            last = measure(pace);
            if (!qualified(last)) return {};
        }
        return {pace, true, last};
    };
    if (passes(cap)) {
        if (cancelled()) return {};
        const auto atLink = confirmed(cap);
        if (atLink.qualified || cancelled()) return cancelled() ? Outcome{} : atLink;
    }
    if (cancelled()) return {};
    int low = 0, high = cap;
    if (floor < cap) {
        if (passes(floor)) low = floor;
        for (int attempt = 0; low && attempt < 16 && high - low > stepKbps && !cancelled(); ++attempt) {
            const int next = (std::max)(low + stepKbps, stepDown((low + high) / 2.0));
            if (next >= high) break;
            if (passes(next)) low = next;
            else high = next;
        }
    }
    if (cancelled()) return {};
    for (int pace = (std::max)(floor, stepDown(low * 0.95)); low && !cancelled();
         pace = (std::max)(floor, stepDown(pace * 0.9))) {
        const auto result = confirmed(pace);
        if (result.qualified) return result;
        if (pace == floor) break;
    }
    // A failed floor says nothing about the speeds between it and the link.
    for (int pace = cap - stepKbps; pace >= floor && !cancelled(); pace -= stepKbps) {
        const auto previous = std::find_if(measurements.begin(), measurements.end(),
            [pace](const Outcome& r) { return r.paceKbps == pace; });
        if (previous != measurements.end() && !qualified(previous->probe)) continue;
        if (previous != measurements.end() || passes(pace)) {
            const auto result = confirmed(pace);
            if (result.qualified) return result;
        }
    }
    if (cancelled()) return {};
    Outcome best;
    for (const auto& result : measurements) {
        if (measured(result.probe) && (!best.paceKbps || better(result.probe, best.probe) ||
            (!better(best.probe, result.probe) && result.paceKbps > best.paceKbps))) best = result;
    }
    if (!best.paceKbps) return {};
    bool passed = true;
    for (int run = 0; run < 2; ++run) {
        if (cancelled()) return {};
        const auto result = measure(best.paceKbps);
        // Never publish a fallback whose final measurements are invalid.
        if (!measured(result)) return {};
        passed &= qualified(result);
        if (run == 0 || better(best.probe, result)) best.probe = result;
    }
    best.qualified = passed;
    return cancelled() ? Outcome{} : best;
}

} // namespace PyroWavePace
