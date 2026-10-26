#pragma once

#include <QObject>
#include <QJsonObject>
#include <QLocalServer>
#include <QLockFile>
#include <memory>
#include <functional>

class StreamingPreferences;

// Same-user local IPC only. A GUI owns the preferences; CLI falls back to an
// exclusive headless transaction only when no GUI is serving the settings.
class SettingsApi : public QObject
{
public:
    explicit SettingsApi(StreamingPreferences* preferences, QObject* parent = nullptr,
                         std::function<bool()> writesAllowed = [] { return true; });
    bool listen();
    static int runCommand(const QStringList& args);

private:
    StreamingPreferences* m_Preferences;
    QLocalServer m_Server;
    std::unique_ptr<QLockFile> m_Lock;
    std::function<bool()> m_WritesAllowed;
};
