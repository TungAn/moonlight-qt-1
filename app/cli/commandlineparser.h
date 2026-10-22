#pragma once

#include "settings/streamingpreferences.h"

#include <QMap>
#include <QString>

class GlobalCommandLineParser
{
public:
    enum ParseResult {
        NormalStartRequested,
        StreamRequested,
        QuitRequested,
        PairRequested,
        ListRequested,
    };

    GlobalCommandLineParser();
    virtual ~GlobalCommandLineParser();

    ParseResult parse(const QStringList &args);

};

class QuitCommandLineParser
{
public:
    QuitCommandLineParser();
    virtual ~QuitCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;
    int getAppId() const { return m_AppId; }
    bool integrationEvents() const { return m_IntegrationEvents; }

private:
    QString m_Host;
    int m_AppId = 0;
    bool m_IntegrationEvents = false;
};

class PairCommandLineParser
{
public:
    PairCommandLineParser();
    virtual ~PairCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;
    QString getPredefinedPin() const;

private:
    QString m_Host;
    QString m_PredefinedPin;
};

class StreamCommandLineParser
{
public:
    StreamCommandLineParser();
    virtual ~StreamCommandLineParser();

    // Null preferences permit help/version queries only, without reading settings.
    void parse(const QStringList &args, StreamingPreferences *preferences);

    QString getHost() const;
    QString getAppName() const;
    int getAppId() const { return m_AppId; }
    int terminateAppId() const { return m_TerminateAppId; }
    bool integrationEvents() const { return m_IntegrationEvents; }

private:
    QString m_Host;
    QString m_AppName;
    int m_AppId = 0;
    int m_TerminateAppId = 0;
    bool m_IntegrationEvents = false;
    QMap<QString, StreamingPreferences::WindowMode> m_WindowModeMap;
    QMap<QString, StreamingPreferences::AudioConfig> m_AudioConfigMap;
    QMap<QString, StreamingPreferences::AudioQuality> m_AudioQualityMap;
    QMap<QString, StreamingPreferences::VideoCodecConfig> m_VideoCodecMap;
    QMap<QString, StreamingPreferences::VideoDecoderSelection> m_VideoDecoderMap;
    QMap<QString, StreamingPreferences::CaptureSysKeysMode> m_CaptureSysKeysModeMap;
    QMap<QString, StreamingPreferences::VrrLatencyMode> m_VrrTimingMap;
};

class ListCommandLineParser
{
public:
    ListCommandLineParser();
    virtual ~ListCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;
    bool isPrintCSV() const;
    bool isPrintJSON() const;
    int artworkId() const { return m_ArtworkId; }
    QString networkAction() const { return m_NetworkAction; }
    bool isVerbose() const;

private:
    QString m_Host;
    bool m_PrintCSV;
    bool m_PrintJSON;
    int m_ArtworkId = 0;
    QString m_NetworkAction;
    bool m_Verbose;
};
