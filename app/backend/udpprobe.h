#pragma once
// Pyroshine UDP bandwidth calibration, protocol version 1 (receiver side).
//
// The host sends frame-shaped datagrams through its real video sender to the
// UDP endpoint from which this client sent an authenticated hello. The client
// keeps sending authenticated reports (the host stops when they stop), then
// submits its per-step statistics over HTTPS and receives the host's
// measurements and bitrate suggestion. The receiver statistics here mirror
// moonshine_core::calibration::client in Pyroshine field by field.
#include <QByteArray>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

namespace PyroshineUdpProbe {

constexpr int ProtocolVersion = 1;
constexpr int MaxSteps = 8;
constexpr int RunIdBytes = 16;
constexpr int KeyBytes = 32;
constexpr int ProbeHeaderBytes = 24;
constexpr int ControlBodyBytes = 4 + RunIdBytes + 4 + 4 + 4 * MaxSteps;
constexpr int ControlBytes = ControlBodyBytes + 16;
constexpr std::uint8_t FlagEndOfFrame = 0x1;
constexpr std::uint8_t FlagEndOfStep = 0x2;
constexpr std::uint8_t FlagEndOfRun = 0x4;

enum class Shape { Burst, Paced };
enum ControlKind : std::uint8_t { Hello = 1, Report = 2, Stop = 3 };

// Receive buffer Moonlight requests for its video socket (LiVideoReceiveBufferSize).
inline int streamReceiveBufferBytes(int packetSize, Shape shape) {
    return (shape == Shape::Paced ? 8192 : 2048) * (packetSize + 16);
}

inline QString xmlValue(const QString& xml, const QString& tag) {
    const QString open = QLatin1Char('<') + tag + QLatin1Char('>');
    const int start = xml.indexOf(open);
    if (start < 0) return QString();
    const int from = start + open.size();
    const int end = xml.indexOf(QStringLiteral("</") + tag + QLatin1Char('>'), from);
    return end < 0 ? QString() : xml.mid(from, end - from);
}

struct StepPlan {
    std::uint32_t offeredKbps = 0, packetsPerFrame = 0, frames = 0;
    std::uint64_t packets() const { return std::uint64_t(packetsPerFrame) * frames; }
};

struct StartReply {
    QByteArray runId, key;
    std::uint16_t udpPort = 0;
    int packetSize = 0, shardBytes = 0, fps = 0, stepMs = 0, fecPercent = 0, chunkPackets = 0;
    Shape shape = Shape::Burst;
    std::vector<StepPlan> steps;

    std::int64_t intervalUs() const { return fps > 0 ? 1000000 / fps : 0; }

    bool parse(const QString& xml, QString* error) {
        auto number = [&](const char* tag, long long* out) {
            bool ok = false;
            *out = xmlValue(xml, QLatin1String(tag)).toLongLong(&ok);
            if (!ok && error) *error = QStringLiteral("missing %1").arg(QLatin1String(tag));
            return ok;
        };
        long long port, packet, shard, f, step, fec, chunk, version;
        if (!number("Version", &version) || !number("UdpPort", &port) || !number("PacketSize", &packet) ||
            !number("ShardBytes", &shard) || !number("Fps", &f) || !number("StepMs", &step) ||
            !number("FecPercent", &fec) || !number("ChunkPackets", &chunk)) return false;
        runId = QByteArray::fromHex(xmlValue(xml, QStringLiteral("RunId")).toLatin1());
        key = QByteArray::fromHex(xmlValue(xml, QStringLiteral("Key")).toLatin1());
        const QString shapeText = xmlValue(xml, QStringLiteral("Shape"));
        if (version != ProtocolVersion || runId.size() != RunIdBytes || key.size() != KeyBytes ||
            port <= 0 || port > 65535 || shard < ProbeHeaderBytes || shard > 65507 || f < 1 || f > 1000 ||
            fec < 0 || fec > 255 || chunk < 1 || (shapeText != QLatin1String("burst") && shapeText != QLatin1String("paced"))) {
            if (error) *error = QStringLiteral("invalid calibration reply");
            return false;
        }
        udpPort = std::uint16_t(port); packetSize = int(packet); shardBytes = int(shard); fps = int(f);
        stepMs = int(step); fecPercent = int(fec); chunkPackets = int(chunk);
        shape = shapeText == QLatin1String("paced") ? Shape::Paced : Shape::Burst;
        steps.clear();
        int from = 0;
        while ((from = xml.indexOf(QStringLiteral("<Step>"), from)) >= 0) {
            const int end = xml.indexOf(QStringLiteral("</Step>"), from);
            if (end < 0) break;
            const QString item = xml.mid(from, end - from);
            StepPlan plan;
            bool a = false, b = false, c = false;
            plan.offeredKbps = xmlValue(item, QStringLiteral("OfferedKbps")).toUInt(&a);
            plan.packetsPerFrame = xmlValue(item, QStringLiteral("PacketsPerFrame")).toUInt(&b);
            plan.frames = xmlValue(item, QStringLiteral("Frames")).toUInt(&c);
            // Bound what this client allocates for one step, whatever the host says.
            if (!a || !b || !c || plan.packetsPerFrame == 0 || plan.packetsPerFrame > 65535 ||
                plan.frames == 0 || plan.frames > 20000 || plan.packets() > 8000000) {
                if (error) *error = QStringLiteral("invalid calibration step");
                return false;
            }
            steps.push_back(plan);
            from = end;
        }
        if (steps.empty() || steps.size() > size_t(MaxSteps)) {
            if (error) *error = QStringLiteral("invalid calibration step count");
            return false;
        }
        return true;
    }
};

inline void putBe32(char* out, std::uint32_t value) {
    out[0] = char(value >> 24); out[1] = char(value >> 16); out[2] = char(value >> 8); out[3] = char(value);
}
inline std::uint32_t getBe32(const char* in) {
    return std::uint32_t(std::uint8_t(in[0])) << 24 | std::uint32_t(std::uint8_t(in[1])) << 16 |
           std::uint32_t(std::uint8_t(in[2])) << 8 | std::uint32_t(std::uint8_t(in[3]));
}

// Authenticated client -> host datagram (truncated HMAC-SHA256 over the body).
inline QByteArray encodeControl(const StartReply& reply, ControlKind kind, std::uint32_t counter,
                                const std::array<std::uint32_t, MaxSteps>& received = {}) {
    QByteArray out(ControlBytes, '\0');
    char* d = out.data();
    memcpy(d, "PYC1", 4);
    memcpy(d + 4, reply.runId.constData(), RunIdBytes);
    d[20] = char(kind);
    putBe32(d + 24, counter);
    for (int step = 0; step < MaxSteps; ++step) putBe32(d + 28 + step * 4, received[step]);
    const QByteArray tag = QMessageAuthenticationCode::hash(out.left(ControlBodyBytes), reply.key,
                                                            QCryptographicHash::Sha256);
    memcpy(d + ControlBodyBytes, tag.constData(), 16);
    return out;
}

struct ProbeHeader {
    std::uint32_t runTag = 0, sequence = 0, frame = 0, frameOffsetUs = 0;
    std::uint8_t step = 0, flags = 0;
    std::uint16_t index = 0;
    static bool parse(const char* d, int length, ProbeHeader* out) {
        if (length < ProbeHeaderBytes || memcmp(d, "PYB1", 4) != 0) return false;
        out->runTag = getBe32(d + 4);
        out->sequence = getBe32(d + 8);
        out->step = std::uint8_t(d[12]);
        out->flags = std::uint8_t(d[13]);
        out->index = std::uint16_t(std::uint8_t(d[14]) << 8 | std::uint8_t(d[15]));
        out->frame = getBe32(d + 16);
        out->frameOffsetUs = getBe32(d + 20);
        return true;
    }
};

// Field order is the protocol's `s<i>=` order.
struct StepSummary {
    std::uint64_t received = 0, duplicates = 0, reordered = 0, gapEvents = 0, maxGap = 0, framesComplete = 0,
                  framesUnrecoverable = 0, queueP50Us = 0, queueP95Us = 0, queueMaxUs = 0, frameJitterUs = 0;
    QString fields() const {
        QStringList list;
        for (auto value : {received, duplicates, reordered, gapEvents, maxGap, framesComplete, framesUnrecoverable,
                           queueP50Us, queueP95Us, queueMaxUs, frameJitterUs})
            list << QString::number(value);
        return list.join(QLatin1Char(','));
    }
};

// Per-run receive accounting. Delays use 50 us buckets from -200 ms to +400 ms
// around the first datagram's delay; only their variation is used.
class Receiver {
public:
    static constexpr std::int64_t BucketUs = 50, BucketOffset = 4000;
    static constexpr int Buckets = 12000;

    explicit Receiver(const StartReply& reply) : m_Reply(reply), m_IntervalUs(reply.intervalUs()) {
        m_RunTag = getBe32(reply.runId.constData());
        std::uint64_t sequence = 0;
        std::uint32_t frame = 0;
        for (const auto& plan : reply.steps) {
            Step step;
            step.plan = plan;
            step.firstSequence = sequence;
            step.firstFrame = frame;
            step.seen.assign(size_t((plan.packets() + 63) / 64), 0);
            step.frameCounts.assign(plan.frames, 0);
            step.frameDelivery.assign(plan.frames, Unset);
            step.histogram.assign(Buckets, 0);
            m_Steps.push_back(std::move(step));
            sequence += plan.packets();
            frame += plan.frames;
        }
    }

    // Returns the step index, or -1 for a datagram that is not part of this run.
    int accept(const char* data, int length, std::int64_t arrivalUs, std::uint8_t* flags) {
        ProbeHeader header;
        if (!ProbeHeader::parse(data, length, &header) || header.runTag != m_RunTag || length != m_Reply.shardBytes ||
            header.step >= m_Steps.size()) return -1;
        Step& step = m_Steps[header.step];
        const std::uint64_t sequence = header.sequence;
        if (sequence < step.firstSequence || header.frame < step.firstFrame) return -1;
        const std::uint64_t index = sequence - step.firstSequence;
        const std::uint32_t frame = header.frame - step.firstFrame;
        if (index >= step.plan.packets() || frame >= step.plan.frames) return -1;
        *flags = header.flags;
        std::uint64_t& word = step.seen[size_t(index / 64)];
        const std::uint64_t bit = std::uint64_t(1) << (index % 64);
        if (word & bit) { ++step.duplicates; return header.step; }
        word |= bit;
        ++step.received;
        ++m_Received;
        if (m_HasHighest && sequence < m_Highest) ++step.reordered;
        if (!m_HasHighest || sequence > m_Highest) { m_Highest = sequence; m_HasHighest = true; }
        ++step.frameCounts[frame];
        if (!step.hasHighestFrame || frame > step.highestFrame) { step.highestFrame = frame; step.hasHighestFrame = true; }

        if (!m_HasStart) { m_StartUs = arrivalUs; m_HasStart = true; }
        std::int64_t scheduled = header.frameOffsetUs;
        if (m_Reply.shape == Shape::Paced) {
            // The host pacer releases whole GSO chunks at proportional deadlines.
            const std::int64_t chunk = (std::max)(m_Reply.chunkPackets, 1);
            const std::int64_t preceding = std::int64_t(header.index) / chunk * chunk;
            scheduled += preceding * m_IntervalUs / std::int64_t(step.plan.packetsPerFrame);
        }
        const std::int64_t delay = (arrivalUs - m_StartUs) - scheduled;
        if (!m_HasFirstDelay) { m_FirstDelay = delay; m_HasFirstDelay = true; }
        const std::int64_t relative = delay - m_FirstDelay;
        const std::int64_t bucket = std::clamp<std::int64_t>(relative / BucketUs + BucketOffset, 0, Buckets - 1);
        ++step.histogram[size_t(bucket)];
        std::int64_t& delivery = step.frameDelivery[frame];
        delivery = delivery == Unset ? relative : (std::max)(delivery, relative);
        if (header.flags & FlagEndOfStep) step.ended = true;
        for (int earlier = 0; earlier < header.step; ++earlier) m_Steps[size_t(earlier)].ended = true;
        return header.step;
    }

    std::uint64_t received() const { return m_Received; }
    bool stepEnded(int step) const { return m_Steps[size_t(step)].ended; }
    int stepCount() const { return int(m_Steps.size()); }
    std::uint64_t lossPercent(int step) const {
        const auto& s = m_Steps[size_t(step)];
        const std::uint64_t planned = (std::max<std::uint64_t>)(s.plan.packets(), 1);
        return (planned - (std::min)(s.received, planned)) * 100 / planned;
    }
    std::array<std::uint32_t, MaxSteps> reportCounts() const {
        std::array<std::uint32_t, MaxSteps> counts{};
        for (size_t i = 0; i < m_Steps.size() && i < size_t(MaxSteps); ++i)
            counts[i] = std::uint32_t((std::min<std::uint64_t>)(m_Steps[i].received, 0xffffffffu));
        return counts;
    }

    std::vector<std::optional<StepSummary>> summarize() const {
        std::int64_t floor = 0;
        for (int bucket = 0; bucket < Buckets; ++bucket) {
            if (std::any_of(m_Steps.begin(), m_Steps.end(), [&](const Step& s) { return s.histogram[size_t(bucket)] > 0; })) {
                floor = bucket;
                break;
            }
        }
        std::vector<std::optional<StepSummary>> result;
        for (const auto& step : m_Steps) result.push_back(summarizeStep(step, floor));
        return result;
    }

private:
    static constexpr std::int64_t Unset = INT64_MIN;
    struct Step {
        StepPlan plan;
        std::uint64_t firstSequence = 0;
        std::uint32_t firstFrame = 0, highestFrame = 0;
        bool hasHighestFrame = false, ended = false;
        std::vector<std::uint64_t> seen;
        std::uint64_t received = 0, duplicates = 0, reordered = 0;
        std::vector<std::uint32_t> frameCounts, histogram;
        std::vector<std::int64_t> frameDelivery;
    };

    std::optional<StepSummary> summarizeStep(const Step& step, std::int64_t floor) const {
        const std::uint32_t frames = step.ended ? step.plan.frames : (step.hasHighestFrame ? step.highestFrame + 1 : 0);
        if (frames == 0) return std::nullopt;
        StepSummary s;
        s.received = step.received;
        s.duplicates = step.duplicates;
        s.reordered = step.reordered;
        const std::uint64_t packets = std::uint64_t(frames) * step.plan.packetsPerFrame;
        std::uint64_t run = 0;
        for (std::uint64_t i = 0; i < packets; ++i) {
            if (!(step.seen[size_t(i / 64)] & (std::uint64_t(1) << (i % 64)))) {
                if (run == 0) ++s.gapEvents;
                s.maxGap = (std::max)(s.maxGap, ++run);
            } else {
                run = 0;
            }
        }
        const std::uint32_t perFrame = step.plan.packetsPerFrame;
        const std::uint32_t data = std::uint32_t((std::uint64_t(perFrame) * 100 + (100 + m_Reply.fecPercent) - 1) /
                                                 std::uint64_t(100 + m_Reply.fecPercent));
        const std::uint32_t repairable = perFrame - (std::min)(data, perFrame);
        std::vector<std::int64_t> deliveries;
        for (std::uint32_t f = 0; f < frames; ++f) {
            const std::uint32_t count = step.frameCounts[f];
            if (count >= perFrame) ++s.framesComplete;
            if (perFrame - (std::min)(count, perFrame) > repairable) ++s.framesUnrecoverable;
            if (step.frameDelivery[f] != Unset) deliveries.push_back(step.frameDelivery[f]);
        }
        std::uint64_t total = 0;
        for (auto c : step.histogram) total += c;
        if (total > 0) {
            auto at = [&](std::uint64_t rank) -> std::uint64_t {
                std::uint64_t seen = 0;
                for (int bucket = 0; bucket < Buckets; ++bucket) {
                    seen += step.histogram[size_t(bucket)];
                    if (seen > rank) return std::uint64_t((std::max<std::int64_t>)(bucket - floor, 0) * BucketUs);
                }
                return 0;
            };
            s.queueP50Us = at(total / 2);
            s.queueP95Us = at(total * 95 / 100);
            int last = Buckets - 1;
            while (last > 0 && step.histogram[size_t(last)] == 0) --last;
            s.queueMaxUs = std::uint64_t((std::max<std::int64_t>)(last - floor, 0) * BucketUs);
        }
        if (!deliveries.empty()) {
            std::sort(deliveries.begin(), deliveries.end());
            auto at = [&](size_t share) { return deliveries[(deliveries.size() - 1) * share / 100]; };
            s.frameJitterUs = std::uint64_t(std::clamp<std::int64_t>(at(95) - at(50), 0, 10 * m_IntervalUs));
        }
        return s;
    }

    const StartReply& m_Reply;
    std::int64_t m_IntervalUs;
    std::uint32_t m_RunTag = 0;
    std::vector<Step> m_Steps;
    std::uint64_t m_Received = 0, m_Highest = 0;
    bool m_HasHighest = false, m_HasStart = false, m_HasFirstDelay = false;
    std::int64_t m_StartUs = 0, m_FirstDelay = 0;
};

inline QString resultQuery(const StartReply& reply, const std::vector<std::optional<StepSummary>>& steps,
                           int clientLinkMbps) {
    QString query = QStringLiteral("run=") + QString::fromLatin1(reply.runId.toHex());
    for (size_t i = 0; i < steps.size(); ++i)
        if (steps[i]) query += QStringLiteral("&s%1=%2").arg(i).arg(steps[i]->fields());
    if (clientLinkMbps > 0) query += QStringLiteral("&clientlinkmbps=%1").arg(clientLinkMbps);
    return query;
}

// Top of the rate ladder: well above the current setting so the limit can be
// found, below any known physical link, within the protocol bound.
inline std::uint32_t ladderTopKbps(int bitrateKbps, int hostLinkMbps, int clientLinkMbps, std::uint32_t hostMaxKbps) {
    std::uint64_t top = (std::max<std::uint64_t>)(std::uint64_t((std::max)(bitrateKbps, 0)) * 2, 150000);
    for (int link : {hostLinkMbps, clientLinkMbps})
        if (link > 0) top = (std::min<std::uint64_t>)(top, std::uint64_t(link) * 1000 * 95 / 100);
    if (hostMaxKbps > 0) top = (std::min<std::uint64_t>)(top, hostMaxKbps);
    return std::uint32_t((std::max<std::uint64_t>)(top, 5000));
}

// Calibration method for a host, from its paired /serverinfo.
enum class Method { Udp, Https, None };
inline Method selectMethod(int udpProbeVersion, bool pyroWaveHttpsProbe) {
    if (udpProbeVersion == ProtocolVersion) return Method::Udp;
    // A newer protocol this client does not speak falls back like an older host.
    return pyroWaveHttpsProbe ? Method::Https : Method::None;
}

} // namespace PyroshineUdpProbe
