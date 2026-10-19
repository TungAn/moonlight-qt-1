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
#include <array>
#include <limits>

PyroWaveCalibrator::~PyroWaveCalibrator() {
    if (m_Worker) { m_Worker->requestInterruption(); m_Worker->wait(); delete m_Worker; }
}
void PyroWaveCalibrator::cancel() { if (m_Worker) m_Worker->requestInterruption(); }
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
    m_Worker = QThread::create([this, address, certificate, port, trueUid, udp] {
        QString message;
        int ceiling = 0;
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
        } catch (const std::exception& e) { message = QString::fromUtf8(e.what()); }
        if (QThread::currentThread()->isInterruptionRequested()) { message = tr("Calibration stopped."); ceiling = 0; }
        QMetaObject::invokeMethod(this, [this, message, ceiling] { m_Message = message; m_Ceiling = ceiling; m_Running = false; emit changed(); }, Qt::QueuedConnection);
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
