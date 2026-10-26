#include "settingsapi.h"
#include "settings/streamingpreferences.h"

#include <QCryptographicHash>
#include <QDir>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <cstdio>

namespace {
QString endpoint()
{
    // Portable installations must never control a different settings store.
    const QByteArray identity = QSettings().fileName().toUtf8();
    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty()) runtime = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(runtime);
    return runtime +
        "/pyrolight-settings-" + QString::fromLatin1(QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex().left(20));
}
QJsonObject failure(const QString& message)
{
    return {{"schema", 1}, {"ok", false}, {"error", QJsonObject{{"code", "unavailable"}, {"message", message}}}};
}
int output(const QJsonObject& result)
{
    fprintf(stdout, "%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    return result.value("ok").toBool() ? 0 : 1;
}
}

SettingsApi::SettingsApi(StreamingPreferences* preferences, QObject* parent, std::function<bool()> writesAllowed)
    : QObject(parent), m_Preferences(preferences), m_WritesAllowed(std::move(writesAllowed))
{
    m_Server.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&m_Server, &QLocalServer::newConnection, this, [this] {
        while (auto socket = m_Server.nextPendingConnection()) {
            socket->setParent(this);
            socket->setReadBufferSize(65537);
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            QTimer::singleShot(5000, socket, [socket] { socket->abort(); socket->deleteLater(); });
            connect(socket, &QLocalSocket::readyRead, socket, [this, socket, buffer] {
                buffer->append(socket->readAll());
                if (buffer->size() > 65536) { socket->abort(); return; }
                if (!buffer->endsWith('\n')) return;
                const auto document = QJsonDocument::fromJson(*buffer);
                const QString operation = document.object().value("operation").toString();
                const bool readOnly = operation == "read" || operation == "validate" || operation == "defaults";
                const QJsonObject result = !readOnly && !m_WritesAllowed() ?
                    failure("Disconnect the standalone Pyrolight stream before changing its preferences") :
                    document.isObject() ? m_Preferences->integrationRequest(document.object()) : failure("Invalid JSON request");
                socket->write(QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n');
                socket->disconnectFromServer();
            });
        }
    });
}

bool SettingsApi::listen()
{
    m_Lock.reset(new QLockFile(endpoint() + ".lock"));
    if (!m_Lock->tryLock(0)) return false;
    // A headless transaction may have completed between GUI construction and
    // lock acquisition. Load its result before QML can edit an older snapshot.
    m_Preferences->reload();
    QLocalServer::removeServer(endpoint());
    return m_Server.listen(endpoint());
}

int SettingsApi::runCommand(const QStringList& args)
{
    if (args.size() == 3 && args[2] == "--help") {
        fprintf(stdout, "Usage: pyrolight settings [--json REQUEST]\nSchema 1 operations: read, defaults, validate, patch, create, duplicate, rename, delete, activate, restore, prepare-launch.\n");
        return 0;
    }
    QJsonObject request{{"schema", 1}, {"operation", "read"}};
    if (args.size() > 2) {
        if (args.size() != 4 || args[2] != "--json" || args[3].toUtf8().size() > 65536)
            return output(failure("Usage: pyrolight settings [--json REQUEST]"));
        const auto document = QJsonDocument::fromJson(args[3].toUtf8());
        if (!document.isObject()) return output(failure("Expected a JSON object"));
        request = document.object();
    }
    QLocalSocket socket;
    socket.connectToServer(endpoint());
    if (socket.waitForConnected(300)) {
        socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n');
        socket.flush();
        QByteArray response;
        while (!response.endsWith('\n') && response.size() <= 262144) {
            if (!socket.bytesAvailable() && !socket.waitForReadyRead(5000)) break;
            response += socket.readAll();
        }
        const auto result = QJsonDocument::fromJson(response);
        return output(result.isObject() ? result.object() : failure("Settings service did not respond"));
    }
    QLockFile lock(endpoint() + ".lock");
    if (!lock.tryLock(500)) return output(failure("Settings are busy; retry the request"));
    return output(StreamingPreferences::get()->integrationRequest(request));
}
