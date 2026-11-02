#include "settings/streamingpreferences.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QMetaProperty>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

// StreamingPreferences only needs these platform queries to choose the normal
// default window mode. Stubbing them keeps this persistence test independent of
// SDL and the window-system integration layer.
namespace WMUtils {
bool isRunningWayland() { return false; }
bool isGpuSlow() { return false; }
}

class StreamingPreferencesTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QVERIFY(m_SettingsDirectory.isValid());
        QCoreApplication::setOrganizationName(QStringLiteral("MoonlightProfileTests"));
        QCoreApplication::setApplicationName(QStringLiteral("MoonlightProfileTests"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                           m_SettingsDirectory.path());
        m_Preferences = StreamingPreferences::get();
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
        m_Preferences->reload();
    }


    void integrationRegistryCoversEveryWritablePreference()
    {
        const auto snapshot = m_Preferences->integrationSnapshot();
        const auto registry = snapshot.value("registry").toArray();
        const auto values = snapshot.value("values").toObject();
        int writable = 0;
        const auto meta = m_Preferences->metaObject();
        for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
            const auto property = meta->property(i);
            if (!property.isWritable()) continue;
            ++writable;
            QVERIFY(values.contains(property.name()));
            QVERIFY(values.value(property.name()).isBool() || values.value(property.name()).isDouble());
            int matches = 0;
            for (const auto& value : registry) {
                const auto descriptor = value.toObject();
                if (descriptor.value("key").toString() != property.name()) continue;
                ++matches;
                QCOMPARE(descriptor.value("application").toString(), QStringLiteral("reconnect"));
                QVERIFY(!descriptor.value("category").toString().isEmpty());
                QVERIFY(descriptor.value("scope").toString() == "global" || descriptor.value("scope").toString() == "profile");
            }
            QCOMPARE(matches, 1);
        }
        QCOMPARE(registry.size(), writable);
        QVERIFY(values.contains("packetSize"));
    }

    void integrationTransactionsRejectStaleWritesAndValidateAtomically()
    {
        auto initial = m_Preferences->integrationSnapshot();
        auto request = QJsonObject{{"schema", 1}, {"operation", "patch"}, {"revision", initial.value("revision")},
            {"values", QJsonObject{{"width", 2560}, {"height", -1}}}};
        QVERIFY(!m_Preferences->integrationRequest(request).value("ok").toBool());
        QCOMPARE(m_Preferences->width, 1920);
        request["values"] = QJsonObject{{"width", 2560}, {"height", 1440}, {"audioConfig", 2}};
        request["operation"] = "validate";
        QVERIFY(m_Preferences->integrationRequest(request).value("ok").toBool());
        QCOMPARE(m_Preferences->width, 1920);
        request["operation"] = "patch";
        auto result = m_Preferences->integrationRequest(request);
        QVERIFY(result.value("ok").toBool());
        QCOMPARE(m_Preferences->width, 2560);
        QCOMPARE(m_Preferences->audioConfig, StreamingPreferences::AC_71_SURROUND);
        QCOMPARE(m_Preferences->bitrateKbps, StreamingPreferences::getDefaultBitrate(2560, 1440, 60, false, 0, false));
        QCOMPARE(m_Preferences->integrationRequest(request).value("error").toObject().value("code").toString(), QStringLiteral("conflict"));
        m_Preferences->reload();
        QCOMPARE(m_Preferences->width, 2560);
        QCOMPARE(m_Preferences->audioConfig, StreamingPreferences::AC_71_SURROUND);
    }

    void integrationGlobalSettingsAndProfileOperationsShareQmlState()
    {
        auto initial = m_Preferences->integrationSnapshot();
        auto created = m_Preferences->integrationRequest({{"schema", 1}, {"operation", "create"},
            {"revision", initial.value("revision")}, {"name", "Deck"}});
        QVERIFY(created.value("ok").toBool());
        QCOMPARE(m_Preferences->profileIds().size(), 2);
        const auto profile = m_Preferences->activeProfileId();
        auto changed = m_Preferences->integrationRequest({{"schema", 1}, {"operation", "patch"},
            {"revision", created.value("revision")}, {"values", QJsonObject{{"backgroundGamepad", false}, {"fps", 90}}}});
        QVERIFY(changed.value("ok").toBool());
        QVERIFY(m_Preferences->activateProfile("default"));
        QCOMPARE(m_Preferences->fps, 60);
        QVERIFY(!m_Preferences->backgroundGamepad);
        // An intervening QML operation invalidates the CLI transaction.
        auto stale = m_Preferences->integrationRequest({{"schema", 1}, {"operation", "rename"},
            {"revision", changed.value("revision")}, {"profileId", profile}, {"name", "TV"}});
        QVERIFY(!stale.value("ok").toBool());
        auto latest = m_Preferences->integrationSnapshot();
        QVERIFY(m_Preferences->integrationRequest({{"schema", 1}, {"operation", "activate"},
            {"revision", latest.value("revision")}, {"profileId", profile}}).value("ok").toBool());
        QCOMPARE(m_Preferences->fps, 90);
        latest = m_Preferences->integrationSnapshot();
        QVERIFY(m_Preferences->integrationRequest({{"schema", 1}, {"operation", "restore"},
            {"revision", latest.value("revision")}}).value("ok").toBool());
        QCOMPARE(m_Preferences->fps, 60);
        QVERIFY(!m_Preferences->backgroundGamepad);
    }

    void integrationCapabilitiesAndVrrDependencies()
    {
        QVERIFY(m_Preferences->integrationSnapshot().value("capabilities").toObject().value("windowReadiness").toBool());
        auto request = QJsonObject{{"schema", 1}, {"operation", "patch"},
            {"revision", m_Preferences->integrationSnapshot().value("revision")},
            {"values", QJsonObject{{"enableVrr", true}, {"enableVsync", false}}}};
        QVERIFY(!m_Preferences->integrationRequest(request).value("ok").toBool());
        request["values"] = QJsonObject{{"enableVrr", true}, {"vrrLatencyMode", 0}, {"vrrBufferPerMille", 750}};
        QVERIFY(m_Preferences->integrationRequest(request).value("ok").toBool());
        QCOMPARE(m_Preferences->vrrBufferPerMille(), 750);
        QCOMPARE(m_Preferences->vrrLatencyMode, 0);
        request["revision"] = m_Preferences->integrationSnapshot().value("revision");
        request["values"] = QJsonObject{{"fps", 59.5}};
        QVERIFY(!m_Preferences->integrationRequest(request).value("ok").toBool());
        request["values"] = QJsonObject{{"fps", true}};
        QVERIFY(!m_Preferences->integrationRequest(request).value("ok").toBool());
        request["values"] = QJsonObject{{"videoCodecConfig", 3}};
        QVERIFY(!m_Preferences->integrationRequest(request).value("ok").toBool());
        request["values"] = QJsonObject{{"recommendedFullScreenMode", 2}};
        QVERIFY(!m_Preferences->integrationRequest(request).value("ok").toBool());

        // Clients such as PyroDeck show the microphone settings under Audio,
        // echo cancellation only with the microphone on.
        for (const auto& value : m_Preferences->integrationSnapshot().value("registry").toArray()) {
            const auto descriptor = value.toObject();
            const QString key = descriptor.value("key").toString();
            if (key == "enableMicrophone" || key == "micEchoCancel") {
                QCOMPARE(descriptor.value("category").toString(), QStringLiteral("Audio"));
                QCOMPARE(descriptor.value("scope").toString(), QStringLiteral("profile"));
            }
            if (key == "micEchoCancel") {
                QCOMPARE(descriptor.value("requires").toString(), QStringLiteral("enableMicrophone"));
                QCOMPARE(descriptor.value("choices").toArray().size(), 3);
            }
        }
        request["revision"] = m_Preferences->integrationSnapshot().value("revision");
        request["values"] = QJsonObject{{"micEchoCancel", 3}};
        QVERIFY(!m_Preferences->integrationRequest(request).value("ok").toBool());
        request["values"] = QJsonObject{{"enableMicrophone", true}, {"micEchoCancel", 2}};
        QVERIFY(m_Preferences->integrationRequest(request).value("ok").toBool());
        QVERIFY(m_Preferences->enableMicrophone);
        QCOMPARE(m_Preferences->micEchoCancel, StreamingPreferences::MEC_OFF);
    }

    void freshInstallCreatesDefault()
    {
        QCOMPARE(m_Preferences->activeProfileId(), QStringLiteral("default"));
        QCOMPARE(m_Preferences->activeProfileName(), QStringLiteral("Default"));
        QCOMPARE(m_Preferences->profileIds(), QStringList{QStringLiteral("default")});
        QCOMPARE(m_Preferences->width, 1920);
        QCOMPARE(m_Preferences->height, 1080);
        QCOMPARE(m_Preferences->fps, 60);

        QSettings settings;
        QCOMPARE(settings.value(QStringLiteral("profiles/version")).toInt(), 1);
        QCOMPARE(settings.value(QStringLiteral("profiles/default/name")).toString(),
                 QStringLiteral("Default"));
    }

    void migratesLegacySettingsExactlyOnce()
    {
        QSettings settings;
        settings.clear();
        settings.setValue(QStringLiteral("width"), 3840);
        settings.setValue(QStringLiteral("height"), 2160);
        settings.setValue(QStringLiteral("fps"), 120);
        settings.setValue(QStringLiteral("bitrate"), 400000);
        settings.setValue(QStringLiteral("autoadjustbitrate"), false);
        settings.setValue(QStringLiteral("videocfg"),
                          static_cast<int>(StreamingPreferences::VCC_FORCE_PYROWAVE));
        settings.setValue(QStringLiteral("hdr"), true);
        settings.setValue(QStringLiteral("yuv444"), true);
        settings.sync();

        m_Preferences->reload();
        QCOMPARE(m_Preferences->width, 3840);
        QCOMPARE(m_Preferences->fps, 120);
        QCOMPARE(m_Preferences->bitrateKbps, 400000);
        QCOMPARE(m_Preferences->videoCodecConfig, StreamingPreferences::VCC_FORCE_PYROWAVE);
        QVERIFY(m_Preferences->enableHdr);
        QVERIFY(m_Preferences->enableYUV444);

        settings.setValue(QStringLiteral("width"), 640);
        settings.sync();
        m_Preferences->reload();
        QCOMPARE(m_Preferences->width, 3840);
        QCOMPARE(m_Preferences->bitrateKbps, 400000);
        QCOMPARE(m_Preferences->videoCodecConfig, StreamingPreferences::VCC_FORCE_PYROWAVE);
        QVERIFY(m_Preferences->enableHdr);
        QVERIFY(m_Preferences->enableYUV444);
    }

    void crudAndActiveProfilePersistence()
    {
        m_Preferences->width = 1280;
        m_Preferences->height = 800;
        m_Preferences->fps = 90;
        m_Preferences->bitrateKbps = 50000;
        m_Preferences->autoAdjustBitrate = false;
        QVERIFY(m_Preferences->createProfile(QStringLiteral("Steam Deck")));
        const QString steamDeckId = m_Preferences->activeProfileId();

        QVERIFY(m_Preferences->duplicateProfile(steamDeckId, QStringLiteral("Steam Deck Copy")));
        const QString copyId = m_Preferences->activeProfileId();
        QVERIFY(m_Preferences->renameProfile(copyId, QStringLiteral("Handheld")));
        QCOMPARE(m_Preferences->activeProfileName(), QStringLiteral("Handheld"));
        QVERIFY(!m_Preferences->createProfile(QStringLiteral(" handheld ")));
        QVERIFY(!m_Preferences->renameProfile(QStringLiteral("default"), QStringLiteral("Other")));
        QVERIFY(!m_Preferences->deleteProfile(QStringLiteral("default")));

        QVERIFY(m_Preferences->deleteProfile(copyId));
        QCOMPARE(m_Preferences->activeProfileId(), QStringLiteral("default"));
        QVERIFY(m_Preferences->activateProfile(steamDeckId));
        m_Preferences->save();
        m_Preferences->reload();
        QCOMPARE(m_Preferences->activeProfileId(), steamDeckId);
        QCOMPARE(m_Preferences->width, 1280);
        QCOMPARE(m_Preferences->height, 800);
        QCOMPARE(m_Preferences->fps, 90);
        QCOMPARE(m_Preferences->bitrateKbps, 50000);
    }

    void vrrSettingsDefaultPersistPerProfileAndKeepFps()
    {
        // Defaults: VRR off, Balanced, Reduce judder on.
        QCOMPARE(m_Preferences->enableVrr, false);
        QCOMPARE(m_Preferences->vrrLatencyMode, int(StreamingPreferences::VLM_BALANCED));
        QVERIFY(m_Preferences->smoothVrrFrameTiming);
        QVERIFY(!m_Preferences->vrrTimingCustomized());
        QCOMPARE(m_Preferences->vrrBufferPerMille(), 1000);

        // Enabling VRR never rewrites the saved frame rate.
        m_Preferences->fps = 144;
        m_Preferences->enableVrr = true;
        m_Preferences->applyVrrPreset(StreamingPreferences::VLM_LOWEST_LATENCY);
        QCOMPARE(m_Preferences->fps, 144);
        QCOMPARE(m_Preferences->vrrBufferPerMille(), 500);
        m_Preferences->setVrrToleranceUs(1100);
        QCOMPARE(m_Preferences->vrrToleranceUs(), 1000);
        m_Preferences->setVrrBufferPerMille(10);
        QCOMPARE(m_Preferences->vrrBufferPerMille(), 250);
        QVERIFY(m_Preferences->vrrTimingCustomized());
        m_Preferences->smoothVrrFrameTiming = false;
        QVERIFY(m_Preferences->createProfile(QStringLiteral("VRR")));
        const QString vrrId = m_Preferences->activeProfileId();
        m_Preferences->save();

        QVERIFY(m_Preferences->activateProfile(QStringLiteral("default")));
        QVERIFY(m_Preferences->activateProfile(vrrId));
        m_Preferences->save();
        m_Preferences->reload();
        QCOMPARE(m_Preferences->activeProfileId(), vrrId);
        QCOMPARE(m_Preferences->enableVrr, true);
        QCOMPARE(m_Preferences->fps, 144);
        QCOMPARE(m_Preferences->vrrLatencyMode, int(StreamingPreferences::VLM_LOWEST_LATENCY));
        QCOMPARE(m_Preferences->vrrToleranceUs(), 1000);
        QCOMPARE(m_Preferences->vrrBufferPerMille(), 250);
        QVERIFY(!m_Preferences->smoothVrrFrameTiming);

        // Selecting a mode restores its preset values.
        m_Preferences->applyVrrPreset(StreamingPreferences::VLM_SMOOTHEST);
        QVERIFY(!m_Preferences->vrrTimingCustomized());
        QCOMPARE(m_Preferences->vrrBufferPerMille(), 4000);
        QCOMPARE(StreamingPreferences::vrrRateForRefresh(120), 116);
        QCOMPARE(StreamingPreferences::lowLatencyVrrRateForRefresh(144), 120);
    }

    void switchingRestoresAllStreamingValuesAndKeepsGlobals()
    {
        QVERIFY(m_Preferences->createProfile(QStringLiteral("Desktop 4K120")));
        const QString desktopId = m_Preferences->activeProfileId();

        m_Preferences->enableMdns = false;
        m_Preferences->language = StreamingPreferences::LANG_FR;
        m_Preferences->width = 3840;
        m_Preferences->height = 2160;
        m_Preferences->fps = 120;
        m_Preferences->bitrateKbps = 400000;
        m_Preferences->autoAdjustBitrate = false;
        m_Preferences->unlockBitrate = true;
        m_Preferences->videoCodecConfig = StreamingPreferences::VCC_FORCE_PYROWAVE;
        m_Preferences->enableHdr = true;
        m_Preferences->enableYUV444 = true;
        m_Preferences->videoDecoderSelection = StreamingPreferences::VDS_FORCE_HARDWARE;
        m_Preferences->rendererSelection = StreamingPreferences::RS_VULKAN;
        m_Preferences->audioConfig = StreamingPreferences::AC_71_SURROUND;
        m_Preferences->audioQuality = StreamingPreferences::AQ_MAXIMUM;
        m_Preferences->enableVsync = false;
        m_Preferences->framePacing = true;
        m_Preferences->gameOptimizations = false;
        m_Preferences->playAudioOnHost = true;
        m_Preferences->showPerformanceOverlay = true;
        m_Preferences->muteOnFocusLoss = true;
        m_Preferences->enableMicrophone = true;
        m_Preferences->micEchoCancel = StreamingPreferences::MEC_OFF;
        m_Preferences->windowMode = StreamingPreferences::WM_WINDOWED;
        m_Preferences->packetSize = 1392;
        m_Preferences->save();

        QVERIFY(m_Preferences->activateProfile(QStringLiteral("default")));
        QCOMPARE(m_Preferences->width, 1920);
        QCOMPARE(m_Preferences->audioQuality, StreamingPreferences::AQ_HOST_DEFAULT);
        // The microphone is off until the user turns it on.
        QVERIFY(!m_Preferences->enableMicrophone);
        QCOMPARE(m_Preferences->micEchoCancel, StreamingPreferences::MEC_AUTO);
        QVERIFY(m_Preferences->activateProfile(desktopId));
        QCOMPARE(m_Preferences->width, 3840);
        QCOMPARE(m_Preferences->height, 2160);
        QCOMPARE(m_Preferences->fps, 120);
        QCOMPARE(m_Preferences->bitrateKbps, 400000);
        QVERIFY(!m_Preferences->autoAdjustBitrate);
        QVERIFY(m_Preferences->enableHdr);
        QVERIFY(m_Preferences->enableYUV444);
        QCOMPARE(m_Preferences->videoCodecConfig, StreamingPreferences::VCC_FORCE_PYROWAVE);
        QVERIFY(m_Preferences->unlockBitrate);
        QCOMPARE(m_Preferences->videoDecoderSelection, StreamingPreferences::VDS_FORCE_HARDWARE);
        QCOMPARE(m_Preferences->rendererSelection, StreamingPreferences::RS_VULKAN);
        QCOMPARE(m_Preferences->audioConfig, StreamingPreferences::AC_71_SURROUND);
        QCOMPARE(m_Preferences->audioQuality, StreamingPreferences::AQ_MAXIMUM);
        QVERIFY(!m_Preferences->enableVsync);
        QVERIFY(m_Preferences->framePacing);
        QVERIFY(!m_Preferences->gameOptimizations);
        QVERIFY(m_Preferences->playAudioOnHost);
        QVERIFY(m_Preferences->showPerformanceOverlay);
        QVERIFY(m_Preferences->muteOnFocusLoss);
        QVERIFY(m_Preferences->enableMicrophone);
        QCOMPARE(m_Preferences->micEchoCancel, StreamingPreferences::MEC_OFF);
        QCOMPARE(m_Preferences->windowMode, StreamingPreferences::WM_WINDOWED);
        QCOMPARE(m_Preferences->packetSize, 1392);
        QVERIFY(!m_Preferences->enableMdns);
        QCOMPARE(m_Preferences->language, StreamingPreferences::LANG_FR);
    }

    void automaticBitrateRecalculatesWhileManualBitrateDoesNot()
    {
        m_Preferences->width = 3840;
        m_Preferences->height = 2160;
        m_Preferences->fps = 120;
        m_Preferences->videoCodecConfig = StreamingPreferences::VCC_FORCE_PYROWAVE;
        m_Preferences->autoAdjustBitrate = true;
        m_Preferences->bitrateKbps = 12345;
        QVERIFY(m_Preferences->createProfile(QStringLiteral("Automatic")));
        const QString automaticId = m_Preferences->activeProfileId();

        QVERIFY(m_Preferences->activateProfile(QStringLiteral("default")));
        QVERIFY(m_Preferences->activateProfile(automaticId));
        QCOMPARE(m_Preferences->bitrateKbps,
                 StreamingPreferences::getDefaultBitrate(3840, 2160, 120, false,
                                                         StreamingPreferences::VCC_FORCE_PYROWAVE,
                                                         false));

        m_Preferences->autoAdjustBitrate = false;
        m_Preferences->bitrateKbps = 412345;
        m_Preferences->save();
        QVERIFY(m_Preferences->activateProfile(QStringLiteral("default")));
        QVERIFY(m_Preferences->activateProfile(automaticId));
        QCOMPARE(m_Preferences->bitrateKbps, 412345);
    }

    void corruptActiveProfileFallsBackAndSignalsUiRefresh()
    {
        QVERIFY(m_Preferences->createProfile(QStringLiteral("Temporary")));
        QSignalSpy loadedSpy(m_Preferences, &StreamingPreferences::profileLoaded);
        QVERIFY(m_Preferences->activateProfile(QStringLiteral("default")));
        QCOMPARE(loadedSpy.count(), 1);

        QSettings settings;
        settings.remove(QStringLiteral("profiles/default"));
        settings.setValue(QStringLiteral("activeProfileId"), QStringLiteral("missing"));
        settings.sync();
        m_Preferences->reload();
        QCOMPARE(m_Preferences->activeProfileId(), QStringLiteral("default"));
        QVERIFY(m_Preferences->profileIds().contains(QStringLiteral("default")));
        QCOMPARE(m_Preferences->width, 1920);
    }

private:
    QTemporaryDir m_SettingsDirectory;
    StreamingPreferences* m_Preferences = nullptr;
};

QTEST_GUILESS_MAIN(StreamingPreferencesTest)

#include "streamingpreferences_test.moc"
