#pragma once

// Wire formats and client-side state of Rubylight's control messages
// (Rubylight docs/control-messages.md), as defined in Nightjar's rubylight-protocol
// crate (protocol/core: phase_lock.rs, control.rs, present_clock.rs). All fields
// are little-endian and every payload starts with version 1; a reader takes the
// fields it knows and ignores appended ones.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace Rubylight {

constexpr uint8_t Version = 1;

inline uint16_t u16At(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t u32At(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint64_t u64At(const uint8_t* p) { return uint64_t(u32At(p)) | uint64_t(u32At(p + 4)) << 32; }
inline void putU16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void putU32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
inline void putU64(uint8_t* p, uint64_t v) { putU32(p, uint32_t(v)); putU32(p + 4, uint32_t(v >> 32)); }

inline int64_t remEuclid(int64_t value, int64_t period)
{
    const int64_t r = value % period;
    return r < 0 ? r + period : r;
}

// 0x5530: slack statistics of the frames shown in one window (phase_lock::Report).
struct PhaseReport {
    uint16_t frames = 0;
    uint32_t periodNs = 0;
    int32_t leadNs = 0;    // 10th percentile slack before the latch; negative when late.
    uint32_t spreadNs = 0; // 10th to 90th percentile.

    static constexpr size_t Bytes = 16;
    std::array<uint8_t, Bytes> encode() const
    {
        std::array<uint8_t, Bytes> out {};
        out[0] = Version;
        putU16(&out[2], frames);
        putU32(&out[4], periodNs);
        putU32(&out[8], uint32_t(leadNs));
        putU32(&out[12], spreadNs);
        return out;
    }
};

// The host keeps the early edge of the slack this far before the latch (PhaseLock::DEFAULT_MARGIN_NS).
constexpr int64_t PhaseMarginNs = 1500000;

// Client side of phase lock (phase_lock::SlackWindow): collects each shown frame's
// slack, the time from "ready" to the next latch deadline, and summarises a window.
class SlackWindow {
public:
    static constexpr size_t Capacity = 256; // Half a second at 500 fps.

    void push(int64_t slackNs, int64_t periodNs)
    {
        if (periodNs <= 0) return;
        if (periodNs != m_PeriodNs) {
            // A refresh-rate change makes earlier samples meaningless.
            m_Count = 0;
            m_PeriodNs = periodNs;
        }
        if (m_Count < Capacity) m_Samples[m_Count++] = remEuclid(slackNs, periodNs);
    }

    size_t size() const { return m_Count; }
    void clear() { m_Count = 0; }

    // Summarises and clears the window. Slack is circular, so samples are centred
    // on their circular mean before taking percentiles: a cluster straddling the
    // latch stays together instead of splitting across both ends of the refresh.
    std::optional<PhaseReport> report(size_t minFrames)
    {
        const size_t count = m_Count;
        m_Count = 0;
        if (count < (std::max)(minFrames, size_t(1))) return std::nullopt;
        const int64_t period = m_PeriodNs;
        int64_t* samples = m_Samples.data();
        const int64_t centre = circularMean(samples, count, period);
        for (size_t i = 0; i < count; ++i) {
            int64_t deviation = remEuclid(samples[i] - centre, period);
            if (deviation > period / 2) deviation -= period;
            samples[i] = deviation;
        }
        std::sort(samples, samples + count);
        const auto percentile = [&](size_t p) { return samples[(count - 1) * p / 100]; };
        const int64_t low = percentile(10);
        const int64_t high = percentile(90);
        int64_t lead = remEuclid(centre + low, period);
        if (lead > period / 2) lead -= period;
        PhaseReport report;
        report.frames = uint16_t((std::min)(count, size_t(UINT16_MAX)));
        report.periodNs = uint32_t(std::clamp<int64_t>(period, 0, UINT32_MAX));
        report.leadNs = int32_t(lead);
        report.spreadNs = uint32_t(std::clamp<int64_t>(high - low, 0, UINT32_MAX));
        return report;
    }

    // report() for a window that took elapsedNs of client time. When nearly every
    // refresh had a frame (90% or more), a missed latch drops a frame, so the early
    // edge is reported lower by a quarter of the spread: the host then keeps it
    // further from the latch, while the whole cluster still fits in half a refresh.
    std::optional<PhaseReport> reportOver(size_t minFrames, int64_t elapsedNs)
    {
        const int64_t frames = int64_t(m_Count);
        const int64_t period = m_PeriodNs;
        auto report = this->report(minFrames);
        if (!report) return report;
        if (elapsedNs > 0 && frames * period * 10 >= elapsedNs * 9) {
            const int64_t spread = report->spreadNs;
            const int64_t target = (std::max)(PhaseMarginNs, spread / 4);
            const int64_t room = (std::max)(period / 2 - spread - target, int64_t(0));
            const int64_t lead = int64_t(report->leadNs) - (std::min)(spread / 4, room);
            report->leadNs = int32_t((std::max)(lead, -(period / 2)));
        }
        return report;
    }

private:
    // The mean after rotating the samples so the widest gap between neighbours sits at the wrap.
    static int64_t circularMean(const int64_t* samples, size_t count, int64_t period)
    {
        std::array<int64_t, Capacity> sorted {};
        std::copy(samples, samples + count, sorted.begin());
        std::sort(sorted.begin(), sorted.begin() + count);
        size_t start = 0;
        int64_t widest = sorted[0] + period - sorted[count - 1];
        for (size_t i = 1; i < count; ++i) {
            const int64_t gap = sorted[i] - sorted[i - 1];
            if (gap > widest) {
                widest = gap;
                start = i;
            }
        }
        const int64_t origin = sorted[start];
        int64_t total = 0;
        for (size_t i = 0; i < count; ++i) total += remEuclid(sorted[i] - origin, period);
        return remEuclid(origin + total / int64_t(count), period);
    }

    std::array<int64_t, Capacity> m_Samples {};
    size_t m_Count = 0;
    int64_t m_PeriodNs = 0;
};

// 0x5536, 0x5538 requests (4 bytes: version, flags bit 0 on, two reserved).
inline std::array<uint8_t, 4> enableRequest(bool on) { return {Version, uint8_t(on ? 1 : 0), 0, 0}; }

// 0x553D request: version, enabled, interval in ms (500-10000).
inline std::array<uint8_t, 4> pipelineRatesRequest(bool on, uint16_t intervalMs)
{
    std::array<uint8_t, 4> out {Version, uint8_t(on ? 1 : 0), 0, 0};
    putU16(&out[2], uint16_t(std::clamp<int>(intervalMs, 500, 10000)));
    return out;
}

// 0x5537 probe: version, three reserved, sequence, the client's send time.
inline std::array<uint8_t, 16> clockSyncRequest(uint32_t sequence, uint64_t clientUs)
{
    std::array<uint8_t, 16> out {};
    out[0] = Version;
    putU32(&out[4], sequence);
    putU64(&out[8], clientUs);
    return out;
}

struct ClockSyncReply {
    uint32_t sequence = 0;
    uint64_t clientUs = 0;      // The probe's own time, echoed.
    uint64_t hostReceiveUs = 0; // Host times are microseconds on the video RTP clock.
    uint64_t hostSendUs = 0;

    static std::optional<ClockSyncReply> decode(const uint8_t* p, int length)
    {
        if (length < 32 || p[0] != Version) return std::nullopt;
        return ClockSyncReply {u32At(p + 4), u64At(p + 8), u64At(p + 16), u64At(p + 24)};
    }
};

// The offset from the host's video clock to the client's (client = host + offset), from
// the exchange with the shortest round trip among the last 32 (present_clock::ClockOffset).
class ClockOffset {
public:
    // Returns the round trip, or nothing for an exchange that cannot be right. An offset
    // more than a second away from the current one means the host restarted its clock.
    std::optional<int64_t> add(const ClockSyncReply& reply, uint64_t clientReceiveUs)
    {
        const int64_t t1 = int64_t(reply.clientUs), t2 = int64_t(reply.hostReceiveUs);
        const int64_t t3 = int64_t(reply.hostSendUs), t4 = int64_t(clientReceiveUs);
        const int64_t rtt = (t4 - t1) - (t3 - t2);
        if (t4 < t1 || rtt < 0 || rtt > 1000000) return std::nullopt;
        const int64_t offset = ((t1 - t2) + (t4 - t3)) / 2;
        if (const auto current = offsetUs(); current && std::llabs(offset - *current) > 1000000) m_Samples.clear();
        if (m_Samples.size() == Samples) m_Samples.erase(m_Samples.begin());
        m_Samples.push_back({offset, rtt});
        return rtt;
    }

    std::optional<int64_t> offsetUs() const
    {
        if (m_Samples.empty()) return std::nullopt;
        return best().offset;
    }
    std::optional<int64_t> rttUs() const
    {
        if (m_Samples.empty()) return std::nullopt;
        return best().rtt;
    }
    void clear() { m_Samples.clear(); }

private:
    struct Sample { int64_t offset, rtt; };
    static constexpr size_t Samples = 32;
    const Sample& best() const
    {
        return *std::min_element(m_Samples.begin(), m_Samples.end(),
                                 [](const Sample& a, const Sample& b) { return a.rtt < b.rtt; });
    }
    std::vector<Sample> m_Samples;
};

// How long ago, in microseconds, the host presented a frame stamped `rtp` (90 kHz,
// t µs × 90 / 1000 modulo 2^32), given the client time now and the clock offset.
inline int64_t frameAgeUs(uint32_t rtp, int64_t clientNowUs, int64_t offsetUs)
{
    const uint64_t hostNowUs = uint64_t(clientNowUs - offsetUs);
    const uint32_t hostNowRtp = uint32_t(hostNowUs * 90 / 1000);
    return int64_t(uint32_t(hostNowRtp - rtp)) * 1000 / 90;
}

// 0x5536 report: one record per new picture, on the host clock.
struct HostTimingRecord {
    uint32_t frame = 0;
    uint32_t hostUs = 0;   // Present stamp to the last packet handed to the socket.
    uint32_t queueUs = 0;  // Present to the encoder taking the picture.
    uint32_t encodeUs = 0; // Encoder taking it to the encoded frame.
    uint32_t sendUs = 0;   // First packet to the last.
};

// Decodes a report; an empty result for one that is malformed. The extension
// (Rubylight 3.0) is not needed here and is skipped.
inline std::vector<HostTimingRecord> decodeHostTiming(const uint8_t* p, int length)
{
    std::vector<HostTimingRecord> records;
    if (length < 4 || p[0] != Version) return records;
    const int count = p[1];
    if (count == 0 || length < 4 + count * 20) return records;
    for (int i = 0; i < count; ++i) {
        const uint8_t* r = p + 4 + i * 20;
        HostTimingRecord record {u32At(r), u32At(r + 4), u32At(r + 8), u32At(r + 12), u32At(r + 16)};
        // Stages that add up to more than the whole reject the report.
        if (uint64_t(record.queueUs) + record.encodeUs + record.sendUs > record.hostUs) return {};
        records.push_back(record);
    }
    return records;
}

// 0x553D report: the frame rates of each host stage over a window, in millihertz.
struct PipelineRates {
    uint32_t windowMs = 0;
    uint32_t sourceMhz = 0, captureMhz = 0, submitMhz = 0, outputMhz = 0, repeatMhz = 0;
    static constexpr uint32_t Unknown = 0xFFFFFFFF;

    static std::optional<PipelineRates> decode(const uint8_t* p, int length)
    {
        if (length < 28 || p[0] != Version) return std::nullopt;
        return PipelineRates {u32At(p + 4), u32At(p + 8), u32At(p + 12), u32At(p + 16), u32At(p + 20), u32At(p + 24)};
    }
};

// 0x5538 notice: the host lowered the video bitrate for the network, or is back.
struct LinkNotice {
    enum State : uint8_t { Full = 0, Limited = 1, Floor = 2 };
    uint8_t state = Full;
    uint32_t ceilingKbps = 0;
    uint32_t currentKbps = 0;

    static std::optional<LinkNotice> decode(const uint8_t* p, int length)
    {
        if (length < 12 || p[0] != Version || p[1] > Floor || p[2] > 1) return std::nullopt;
        // State 0 pairs only with reason 0, any other state only with a reason.
        if ((p[1] == Full) != (p[2] == 0)) return std::nullopt;
        LinkNotice notice {p[1], u32At(p + 4), u32At(p + 8)};
        if (!notice.ceilingKbps || !notice.currentKbps) return std::nullopt;
        return notice;
    }
};

} // namespace Rubylight
