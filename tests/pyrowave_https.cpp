#include "backend/nvhttp.h"
#include "backend/pyrowavebandwidth.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 5) return 2;
    QTemporaryDir settings;
    QCoreApplication::setOrganizationName("PyroWaveTests");
    QCoreApplication::setApplicationName("HttpsProbe");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    IdentityManager::get();
    QFile certificate(argv[2]);
    if (!certificate.open(QIODevice::ReadOnly)) return 2;
    NvHTTP http(NvAddress("127.0.0.1", 1), QString(argv[1]).toUShort(),
                QSslCertificate(certificate.readAll()), true);
    const QString operation(argv[3]), expected(argv[4]);
    try {
        if (operation == "discover") {
            const auto info = http.getServerInfo(NvHTTP::NVLL_ERROR);
            const bool supported = NvHTTP::getXmlString(info, "PyroWaveBandwidthProbeBytes").toLongLong() == PyroWaveBandwidth::ProbeBytes;
            return supported == (expected == "supported") ? 0 : 1;
        }
        if (operation == "cancel") {
            http.quitApp(42);
            return expected == "success" ? 0 : 1;
        }
        // One warm-up followed by three independent bounded measurements.
        for (int i = 0; i < 4; ++i) {
            if (http.probePyroWaveDownloadKbps() <= 0) return 1;
        }
        return expected == "success" ? 0 : 1;
    } catch (const std::exception& error) {
        std::printf("Probe rejected: %s\n", error.what());
        return expected == "reject" ? 0 : 1;
    }
}
