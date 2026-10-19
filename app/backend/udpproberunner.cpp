#include "udpproberunner.h"

#include <QElapsedTimer>
#include <QUdpSocket>
#include <QVariant>
#include <stdexcept>

#ifdef Q_OS_LINUX
#include <poll.h>
#include <sys/socket.h>
#endif

namespace PyroshineUdpProbe {

namespace {
constexpr std::int64_t HelloIntervalUs = 50000, ReportIntervalUs = 100000;
constexpr std::int64_t HelloTimeoutUs = 3000000, SilenceTimeoutUs = 1000000, EndLingerUs = 50000;
constexpr std::uint64_t StopLossPercent = 10;
constexpr int Batch = 64;

bool sameHost(const QHostAddress& a, const QHostAddress& b) {
    return a.isEqual(b, QHostAddress::TolerantConversion);
}
} // namespace

RunResult receive(const StartReply& reply, const QHostAddress& host, const std::function<bool()>& interrupted) {
    QUdpSocket socket;
    const bool v6 = host.protocol() == QAbstractSocket::IPv6Protocol && host.toIPv4Address() == 0;
    if (!socket.bind(v6 ? QHostAddress(QHostAddress::AnyIPv6) : QHostAddress(QHostAddress::AnyIPv4), 0))
        throw std::runtime_error("Cannot open a UDP socket for calibration.");
    RunResult result;
    // Measure with the buffer the stream's video socket would request, so a
    // too-small client buffer shows up here as it would while streaming.
    result.requestedBufferBytes = streamReceiveBufferBytes(reply.packetSize, reply.shape);
    socket.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, result.requestedBufferBytes);
    result.receiveBufferBytes = socket.socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toInt();
#ifdef Q_OS_LINUX
    result.receiveBufferBytes /= 2; // Linux reports twice the effective size.
#endif

    Receiver receiver(reply);
    QElapsedTimer clock;
    clock.start();
    auto now = [&] { return clock.nsecsElapsed() / 1000; };
    std::uint32_t counter = 0;
    auto send = [&](ControlKind kind) {
        const QByteArray control = encodeControl(reply, kind, ++counter,
                                                 kind == Report ? receiver.reportCounts() : std::array<std::uint32_t, MaxSteps>{});
        socket.writeDatagram(control, host, reply.udpPort);
    };
    auto stop = [&](ControlKind kind) { for (int i = 0; i < 3; ++i) send(kind); };

    std::int64_t nextControl = 0, endAt = -1, lastArrival = -1;
    bool stopSent = false;
    std::vector<char> buffer(size_t(Batch) * size_t(reply.shardBytes + 1));

    auto handle = [&](const char* data, int length, const QHostAddress& from) {
        if (!sameHost(from, host)) return;
        std::uint8_t flags = 0;
        const std::int64_t arrival = now();
        if (receiver.accept(data, length, arrival, &flags) < 0) return;
        lastArrival = arrival;
        if ((flags & FlagEndOfRun) && endAt < 0) endAt = arrival + EndLingerUs;
        if (!stopSent) {
            // Stop the ladder after a finished step that lost too much.
            for (int s = 0; s + 1 < receiver.stepCount(); ++s) {
                if (receiver.stepEnded(s) && receiver.lossPercent(s) > StopLossPercent) {
                    stop(Stop);
                    stopSent = true;
                    result.stop = StopReason::Saturated;
                    endAt = arrival + EndLingerUs;
                    break;
                }
            }
        }
    };

    for (;;) {
        const std::int64_t t = now();
        if (endAt >= 0 && t >= endAt) break;
        if (lastArrival < 0 && t >= HelloTimeoutUs)
            throw std::runtime_error("No calibration datagram arrived. UDP to the host's video port may be blocked.");
        if (lastArrival >= 0 && t - lastArrival > SilenceTimeoutUs) {
            if (!stopSent) result.stop = StopReason::Silent;
            break;
        }
        if (interrupted()) {
            stop(Stop);
            result.stop = StopReason::Cancelled;
            break;
        }
        if (t >= nextControl) {
            send(lastArrival < 0 ? Hello : Report);
            nextControl = t + (lastArrival < 0 ? HelloIntervalUs : ReportIntervalUs);
        }
        std::int64_t wake = nextControl;
        if (endAt >= 0) wake = (std::min)(wake, endAt);
        const int waitMs = int(std::clamp<std::int64_t>((wake - now()) / 1000, 0, 20));
#ifdef Q_OS_LINUX
        // recvmmsg drains bursts of thousands of datagrams with few syscalls.
        pollfd fd{int(socket.socketDescriptor()), POLLIN, 0};
        if (poll(&fd, 1, waitMs) <= 0) continue;
        mmsghdr messages[Batch];
        iovec vectors[Batch];
        sockaddr_storage sources[Batch];
        for (;;) {
            for (int i = 0; i < Batch; ++i) {
                vectors[i] = {buffer.data() + size_t(i) * size_t(reply.shardBytes + 1), size_t(reply.shardBytes + 1)};
                messages[i] = {};
                messages[i].msg_hdr.msg_iov = &vectors[i];
                messages[i].msg_hdr.msg_iovlen = 1;
                messages[i].msg_hdr.msg_name = &sources[i];
                messages[i].msg_hdr.msg_namelen = sizeof(sources[i]);
            }
            const int count = recvmmsg(fd.fd, messages, Batch, MSG_DONTWAIT, nullptr);
            if (count <= 0) break;
            for (int i = 0; i < count; ++i)
                handle(static_cast<const char*>(vectors[i].iov_base), int(messages[i].msg_len),
                       QHostAddress(reinterpret_cast<const sockaddr*>(&sources[i])));
            if (count < Batch) break;
        }
#else
        if (!socket.waitForReadyRead(waitMs)) continue;
        while (socket.hasPendingDatagrams()) {
            QHostAddress from;
            const qint64 length = socket.readDatagram(buffer.data(), qint64(buffer.size()), &from, nullptr);
            if (length > 0) handle(buffer.data(), int(length), from);
        }
#endif
    }
    result.steps = receiver.summarize();
    return result;
}

} // namespace PyroshineUdpProbe
