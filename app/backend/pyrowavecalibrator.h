#pragma once
#include <QObject>
#include <QVariantList>
#include <QThread>
class ComputerManager;
class NvHTTP;
class NvAddress;
class PyroWaveCalibrator : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(int ceilingKbps READ ceilingKbps NOTIFY changed)
    // The PyroWave pace stored for the host this calibration ran against; 0 when none.
    Q_PROPERTY(int paceMbps READ paceMbps NOTIFY changed)
public:
    explicit PyroWaveCalibrator(QObject* parent = nullptr) : QObject(parent) {}
    ~PyroWaveCalibrator();
    bool running() const { return m_Running; }
    QString message() const { return m_Message; }
    int ceilingKbps() const { return m_Ceiling; }
    int paceMbps() const { return m_PaceMbps; }
    // The calibrated PyroWave frame pace stored for a host, in Mbps; 0 when none.
    Q_INVOKABLE static int storedPaceMbps(const QString& uuid);
    Q_INVOKABLE void clearPace(const QString& uuid);
    Q_INVOKABLE QVariantList hosts(ComputerManager* manager);
    Q_INVOKABLE void start(ComputerManager* manager, const QString& uuid);
    Q_INVOKABLE void cancel();
signals:
    void changed();
private:
    struct UdpSettings { bool pyroWave = false, encrypted = false; int fps = 60, bitrateKbps = 0, packetSize = 1392; };
    int calibrateUdp(NvHTTP& http, const QString& info, const NvAddress& address, const UdpSettings& settings, QString* message);
    // Frame-pace search on hosts with Vibeshine's frame-shaped UDP probe (Rubylight).
    // Returns the qualified pace in Mbps, 0 when none qualified, or -1 when the
    // pace was not tested; the reason is in `message`.
    static int calibratePace(NvHTTP& http, const QString& info, int clientLinkMbps, int ceilingKbps,
                             const UdpSettings& settings, QString* message);
    static void storePaceMbps(const QString& uuid, int mbps);
    QThread* m_Worker = nullptr;
    bool m_Running = false;
    int m_Ceiling = 0;
    int m_PaceMbps = 0;
    QString m_Message;
};
