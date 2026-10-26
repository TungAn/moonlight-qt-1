#include "cli/settingsapi.h"
#include "settings/streamingpreferences.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

namespace WMUtils {
bool isRunningWayland() { return false; }
bool isGpuSlow() { return false; }
}

class SettingsApiTest : public QObject {
    Q_OBJECT
private slots:
    void guiOwnerSerializesExternalCliTransactions()
    {
        auto preferences = StreamingPreferences::get();
        bool writesAllowed = true;
        SettingsApi owner(preferences, nullptr, [&writesAllowed] { return writesAllowed; });
        QVERIFY(owner.listen());
        SettingsApi second(preferences);
        QVERIFY(!second.listen());
        auto request = QJsonObject{{"schema", 1}, {"operation", "patch"},
            {"revision", preferences->integrationSnapshot().value("revision")},
            {"values", QJsonObject{{"fps", 90}}}};
        const QStringList args{"settings", "--json", QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact))};
        QProcess first, stale;
        first.start(QCoreApplication::applicationFilePath(), args);
        stale.start(QCoreApplication::applicationFilePath(), args);
        QTRY_COMPARE(first.state(), QProcess::NotRunning);
        QTRY_COMPARE(stale.state(), QProcess::NotRunning);
        const auto firstResult = QJsonDocument::fromJson(first.readAllStandardOutput()).object();
        const auto staleResult = QJsonDocument::fromJson(stale.readAllStandardOutput()).object();
        QVERIFY(firstResult.value("ok").toBool() != staleResult.value("ok").toBool());
        QCOMPARE(preferences->fps, 90);
        // The GUI's actual singleton, not a separately reloaded preferences copy.
        const auto failed = firstResult.value("ok").toBool() ? staleResult : firstResult;
        QCOMPARE(failed.value("error").toObject().value("code").toString(), QStringLiteral("conflict"));
        writesAllowed = false;
        request["revision"] = preferences->integrationSnapshot().value("revision");
        request["values"] = QJsonObject{{"fps", 120}};
        QProcess blocked;
        blocked.start(QCoreApplication::applicationFilePath(), {"settings", "--json",
            QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact))});
        QTRY_COMPARE(blocked.state(), QProcess::NotRunning);
        QVERIFY(!QJsonDocument::fromJson(blocked.readAllStandardOutput()).object().value("ok").toBool());
        QCOMPARE(preferences->fps, 90);
    }

    void headlessFallbackUsesSamePreferences()
    {
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"settings"});
        QVERIFY(child.waitForFinished());
        QCOMPARE(child.exitCode(), 0);
        auto snapshot = QJsonDocument::fromJson(child.readAllStandardOutput()).object();
        QVERIFY(snapshot.value("ok").toBool());
        QCOMPARE(snapshot.value("values").toObject().value("fps").toInt(), 90);
        QProcess repeated;
        repeated.start(QCoreApplication::applicationFilePath(), {"settings"});
        QVERIFY(repeated.waitForFinished());
        const auto same = QJsonDocument::fromJson(repeated.readAllStandardOutput()).object();
        QCOMPARE(same.value("revision"), snapshot.value("revision"));
        const QJsonObject request{{"schema", 1}, {"operation", "patch"}, {"revision", snapshot.value("revision")},
            {"values", QJsonObject{{"fps", 144}}}};
        QProcess writer;
        writer.start(QCoreApplication::applicationFilePath(), {"settings", "--json",
            QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact))});
        QVERIFY(writer.waitForFinished());
        const auto updated = QJsonDocument::fromJson(writer.readAllStandardOutput()).object();
        QVERIFY(updated.value("ok").toBool());
        QCOMPARE(updated.value("values").toObject().value("fps").toInt(), 144);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName("PyrolightSettingsApiTests");
    QCoreApplication::setApplicationName("PyrolightSettingsApiTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    std::unique_ptr<QTemporaryDir> directory;
    if (qEnvironmentVariableIsEmpty("PYROLIGHT_API_TEST_CONFIG")) {
        directory.reset(new QTemporaryDir());
        if (!directory->isValid()) return 1;
        qputenv("PYROLIGHT_API_TEST_CONFIG", directory->path().toUtf8());
    }
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, qEnvironmentVariable("PYROLIGHT_API_TEST_CONFIG"));
    if (app.arguments().value(1) == "settings") return SettingsApi::runCommand(app.arguments());
    SettingsApiTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "settingsapi_test.moc"
