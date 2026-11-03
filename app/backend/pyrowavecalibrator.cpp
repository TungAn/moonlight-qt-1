#include "pyrowavecalibrator.h"
#include "pyrowavebandwidth.h"
#include "udpproberunner.h"
#include "settings/streamingpreferences.h"
#include "networkbuffers.h"
#include "computermanager.h"
#include "nvcomputer.h"
#include "streaming/session.h"
#include <QHostInfo>
#include <QReadLocker>
#include <QSettings>
#include <array>
#include <limits>

PyroWaveCalibrator::~PyroWaveCalibrator() {
    if (m_Worker) { m_Worker->requestInterruption(); m_Worker->wait(); delete m_Worker; }
}
void PyroWaveCalibrator::cancel() { if (m_Worker) m_Worker->requestInterruption(); }
static QString paceKey(const QString& uuid) { return QStringLiteral("pyrowavepace/") + uuid; }
int PyroWaveCalibrator::storedPaceMbps(const QString& uuid) {
    if (uuid.isEmpty()) return 0;
    const int mbps = QSettings().value(paceKey(uuid), 0).toInt();
    return mbps > 0 && mbps <= 9999 ? mbps : 0;
}
void PyroWaveCalibrator::storePaceMbps(const QString& uuid, int mbps) {
    QSettings settings;
    if (mbps > 0) settings.setValue(paceKey(uuid), mbps);
    else settings.remove(paceKey(uuid));
}
void PyroWaveCalibrator::clearPace(const QString& uuid) {
    storePaceMbps(uuid, 0);
    m_PaceMbps = 0;
    m_Message = tr("The calibrated PyroWave pace for this host was cleared; the host paces PyroWave by itself again.");
    emit changed();
}
QVariantList PyroWaveCalibrator::hosts(ComputerManager* manager) {
    QVariantList result;
    if (!manager) return result;
    for (const auto* pc : manager->getComputers()) {
        QReadLocker lock(&pc->lock);
        if (pc->state == NvComputer::CS_ONLINE && pc->pairState == NvComputer::PS_PAIRED &&
            (pc->udpProbeVersion > 0 || (pc->serverCodecModeSupport & SCM_MASK_PYROWAVE)))
            result.append(QVariantMap{{"name", pc->name}, {"uuid", pc->uuid}, {"address", pc->activeAddress.address()}});
    }
    return result;
}
void PyroWaveCalibrator::start(ComputerManager* manager, const QString& uuid) {
    if (m_Running) return;
    m_Ceiling = 0;
    m_PaceMbps = storedPaceMbps(uuid);
    if (Session::get() != nullptr) { m_Message = tr("End the stream before calibrating."); emit changed(); return; }
    NvAddress address;
    QSslCertificate certificate;
    uint16_t port = 0;
    bool trueUid = true;
    if (manager) for (const auto* pc : manager->getComputers()) {
        QReadLocker lock(&pc->lock);
        if (pc->uuid != uuid || pc->state != NvComputer::CS_ONLINE || pc->pairState != NvComputer::PS_PAIRED ||
            !(pc->udpProbeVersion > 0 || (pc->serverCodecModeSupport & SCM_MASK_PYROWAVE))) continue;
        address = pc->activeAddress; certificate = pc->serverCert;
        port = pc->activeHttpsPort; trueUid = !pc->isNvidiaServerSoftware; break;
    }
    if (address.isNull() || certificate.isNull() || port == 0) {
        m_Message = tr("Select an online, paired Pyroshine host."); emit changed(); return;
    }
    // Probe with the shape, frame rate and packet size of the stream these
    // settings would start. Read here: preferences belong to the GUI thread.
    const auto* prefs = StreamingPreferences::get();
    UdpSettings udp;
    udp.pyroWave = prefs->videoCodecConfig == StreamingPreferences::VCC_FORCE_PYROWAVE;
    udp.fps = prefs->fps;
    udp.bitrateKbps = prefs->bitrateKbps;
    udp.packetSize = prefs->packetSize != 0 ? prefs->packetSize : 1392;
    // Moonlight requests video encryption on fast-AES clients; the host decides.
    udp.encrypted = QThread::idealThreadCount() > 2;
    if (m_Worker) { m_Worker->wait(); delete m_Worker; }
    m_Running = true;
    m_Message = tr("Testing host → client bandwidth…"); emit changed();
    m_Worker = QThread::create([this, address, certificate, port, trueUid, udp, uuid] {
        QString message;
        int ceiling = 0;
        int pace = -1; // -1: this run did not test the pace
        try {
            NvHTTP http(address, port, certificate, trueUid);
            const QString info = http.getServerInfo(NvHTTP::NVLL_ERROR);
            if (NvHTTP::getXmlString(info, "PairStatus") != "1")
                throw std::runtime_error("Host is no longer paired.");
            const auto method = PyroshineUdpProbe::selectMethod(
                NvHTTP::getXmlString(info, "PyroshineUdpProbeVersion").toInt(),
                NvHTTP::getXmlString(info, "PyroWaveBandwidthProbeBytes").toLongLong() == PyroWaveBandwidth::ProbeBytes);
            if (method == PyroshineUdpProbe::Method::None)
                throw std::runtime_error("This host offers neither UDP calibration nor the PyroWave bandwidth probe.");
            if (method == PyroshineUdpProbe::Method::Udp) {
                ceiling = calibrateUdp(http, info, address, udp, &message);
                QMetaObject::invokeMethod(this, [this, message, ceiling] { m_Message = message; m_Ceiling = ceiling; m_Running = false; emit changed(); }, Qt::QueuedConnection);
                return;
            }
            if (NvHTTP::getXmlString(info, "PyroWaveBandwidthProbeBytes").toLongLong() != PyroWaveBandwidth::ProbeBytes)
                throw std::runtime_error("This host does not support the 32 MiB PyroWave bandwidth probe.");
            if (!(NvHTTP::getXmlString(info, "ServerCodecModeSupport").toUInt() & SCM_MASK_PYROWAVE) ||
                NvHTTP::getXmlString(info, "PairStatus") != "1")
                throw std::runtime_error("Host is no longer paired or PyroWave-capable.");
            const auto hostMbps = NvHTTP::getXmlString(info, "PyroWaveHostLinkMbps").toLongLong();
            QHostAddress host(address.address());
            if (host.isNull()) {
                const auto resolved = QHostInfo::fromName(address.address());
                if (resolved.addresses().size() == 1) host = resolved.addresses().first();
            }
            const auto clientMbps = NetworkBuffers::routedWiredLinkMbps(host);
            const auto clientType = NetworkBuffers::routedLinkType(host);
            http.probePyroWaveDownloadKbps();
            std::array<std::int64_t, 3> samples{};
            for (auto& sample : samples) {
                if (QThread::currentThread()->isInterruptionRequested()) throw std::runtime_error("Calibration stopped.");
                sample = http.probePyroWaveDownloadKbps();
            }
            const auto recommended = PyroWaveBandwidth::ceilingKbps(samples, hostMbps, clientMbps);
            ceiling = int((std::min)(recommended, std::int64_t(2000000)));
            const auto known = [](qint64 speed) { return speed > 0 ? QString::number(speed) + " Mbps" : tr("unknown"); };
            message = tr("Measured host → client (HTTPS; the host has no UDP calibration): %1 Mbps\nHost routed Ethernet: %2\nClient route (%5), physical Ethernet capacity: %3\nRecommended video ceiling: %4 Mbps (20% reserved).\nThis HTTPS measurement does not guarantee loss-free UDP streaming.")
                .arg((std::min)({samples[0], samples[1], samples[2]}) / 1000.0, 0, 'f', 1)
                .arg(known(hostMbps), known(clientMbps)).arg(ceiling / 1000.0, 0, 'f', 1).arg(clientType);
            if (NvHTTP::getXmlString(info, "PyroWaveUdpProbeBurstVersion") == "1" &&
                NvHTTP::getXmlString(info, "PyroWaveUdpHandshakeVersion") == "1") {
                QString paceMessage;
                pace = calibratePace(http, info, int(clientMbps), ceiling, udp, &paceMessage);
                message += QStringLiteral("\n\n") + paceMessage;
            }
        } catch (const std::exception& e) { message = QString::fromUtf8(e.what()); }
        if (QThread::currentThread()->isInterruptionRequested()) { message = tr("Calibration stopped."); ceiling = 0; pace = -1; }
        // A finished pace test replaces the stored pace; 0 removes it.
        if (pace >= 0) storePaceMbps(uuid, pace);
        QMetaObject::invokeMethod(this, [this, message, ceiling, pace] {
            m_Message = message; m_Ceiling = ceiling; m_Running = false;
            if (pace >= 0) m_PaceMbps = pace;
            emit changed();
        }, Qt::QueuedConnection);
    });
    m_Worker->start();
}

// UDP calibration (Pyroshine). Errors other than cancellation throw.
int PyroWaveCalibrator::calibrateUdp(NvHTTP& http, const QString& info, const NvAddress& address,
                                     const UdpSettings& settings, QString* message)
{
    using namespace PyroshineUdpProbe;
    QHostAddress host(address.address());
    if (host.isNull()) {
        const auto resolved = QHostInfo::fromName(address.address());
        if (resolved.addresses().isEmpty()) throw std::runtime_error("Cannot resolve the host address.");
        host = resolved.addresses().first();
    }
    const int hostLinkMbps = NvHTTP::getXmlString(info, "PyroWaveHostLinkMbps").toInt();
    const int clientLinkMbps = NetworkBuffers::routedWiredLinkMbps(host);
    const std::uint32_t top = ladderTopKbps(settings.bitrateKbps, hostLinkMbps, clientLinkMbps,
                                            NvHTTP::getXmlString(info, "PyroshineUdpProbeMaxKbps").toUInt());
    const QString arguments = QStringLiteral("version=%1&maxkbps=%2&steps=8&stepms=750&fps=%3&shape=%4&packetsize=%5&encrypted=%6")
        .arg(ProtocolVersion).arg(top).arg(std::clamp(settings.fps, 10, 480))
        .arg(settings.pyroWave ? "paced" : "burst").arg(settings.packetSize).arg(settings.encrypted ? 1 : 0);
    const QString startXml = http.openConnectionToString(http.m_BaseUrlHttps, "pyroshine-udp-probe", arguments, 10000, NvHTTP::NVLL_ERROR);
    NvHTTP::verifyResponseStatus(startXml);
    StartReply reply;
    QString error;
    if (!reply.parse(startXml, &error)) throw std::runtime_error(error.toStdString());

    const RunResult run = receive(reply, host, [] { return QThread::currentThread()->isInterruptionRequested(); });
    // Always submit, so the host reports what it sent even after cancellation.
    const QString resultXml = http.openConnectionToString(http.m_BaseUrlHttps, "pyroshine-udp-probe-result",
                                                          resultQuery(reply, run.steps, clientLinkMbps), 30000, NvHTTP::NVLL_ERROR);
    NvHTTP::verifyResponseStatus(resultXml);
    if (run.stop == StopReason::Cancelled) { *message = tr("Calibration stopped."); return 0; }

    QStringList lines;
    lines << tr("UDP calibration: %1 frames at %2 FPS, %3-byte packets, FEC %4%.")
                 .arg(reply.shape == Shape::Paced ? tr("PyroWave-paced") : tr("burst-sent"))
                 .arg(reply.fps).arg(reply.packetSize).arg(reply.fecPercent);
    int from = 0;
    while ((from = resultXml.indexOf(QStringLiteral("<Step>"), from)) >= 0) {
        const int end = resultXml.indexOf(QStringLiteral("</Step>"), from);
        if (end < 0) break;
        const QString step = resultXml.mid(from, end - from);
        from = end;
        const QString verdict = xmlValue(step, QStringLiteral("Verdict"));
        if (verdict == QLatin1String("not_run")) continue;
        const QString loss = xmlValue(step, QStringLiteral("NetworkLossPpm"));
        lines << tr("%1 Mbps: %2, loss %3%, queueing %4 ms (p95)")
                     .arg(xmlValue(step, QStringLiteral("OfferedKbps")).toDouble() / 1000.0, 0, 'f', 0)
                     .arg(verdict)
                     .arg(loss.isEmpty() ? QStringLiteral("?") : QString::number(loss.toDouble() / 10000.0, 'f', 2))
                     .arg(xmlValue(step, QStringLiteral("QueueP95Us")).toDouble() / 1000.0, 0, 'f', 1);
    }
    const int recommended = xmlValue(resultXml, QStringLiteral("RecommendedKbps")).toInt();
    if (recommended > 0) {
        lines << tr("Usable on the wire: %1 Mbps. Recommended video bitrate: %2 Mbps (confidence %3; %4% reserved).")
                     .arg(xmlValue(resultXml, QStringLiteral("UsableWireKbps")).toDouble() / 1000.0, 0, 'f', 0)
                     .arg(recommended / 1000.0, 0, 'f', 1)
                     .arg(xmlValue(resultXml, QStringLiteral("Confidence")))
                     .arg(xmlValue(resultXml, QStringLiteral("MarginPercent")));
    } else {
        lines << tr("No recommendation.");
    }
    lines << xmlValue(resultXml, QStringLiteral("Reason")).replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    if (run.receiveBufferBytes > 0 && run.receiveBufferBytes < run.requestedBufferBytes)
        lines << tr("This client's UDP receive buffer (%1 KiB) is below what the stream requests (%2 KiB); loss may be local.")
                     .arg(run.receiveBufferBytes / 1024).arg(run.requestedBufferBytes / 1024);
    lines << tr("A short test cannot guarantee bandwidth; repeat it at the time you usually stream.");
    *message = lines.join(QLatin1Char('\n'));
    return int((std::min)(recommended, 2000000));
}

// Frame-pace calibration (Vibeshine's frame-shaped UDP probe, served by Rubylight): the
// stream's bitrate is sent as whole frames at the profile's frame rate, each paced at
// a candidate rate, and graded on frame-tail and aggregate loss (PyroWavePace).
int PyroWaveCalibrator::calibratePace(NvHTTP& http, const QString& info, int clientLinkMbps, int ceilingKbps,
                                      const UdpSettings& settings, QString* message)
{
    using namespace PyroWavePace;
    const int hostLinkMbps = NvHTTP::getXmlString(info, "PyroWaveHostLinkMbps").toInt();
    int linkMbps = 0;
    for (int known : {hostLinkMbps, clientLinkMbps})
        if (known > 0) linkMbps = linkMbps ? (std::min)(linkMbps, known) : known;
    if (linkMbps <= 0) {
        *message = tr("PyroWave pace: not tested, because neither end's wired link speed is known. The stored pace is unchanged.");
        return -1;
    }
    linkMbps = (std::min)(linkMbps, 9999);
    // Probe frames as large as this profile's stream: its bitrate, within what the link carried.
    int budgetKbps = settings.bitrateKbps > 0 ? settings.bitrateKbps : ceilingKbps;
    if (ceilingKbps > 0) budgetKbps = (std::min)(budgetKbps, ceilingKbps);
    budgetKbps = (std::max)(minimumKbps, (std::min)(budgetKbps, 3000000) / minimumKbps * minimumKbps);
    const int fps = (std::max)(10, (std::min)(settings.fps, 500));
    const int packetSize = (std::max)(256, (std::min)(settings.packetSize, 1392));
    const auto interrupted = [] { return QThread::currentThread()->isInterruptionRequested(); };
    Outcome outcome;
    try {
        outcome = search(linkMbps * 1000, budgetKbps / 4 * 5, [&](int paceKbps) {
            const Result result = http.probePyroWaveBurst(budgetKbps, packetSize, fps, (std::max)(paceKbps, budgetKbps));
            qInfo().noquote() << QString("PyroWave pace probe: %1 kbps as %2 FPS frames paced at %3 kbps: sent %4/%5, "
                                         "received %6, loss %7%, worst 100 ms %8%, missing tails %9%, worst frame %10%, "
                                         "host %11 ms, host send retries %12: %13")
                .arg(budgetKbps).arg(fps).arg(paceKbps).arg(result.sent).arg(result.expected).arg(result.received)
                .arg(result.lossPercent, 0, 'f', 3).arg(result.worstWindowLossPercent, 0, 'f', 3)
                .arg(result.missingTailFramePercent(), 0, 'f', 3).arg(result.worstFrameLossPercent, 0, 'f', 3)
                .arg(result.senderMs, 0, 'f', 1).arg(result.hostSendRetries)
                .arg(qualified(result) ? "pass" : "fail");
            return result;
        }, interrupted);
    } catch (const std::exception& e) {
        *message = tr("PyroWave pace: the test failed (%1). The stored pace is unchanged.").arg(QString::fromUtf8(e.what()));
        return -1;
    }
    if (interrupted()) return -1;
    if (!outcome.paceKbps || !outcome.qualified) {
        *message = outcome.paceKbps ?
            tr("PyroWave pace: no tested pace met every loss limit (best %1 Mbps, missing frame tails %2%). "
               "No pace is sent; the host paces PyroWave by itself.")
                .arg(outcome.paceKbps / 1000).arg(outcome.probe.missingTailFramePercent(), 0, 'f', 2) :
            tr("PyroWave pace: no usable measurement. No pace is sent; the host paces PyroWave by itself.");
        return 0;
    }
    const int paceMbps = outcome.paceKbps / 1000;
    *message = tr("PyroWave pace: %1 Mbps confirmed for %2 Mbps of %3 FPS frames on this %4 Mbps path "
                  "(loss %5%, missing frame tails %6%). It is sent to this host with every PyroWave stream.")
        .arg(paceMbps).arg(budgetKbps / 1000).arg(fps).arg(linkMbps)
        .arg(outcome.probe.lossPercent, 0, 'f', 2).arg(outcome.probe.missingTailFramePercent(), 0, 'f', 2);
    return paceMbps;
}
