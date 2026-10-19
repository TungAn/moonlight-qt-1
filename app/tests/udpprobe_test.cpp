// Pyroshine UDP calibration receiver: protocol compatibility with the host
// (reply parsing, the shared HMAC vector) and receive statistics.
#include "backend/udpprobe.h"

#include <QtTest>

using namespace PyroshineUdpProbe;

namespace {

// As Pyroshine's StartReply::to_xml writes it (protocol version 1).
QString replyXml(const char* shape = "burst", int fec = 20) {
    return QStringLiteral(
        "<root status_code=\"200\"><Version>1</Version><RunId>07070707070707070707070707070707</RunId>"
        "<Key>0909090909090909090909090909090909090909090909090909090909090909</Key><UdpPort>47998</UdpPort>"
        "<PacketSize>1392</PacketSize><ShardBytes>1408</ShardBytes><WirePacketBytes>1436</WirePacketBytes>"
        "<Fps>60</Fps><StepMs>250</StepMs><Shape>%1</Shape><FecPercent>%2</FecPercent><ChunkPackets>46</ChunkPackets>"
        "<HelloTimeoutMs>3000</HelloTimeoutMs><ConsentTimeoutMs>1000</ConsentTimeoutMs>"
        "<Step><Index>0</Index><OfferedKbps>10339</OfferedKbps><PacketsPerFrame>10</PacketsPerFrame><Frames>4</Frames></Step>"
        "<Step><Index>1</Index><OfferedKbps>19989</OfferedKbps><PacketsPerFrame>20</PacketsPerFrame><Frames>4</Frames></Step>"
        "</root>").arg(QLatin1String(shape)).arg(fec);
}

QByteArray datagram(const StartReply& reply, std::uint32_t sequence, int step, int index, std::uint32_t frame,
                    std::uint8_t flags = 0) {
    QByteArray out(reply.shardBytes, char(0xa5));
    char* d = out.data();
    memcpy(d, "PYB1", 4);
    putBe32(d + 4, getBe32(reply.runId.constData()));
    putBe32(d + 8, sequence);
    d[12] = char(step);
    d[13] = char(flags);
    d[14] = char(index >> 8);
    d[15] = char(index);
    putBe32(d + 16, frame);
    putBe32(d + 20, std::uint32_t(frame * 1000000ull / reply.fps));
    return out;
}

} // namespace

class UdpProbeTest : public QObject {
    Q_OBJECT
private slots:
    void parsesTheHostReply() {
        StartReply reply;
        QString error;
        QVERIFY2(reply.parse(replyXml(), &error), qPrintable(error));
        QCOMPARE(reply.udpPort, std::uint16_t(47998));
        QCOMPARE(reply.shardBytes, 1408);
        QCOMPARE(reply.chunkPackets, 46);
        QCOMPARE(int(reply.steps.size()), 2);
        QCOMPARE(reply.steps[1].packets(), std::uint64_t(80));
        QCOMPARE(reply.shape, Shape::Burst);
        // Another protocol version or malformed values are refused.
        QVERIFY(!reply.parse(QString(replyXml()).replace("<Version>1<", "<Version>2<"), &error));
        QVERIFY(!reply.parse(QString(replyXml()).replace("<Shape>burst<", "<Shape>flood<"), &error));
        QVERIFY(!reply.parse(QString(replyXml()).replace("<PacketsPerFrame>20<", "<PacketsPerFrame>70000<"), &error));
        QVERIFY(!reply.parse(QStringLiteral("<root status_code=\"200\"></root>"), &error));
    }

    // The same vector is asserted by Pyroshine's wire tests (computed with Python's hmac).
    void controlDatagramMatchesTheSharedVector() {
        StartReply reply;
        QString error;
        QVERIFY(reply.parse(replyXml(), &error));
        const QByteArray control = encodeControl(reply, Report, 42, {1, 2, 3, 4, 5, 6, 7, 0xffffffffu});
        QCOMPARE(control.size(), ControlBytes);
        QCOMPARE(control.toHex(),
                 QByteArray("5059433107070707070707070707070707070707020000000000002a0000000100000002000000030000"
                            "0004000000050000000600000007ffffffff798b9b0ab53af8617b131f1300cf0eb1"));
    }

    void countsLossGapsAndUnrecoverableFrames() {
        StartReply reply;
        QString error;
        QVERIFY(reply.parse(replyXml("burst", 20), &error));
        Receiver receiver(reply);
        std::uint8_t flags = 0;
        // Step 0: 4 frames x 10 packets; frame 1 loses 1 (repairable with 20% FEC:
        // 9 data + 1 parity... ceil(10*100/120) = 9 data, 1 repairable), frame 2 loses 3.
        std::uint32_t sequence = 0;
        for (std::uint32_t frame = 0; frame < 4; ++frame) {
            for (int index = 0; index < 10; ++index, ++sequence) {
                if ((frame == 1 && index == 4) || (frame == 2 && index >= 5 && index < 8)) continue;
                const std::uint8_t f = (frame == 3 && index == 9) ? (FlagEndOfFrame | FlagEndOfStep) : 0;
                QCOMPARE(receiver.accept(datagram(reply, sequence, 0, index, frame, f).constData(), reply.shardBytes,
                                         frame * 16667 + 100, &flags), 0);
            }
        }
        // A duplicate, a stray from another run and a truncated datagram.
        QCOMPARE(receiver.accept(datagram(reply, 0, 0, 0, 0).constData(), reply.shardBytes, 70000, &flags), 0);
        QByteArray stray = datagram(reply, 1, 0, 1, 0);
        stray[4] = char(stray[4] ^ 0xff);
        QCOMPARE(receiver.accept(stray.constData(), stray.size(), 70000, &flags), -1);
        QCOMPARE(receiver.accept(stray.constData(), 20, 70000, &flags), -1);
        // Step 1 only reaches frame 1 (the run was stopped).
        for (std::uint32_t frame = 4; frame < 6; ++frame)
            for (int index = 0; index < 20; ++index, ++sequence)
                QCOMPARE(receiver.accept(datagram(reply, sequence, 1, index, frame).constData(), reply.shardBytes,
                                         frame * 16667 + 100, &flags), 1);

        QVERIFY(receiver.stepEnded(0));
        QCOMPARE(receiver.lossPercent(0), std::uint64_t(10));
        const auto steps = receiver.summarize();
        QVERIFY(steps[0] && steps[1]);
        QCOMPARE(steps[0]->received, std::uint64_t(36));
        QCOMPARE(steps[0]->duplicates, std::uint64_t(1));
        QCOMPARE(steps[0]->gapEvents, std::uint64_t(2));
        QCOMPARE(steps[0]->maxGap, std::uint64_t(3));
        QCOMPARE(steps[0]->framesComplete, std::uint64_t(2));
        QCOMPARE(steps[0]->framesUnrecoverable, std::uint64_t(1));
        // An unfinished step is evaluated up to its newest frame only.
        QCOMPARE(steps[1]->received, std::uint64_t(40));
        QCOMPARE(steps[1]->gapEvents, std::uint64_t(0));
        QCOMPARE(steps[1]->framesComplete, std::uint64_t(2));
        QCOMPARE(receiver.reportCounts()[0], 36u);

        const QString query = resultQuery(reply, steps, 1000);
        QVERIFY(query.startsWith("run=07070707070707070707070707070707&s0=36,1,0,2,3,2,1,"));
        QVERIFY(query.contains("&s1=40,0,0,0,0,2,0,"));
        QVERIFY(query.endsWith("&clientlinkmbps=1000"));
    }

    void pacedDelaysFollowChunkDeadlines() {
        StartReply reply;
        QString error;
        QVERIFY(reply.parse(QString(replyXml("paced", 0)).replace("<ChunkPackets>46<", "<ChunkPackets>5<"), &error));
        Receiver receiver(reply);
        std::uint8_t flags = 0;
        // Each 5-packet chunk arrives exactly at its deadline: no queueing delay.
        for (int index = 0; index < 10; ++index) {
            const std::int64_t deadline = std::int64_t(index / 5 * 5) * reply.intervalUs() / 10;
            QCOMPARE(receiver.accept(datagram(reply, index, 0, index, 0).constData(), reply.shardBytes, deadline, &flags), 0);
        }
        const auto steps = receiver.summarize();
        QCOMPARE(steps[0]->queueP95Us, std::uint64_t(0));
        QCOMPARE(steps[0]->queueMaxUs, std::uint64_t(0));
    }

    void methodSelectionAndLadder() {
        QCOMPARE(selectMethod(1, true), Method::Udp);
        QCOMPARE(selectMethod(1, false), Method::Udp);
        QCOMPARE(selectMethod(0, true), Method::Https);  // Older Pyroshine with PyroWave
        QCOMPARE(selectMethod(2, true), Method::Https);  // Newer protocol only
        QCOMPARE(selectMethod(0, false), Method::None);  // Sunshine, GameStream
        QCOMPARE(ladderTopKbps(20000, 0, 0, 3000000), 150000u);
        QCOMPARE(ladderTopKbps(650000, 0, 0, 3000000), 1300000u);
        QCOMPARE(ladderTopKbps(650000, 1000, 2500, 3000000), 950000u);
        QCOMPARE(ladderTopKbps(2000000, 0, 0, 3000000), 3000000u);
        QCOMPARE(ladderTopKbps(80000, 100, 0, 3000000), 95000u);
    }
};

QTEST_GUILESS_MAIN(UdpProbeTest)
#include "udpprobe_test.moc"
