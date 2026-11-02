#include "streamingpreferences.h"
#include "bitratecalculator.h"
#include "streaming/vrrratepolicy.h"
#include "brandtranslator.h"
#include "utils.h"

#include <QSettings>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QMetaProperty>
#include <QMetaEnum>
#include <QSet>
#include <QSignalBlocker>
#include <cmath>
#include <QCoreApplication>
#include <QLocale>
#include <QReadWriteLock>
#include <QRegularExpression>
#include <QUuid>
#include <QtMath>

#include <algorithm>

#include <QtDebug>

#define SER_STREAMSETTINGS "streamsettings"
#define SER_WIDTH "width"
#define SER_HEIGHT "height"
#define SER_FPS "fps"
#define SER_BITRATE "bitrate"
#define SER_UNLOCK_BITRATE "unlockbitrate"
#define SER_AUTOADJUSTBITRATE "autoadjustbitrate"
#define SER_FULLSCREEN "fullscreen"
#define SER_VSYNC "vsync"
#define SER_GAMEOPTS "gameopts"
#define SER_HOSTAUDIO "hostaudio"
#define SER_MULTICONT "multicontroller"
#define SER_AUDIOCFG "audiocfg"
#define SER_AUDIOQUALITY "audioquality"
#define SER_MICROPHONE "microphone"
#define SER_MICECHOCANCEL "micechocancel"
#define SER_VIDEOCFG "videocfg"
#define SER_HDR "hdr"
#define SER_YUV444 "yuv444"
#define SER_VIDEODEC "videodec"
#define SER_WINDOWMODE "windowmode"
#define SER_MDNS "mdns"
#define SER_QUITAPPAFTER "quitAppAfter"
#define SER_ABSMOUSEMODE "mouseacceleration"
#define SER_ABSTOUCHMODE "abstouchmode"
#define SER_STARTWINDOWED "startwindowed"
#define SER_FRAMEPACING "framepacing"
#define SER_CONNWARNINGS "connwarnings"
#define SER_CONFWARNINGS "confwarnings"
#define SER_UIDISPLAYMODE "uidisplaymode"
#define SER_RICHPRESENCE "richpresence"
#define SER_GAMEPADMOUSE "gamepadmouse"
#define SER_DEFAULTVER "defaultver"
#define SER_PACKETSIZE "packetsize"
#define SER_DETECTNETBLOCKING "detectnetblocking"
#define SER_SHOWPERFOVERLAY "showperfoverlay"
#define SER_SWAPMOUSEBUTTONS "swapmousebuttons"
#define SER_MUTEONFOCUSLOSS "muteonfocusloss"
#define SER_BACKGROUNDGAMEPAD "backgroundgamepad"
#define SER_REVERSESCROLL "reversescroll"
#define SER_SWAPFACEBUTTONS "swapfacebuttons"
#define SER_CAPTURESYSKEYS "capturesyskeys"
#define SER_KEEPAWAKE "keepawake"
#define SER_LANGUAGE "language"
#define SER_RENDERER "renderer"
#define SER_ENABLEVRR "enablevrr"
#define SER_VRRLATENCYMODE "vrrlatencymode"
#define SER_VRRBUFFERPERMILLE "vrrbufferpermille"
#define SER_VRRTARGETHUNDREDTHS "vrrtargethundredths"
#define SER_VRRHISTORYSECONDS "vrrhistoryseconds"
#define SER_VRRTOLERANCEUS "vrrtoleranceus"
#define SER_SMOOTHVRRFRAMETIMING "smoothvrrframetiming"

#define SER_PROFILE_VERSION "profiles/version"
#define SER_ACTIVE_PROFILE "activeProfileId"
#define PROFILE_VERSION 1

static const QString DEFAULT_PROFILE_ID = QStringLiteral("default");

#define CURRENT_DEFAULT_VER 2

static StreamingPreferences* s_GlobalPrefs;

Q_GLOBAL_STATIC(QReadWriteLock, s_GlobalPrefsLock)

StreamingPreferences::StreamingPreferences(QQmlEngine *qmlEngine)
    : m_QmlEngine(qmlEngine)
{
    reload();
}

StreamingPreferences* StreamingPreferences::get(QQmlEngine *qmlEngine)
{
    {
        QReadLocker readGuard(s_GlobalPrefsLock);

        // If we have a preference object and it's associated with a QML engine or
        // if the caller didn't specify a QML engine, return the existing object.
        if (s_GlobalPrefs && (s_GlobalPrefs->m_QmlEngine || !qmlEngine)) {
            // The lifetime logic here relies on the QML engine also being a singleton.
            Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            return s_GlobalPrefs;
        }
    }

    {
        QWriteLocker writeGuard(s_GlobalPrefsLock);

        // If we already have an preference object but the QML engine is now available,
        // associate the QML engine with the preferences.
        if (s_GlobalPrefs) {
            if (!s_GlobalPrefs->m_QmlEngine) {
                s_GlobalPrefs->m_QmlEngine = qmlEngine;
            }
            else {
                // We could reach this codepath if another thread raced with us
                // and created the object while we were outside the pref lock.
                Q_ASSERT(!qmlEngine || s_GlobalPrefs->m_QmlEngine == qmlEngine);
            }
        }
        else {
            s_GlobalPrefs = new StreamingPreferences(qmlEngine);
        }

        return s_GlobalPrefs;
    }
}

void StreamingPreferences::initializeRecommendedFullScreenMode()
{
#ifdef Q_OS_DARWIN
    recommendedFullScreenMode = WindowMode::WM_FULLSCREEN_DESKTOP;
#else
    // Wayland doesn't support modesetting, so use fullscreen desktop mode
    // unless a slow GPU benefits from wp_viewporter scaling.
    recommendedFullScreenMode = WMUtils::isRunningWayland() && !WMUtils::isGpuSlow() ?
                                WindowMode::WM_FULLSCREEN_DESKTOP :
                                WindowMode::WM_FULLSCREEN;
#endif
}

void StreamingPreferences::loadLegacySettings(QSettings& settings)
{
    width = settings.value(SER_WIDTH, 1920).toInt();
    height = settings.value(SER_HEIGHT, 1080).toInt();
    fps = settings.value(SER_FPS, 60).toInt();
    enableYUV444 = settings.value(SER_YUV444, false).toBool();
    enableHdr = settings.value(SER_HDR, false).toBool();
    videoCodecConfig = static_cast<VideoCodecConfig>(settings.value(SER_VIDEOCFG,
                                                  static_cast<int>(VideoCodecConfig::VCC_AUTO)).toInt());
    unlockBitrate = settings.value(SER_UNLOCK_BITRATE, false).toBool();
    autoAdjustBitrate = settings.value(SER_AUTOADJUSTBITRATE, true).toBool();
    const int calculatedDefault = getDefaultBitrate(width, height, fps, enableYUV444,
                                                     videoCodecConfig, enableHdr);
    bitrateKbps = autoAdjustBitrate ? calculatedDefault :
                  settings.value(SER_BITRATE, calculatedDefault).toInt();
    enableVsync = settings.value(SER_VSYNC, true).toBool();
    gameOptimizations = settings.value(SER_GAMEOPTS, true).toBool();
    playAudioOnHost = settings.value(SER_HOSTAUDIO, false).toBool();
    multiController = settings.value(SER_MULTICONT, true).toBool();
    enableMdns = settings.value(SER_MDNS, true).toBool();
    quitAppAfter = settings.value(SER_QUITAPPAFTER, false).toBool();
    absoluteMouseMode = settings.value(SER_ABSMOUSEMODE, false).toBool();
    absoluteTouchMode = settings.value(SER_ABSTOUCHMODE, true).toBool();
    framePacing = settings.value(SER_FRAMEPACING, false).toBool();
    enableVrr = settings.value(SER_ENABLEVRR, false).toBool();
    vrrLatencyMode = qBound(int(VLM_SMOOTHEST), settings.value(SER_VRRLATENCYMODE, int(VLM_BALANCED)).toInt(),
                            int(VLM_LOWEST_LATENCY));
    m_VrrTimingOptions = VrrTimingOptions{
        settings.value(SER_VRRBUFFERPERMILLE, 0).toInt(),
        settings.value(SER_VRRTARGETHUNDREDTHS, 0).toInt(),
        settings.value(SER_VRRHISTORYSECONDS, 0).toInt(),
        settings.value(SER_VRRTOLERANCEUS, 0).toInt(),
    }.resolved(vrrLatencyMode);
    smoothVrrFrameTiming = settings.value(SER_SMOOTHVRRFRAMETIMING, true).toBool();
    connectionWarnings = settings.value(SER_CONNWARNINGS, true).toBool();
    configurationWarnings = settings.value(SER_CONFWARNINGS, true).toBool();
    richPresence = settings.value(SER_RICHPRESENCE, true).toBool();
    gamepadMouse = settings.value(SER_GAMEPADMOUSE, true).toBool();
    detectNetworkBlocking = settings.value(SER_DETECTNETBLOCKING, true).toBool();
    showPerformanceOverlay = settings.value(SER_SHOWPERFOVERLAY, false).toBool();
    packetSize = settings.value(SER_PACKETSIZE, 0).toInt();
    swapMouseButtons = settings.value(SER_SWAPMOUSEBUTTONS, false).toBool();
    muteOnFocusLoss = settings.value(SER_MUTEONFOCUSLOSS, false).toBool();
    backgroundGamepad = settings.value(SER_BACKGROUNDGAMEPAD, false).toBool();
    reverseScrollDirection = settings.value(SER_REVERSESCROLL, false).toBool();
    swapFaceButtons = settings.value(SER_SWAPFACEBUTTONS, false).toBool();
    keepAwake = settings.value(SER_KEEPAWAKE, true).toBool();
    captureSysKeysMode = static_cast<CaptureSysKeysMode>(settings.value(SER_CAPTURESYSKEYS,
                                                         static_cast<int>(CaptureSysKeysMode::CSK_OFF)).toInt());
    audioConfig = static_cast<AudioConfig>(settings.value(SER_AUDIOCFG,
                                                  static_cast<int>(AudioConfig::AC_STEREO)).toInt());
    audioQuality = static_cast<AudioQuality>(qBound(int(AQ_HOST_DEFAULT),
                                                    settings.value(SER_AUDIOQUALITY, int(AQ_HOST_DEFAULT)).toInt(),
                                                    int(AQ_MAXIMUM)));
    enableMicrophone = settings.value(SER_MICROPHONE, false).toBool();
    micEchoCancel = static_cast<MicEchoCancel>(qBound(int(MEC_AUTO),
                                                      settings.value(SER_MICECHOCANCEL, int(MEC_AUTO)).toInt(),
                                                      int(MEC_OFF)));
    videoDecoderSelection = static_cast<VideoDecoderSelection>(settings.value(SER_VIDEODEC,
                                                  static_cast<int>(VideoDecoderSelection::VDS_AUTO)).toInt());
    rendererSelection = static_cast<RendererSelection>(settings.value(SER_RENDERER,
                                                  static_cast<int>(RendererSelection::RS_AUTO)).toInt());
    windowMode = static_cast<WindowMode>(settings.value(SER_WINDOWMODE,
                                                        static_cast<int>(settings.value(SER_FULLSCREEN, true).toBool() ?
                                                                             recommendedFullScreenMode : WindowMode::WM_WINDOWED)).toInt());
    uiDisplayMode = static_cast<UIDisplayMode>(settings.value(SER_UIDISPLAYMODE,
                                               static_cast<int>(settings.value(SER_STARTWINDOWED, true).toBool() ? UIDisplayMode::UI_WINDOWED
                                                                                                                 : UIDisplayMode::UI_MAXIMIZED)).toInt());
    language = static_cast<Language>(settings.value(SER_LANGUAGE,
                                                    static_cast<int>(Language::LANG_AUTO)).toInt());
}

void StreamingPreferences::reload()
{
    QSettings settings;

    int defaultVer = settings.value(SER_DEFAULTVER, 0).toInt();

    initializeRecommendedFullScreenMode();

    // Load the legacy flat settings first. Before profiles are initialized these
    // values are the migration source; afterwards only the global subset remains
    // authoritative and the active profile replaces the session settings below.
    loadLegacySettings(settings);

    // Perform default settings updates as required based on last default version
    if (defaultVer < 1) {
#ifdef Q_OS_DARWIN
        // Update window mode setting on macOS from full-screen (old default) to borderless windowed (new default)
        if (windowMode == WindowMode::WM_FULLSCREEN) {
            windowMode = WindowMode::WM_FULLSCREEN_DESKTOP;
        }
#endif
    }
    if (defaultVer < 2) {
        if (windowMode == WindowMode::WM_FULLSCREEN && WMUtils::isRunningWayland()) {
            windowMode = WindowMode::WM_FULLSCREEN_DESKTOP;
        }
    }

    // Fixup VCC value to the new settings format with codec and HDR separate
    if (videoCodecConfig == VCC_FORCE_HEVC_HDR_DEPRECATED) {
        videoCodecConfig = VCC_AUTO;
        enableHdr = true;
    }

    initializeProfiles(settings);
}

bool StreamingPreferences::retranslate()
{
    static QTranslator* translator = nullptr;

#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
    if (m_QmlEngine != nullptr) {
        // Dynamic retranslation is not supported until Qt 5.10
        return false;
    }
#endif

    QTranslator* newTranslator = new BrandTranslator();
    QString languageSuffix = getSuffixFromLanguage(language);

    // Remove the old translator, even if we can't load a new one.
    // Otherwise we'll be stuck with the old translated values instead
    // of defaulting to English.
    if (translator != nullptr) {
        QCoreApplication::removeTranslator(translator);
        delete translator;
        translator = nullptr;
    }

    if (newTranslator->load(QString(":/languages/qml_") + languageSuffix)) {
        qInfo() << "Successfully loaded translation for" << languageSuffix;
    }
    else {
        qInfo() << "No translation available for" << languageSuffix;
    }

    // Install even without a catalog: English UI strings need branding too.
    translator = newTranslator;
    QCoreApplication::installTranslator(translator);

    if (m_QmlEngine != nullptr) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
        // This is a dynamic retranslation from the settings page.
        // We have to kick the QML engine into reloading our text.
        m_QmlEngine->retranslate();
#else
        // Unreachable below Qt 5.10 due to the check above
        Q_ASSERT(false);
#endif
    }
    else {
        // This is a translation from a non-QML context, which means
        // it is probably app startup. There's nothing to refresh.
    }

    return true;
}

QString StreamingPreferences::getSuffixFromLanguage(StreamingPreferences::Language lang)
{
    switch (lang)
    {
    case LANG_DE:
        return "de";
    case LANG_EN:
        return "en";
    case LANG_FR:
        return "fr";
    case LANG_ZH_CN:
        return "zh_CN";
    case LANG_NB_NO:
        return "nb_NO";
    case LANG_RU:
        return "ru";
    case LANG_ES:
        return "es";
    case LANG_JA:
        return "ja";
    case LANG_VI:
        return "vi";
    case LANG_TH:
        return "th";
    case LANG_KO:
        return "ko";
    case LANG_HU:
        return "hu";
    case LANG_NL:
        return "nl";
    case LANG_SV:
        return "sv";
    case LANG_TR:
        return "tr";
    case LANG_UK:
        return "uk";
    case LANG_ZH_TW:
        return "zh_TW";
    case LANG_PT:
        return "pt";
    case LANG_PT_BR:
        return "pt_BR";
    case LANG_EL:
        return "el";
    case LANG_IT:
        return "it";
    case LANG_HI:
        return "hi";
    case LANG_PL:
        return "pl";
    case LANG_CS:
        return "cs";
    case LANG_HE:
        return "he";
    case LANG_CKB:
        return "ckb";
    case LANG_LT:
        return "lt";
    case LANG_ET:
        return "et";
    case LANG_BG:
        return "bg";
    case LANG_EO:
        return "eo";
    case LANG_TA:
        return "ta";
    case LANG_AUTO:
    default:
        return QLocale::system().name();
    }
}

void StreamingPreferences::save()
{
    QSettings settings;
    saveActiveProfile(settings);
    saveGlobalSettings(settings);
    settings.setValue(SER_ACTIVE_PROFILE, m_ActiveProfileId);
}

QVariantMap StreamingPreferences::profileSettings() const
{
    return {
        {SER_WIDTH, width},
        {SER_HEIGHT, height},
        {SER_FPS, fps},
        {SER_BITRATE, bitrateKbps},
        {SER_UNLOCK_BITRATE, unlockBitrate},
        {SER_AUTOADJUSTBITRATE, autoAdjustBitrate},
        {SER_VSYNC, enableVsync},
        {SER_GAMEOPTS, gameOptimizations},
        {SER_HOSTAUDIO, playAudioOnHost},
        {SER_FRAMEPACING, framePacing},
        {SER_PACKETSIZE, packetSize},
        {SER_SHOWPERFOVERLAY, showPerformanceOverlay},
        {SER_AUDIOCFG, static_cast<int>(audioConfig)},
        {SER_AUDIOQUALITY, static_cast<int>(audioQuality)},
        {SER_MICROPHONE, enableMicrophone},
        {SER_MICECHOCANCEL, static_cast<int>(micEchoCancel)},
        {SER_HDR, enableHdr},
        {SER_YUV444, enableYUV444},
        {SER_VIDEOCFG, static_cast<int>(videoCodecConfig)},
        {SER_VIDEODEC, static_cast<int>(videoDecoderSelection)},
        {SER_RENDERER, static_cast<int>(rendererSelection)},
        {SER_WINDOWMODE, static_cast<int>(windowMode)},
        {SER_MUTEONFOCUSLOSS, muteOnFocusLoss},
        {SER_ENABLEVRR, enableVrr},
        {SER_VRRLATENCYMODE, vrrLatencyMode},
        {SER_VRRBUFFERPERMILLE, m_VrrTimingOptions.bufferPerMille},
        {SER_VRRTARGETHUNDREDTHS, m_VrrTimingOptions.targetHundredths},
        {SER_VRRHISTORYSECONDS, m_VrrTimingOptions.historySeconds},
        {SER_VRRTOLERANCEUS, m_VrrTimingOptions.toleranceUs},
        {SER_SMOOTHVRRFRAMETIMING, smoothVrrFrameTiming},
    };
}

QVariantMap StreamingPreferences::defaultProfileSettings() const
{
    QVariantMap defaults {
        {SER_WIDTH, 1920},
        {SER_HEIGHT, 1080},
        {SER_FPS, 60},
        {SER_UNLOCK_BITRATE, false},
        {SER_AUTOADJUSTBITRATE, true},
        {SER_VSYNC, true},
        {SER_GAMEOPTS, true},
        {SER_HOSTAUDIO, false},
        {SER_FRAMEPACING, false},
        {SER_PACKETSIZE, 0},
        {SER_SHOWPERFOVERLAY, false},
        {SER_AUDIOCFG, static_cast<int>(AudioConfig::AC_STEREO)},
        {SER_AUDIOQUALITY, static_cast<int>(AudioQuality::AQ_HOST_DEFAULT)},
        {SER_MICROPHONE, false},
        {SER_MICECHOCANCEL, static_cast<int>(MicEchoCancel::MEC_AUTO)},
        {SER_HDR, false},
        {SER_YUV444, false},
        {SER_VIDEOCFG, static_cast<int>(VideoCodecConfig::VCC_AUTO)},
        {SER_VIDEODEC, static_cast<int>(VideoDecoderSelection::VDS_AUTO)},
        {SER_RENDERER, static_cast<int>(RendererSelection::RS_AUTO)},
        {SER_WINDOWMODE, static_cast<int>(recommendedFullScreenMode)},
        {SER_MUTEONFOCUSLOSS, false},
        {SER_ENABLEVRR, false},
        {SER_VRRLATENCYMODE, static_cast<int>(VLM_BALANCED)},
        {SER_VRRBUFFERPERMILLE, VrrTimingOptions::preset(VLM_BALANCED).bufferPerMille},
        {SER_VRRTARGETHUNDREDTHS, VrrTimingOptions::preset(VLM_BALANCED).targetHundredths},
        {SER_VRRHISTORYSECONDS, VrrTimingOptions::preset(VLM_BALANCED).historySeconds},
        {SER_VRRTOLERANCEUS, VrrTimingOptions::preset(VLM_BALANCED).toleranceUs},
        {SER_SMOOTHVRRFRAMETIMING, true},
    };
    defaults.insert(SER_BITRATE, getDefaultBitrate(1920, 1080, 60, false, VCC_AUTO, false));
    return defaults;
}

QVariantMap StreamingPreferences::readProfileSettings(QSettings& settings,
                                                       const QString& profileId) const
{
    QVariantMap values = defaultProfileSettings();
    settings.beginGroup(QStringLiteral("profiles/") + profileId);
    for (auto it = values.begin(); it != values.end(); ++it) {
        it.value() = settings.value(it.key(), it.value());
    }
    settings.endGroup();
    return values;
}

void StreamingPreferences::writeProfileSettings(QSettings& settings, const QString& profileId,
                                                 const QVariantMap& values) const
{
    settings.beginGroup(QStringLiteral("profiles/") + profileId);
    const QVariantMap defaults = defaultProfileSettings();
    for (auto it = defaults.cbegin(); it != defaults.cend(); ++it) {
        settings.setValue(it.key(), values.value(it.key(), it.value()));
    }
    settings.endGroup();
}

void StreamingPreferences::applyProfileSettings(const QVariantMap& values)
{
    const QVariantMap defaults = defaultProfileSettings();
    auto value = [&values, &defaults](const char* key) {
        return values.value(QString::fromLatin1(key), defaults.value(QString::fromLatin1(key)));
    };
    auto boundedInt = [&value](const char* key, int minimum, int maximum, int fallback) {
        bool ok = false;
        const int result = value(key).toInt(&ok);
        return ok && result >= minimum && result <= maximum ? result : fallback;
    };

    width = boundedInt(SER_WIDTH, 1, 16384, 1920);
    height = boundedInt(SER_HEIGHT, 1, 16384, 1080);
    fps = boundedInt(SER_FPS, 1, 1000, 60);
    unlockBitrate = value(SER_UNLOCK_BITRATE).toBool();
    autoAdjustBitrate = value(SER_AUTOADJUSTBITRATE).toBool();
    enableVsync = value(SER_VSYNC).toBool();
    gameOptimizations = value(SER_GAMEOPTS).toBool();
    playAudioOnHost = value(SER_HOSTAUDIO).toBool();
    framePacing = value(SER_FRAMEPACING).toBool();
    packetSize = boundedInt(SER_PACKETSIZE, 0, 65535, 0);
    showPerformanceOverlay = value(SER_SHOWPERFOVERLAY).toBool();
    muteOnFocusLoss = value(SER_MUTEONFOCUSLOSS).toBool();

    const int audio = boundedInt(SER_AUDIOCFG, AC_STEREO, AC_71_SURROUND, AC_STEREO);
    audioConfig = static_cast<AudioConfig>(audio);
    audioQuality = static_cast<AudioQuality>(
        boundedInt(SER_AUDIOQUALITY, AQ_HOST_DEFAULT, AQ_MAXIMUM, AQ_HOST_DEFAULT));
    enableMicrophone = value(SER_MICROPHONE).toBool();
    micEchoCancel = static_cast<MicEchoCancel>(boundedInt(SER_MICECHOCANCEL, MEC_AUTO, MEC_OFF, MEC_AUTO));
    enableHdr = value(SER_HDR).toBool();
    enableYUV444 = value(SER_YUV444).toBool();
    int codec = boundedInt(SER_VIDEOCFG, VCC_AUTO, VCC_FORCE_PYROWAVE, VCC_AUTO);
    if (codec == VCC_FORCE_HEVC_HDR_DEPRECATED) {
        codec = VCC_AUTO;
        enableHdr = true;
    }
    videoCodecConfig = static_cast<VideoCodecConfig>(codec);
    videoDecoderSelection = static_cast<VideoDecoderSelection>(
        boundedInt(SER_VIDEODEC, VDS_AUTO, VDS_FORCE_SOFTWARE, VDS_AUTO));
    rendererSelection = static_cast<RendererSelection>(
        boundedInt(SER_RENDERER, RS_AUTO, RS_AVSBDL, RS_AUTO));
    windowMode = static_cast<WindowMode>(
        boundedInt(SER_WINDOWMODE, WM_FULLSCREEN, WM_WINDOWED, recommendedFullScreenMode));
    enableVrr = value(SER_ENABLEVRR).toBool();
    vrrLatencyMode = boundedInt(SER_VRRLATENCYMODE, VLM_SMOOTHEST, VLM_LOWEST_LATENCY, VLM_BALANCED);
    m_VrrTimingOptions = VrrTimingOptions{
        boundedInt(SER_VRRBUFFERPERMILLE, 0, 100000, 0),
        boundedInt(SER_VRRTARGETHUNDREDTHS, 0, 100000, 0),
        boundedInt(SER_VRRHISTORYSECONDS, 0, 100000, 0),
        boundedInt(SER_VRRTOLERANCEUS, 0, 100000, 0),
    }.resolved(vrrLatencyMode);
    smoothVrrFrameTiming = value(SER_SMOOTHVRRFRAMETIMING).toBool();

    const int defaultBitrate = getDefaultBitrate(width, height, fps, enableYUV444,
                                                  videoCodecConfig, enableHdr);
    bitrateKbps = autoAdjustBitrate ? defaultBitrate :
                  boundedInt(SER_BITRATE, 1, 10000000, defaultBitrate);
}

void StreamingPreferences::saveActiveProfile(QSettings& settings) const
{
    if (!m_ActiveProfileId.isEmpty()) {
        writeProfileSettings(settings, m_ActiveProfileId, profileSettings());
    }
}

void StreamingPreferences::saveGlobalSettings(QSettings& settings) const
{
    settings.setValue(SER_MULTICONT, multiController);
    settings.setValue(SER_MDNS, enableMdns);
    settings.setValue(SER_QUITAPPAFTER, quitAppAfter);
    settings.setValue(SER_ABSMOUSEMODE, absoluteMouseMode);
    settings.setValue(SER_ABSTOUCHMODE, absoluteTouchMode);
    settings.setValue(SER_CONNWARNINGS, connectionWarnings);
    settings.setValue(SER_CONFWARNINGS, configurationWarnings);
    settings.setValue(SER_RICHPRESENCE, richPresence);
    settings.setValue(SER_GAMEPADMOUSE, gamepadMouse);
    settings.setValue(SER_DETECTNETBLOCKING, detectNetworkBlocking);
    settings.setValue(SER_UIDISPLAYMODE, static_cast<int>(uiDisplayMode));
    settings.setValue(SER_LANGUAGE, static_cast<int>(language));
    settings.setValue(SER_DEFAULTVER, CURRENT_DEFAULT_VER);
    settings.setValue(SER_SWAPMOUSEBUTTONS, swapMouseButtons);
    settings.setValue(SER_BACKGROUNDGAMEPAD, backgroundGamepad);
    settings.setValue(SER_REVERSESCROLL, reverseScrollDirection);
    settings.setValue(SER_SWAPFACEBUTTONS, swapFaceButtons);
    settings.setValue(SER_CAPTURESYSKEYS, captureSysKeysMode);
    settings.setValue(SER_KEEPAWAKE, keepAwake);
}

bool StreamingPreferences::profileExists(QSettings& settings, const QString& profileId) const
{
    settings.beginGroup(QStringLiteral("profiles"));
    const bool exists = settings.childGroups().contains(profileId);
    settings.endGroup();
    return exists;
}

void StreamingPreferences::initializeProfiles(QSettings& settings)
{
    if (settings.value(SER_PROFILE_VERSION, 0).toInt() != PROFILE_VERSION) {
        // The flat keys are read immediately before this function. Capture them
        // exactly once as the Default profile, then make the profile tree the
        // sole source of truth for session settings.
        settings.remove(QStringLiteral("profiles"));
        settings.setValue(QStringLiteral("profiles/default/name"), QStringLiteral("Default"));
        writeProfileSettings(settings, DEFAULT_PROFILE_ID, profileSettings());
        settings.setValue(SER_ACTIVE_PROFILE, DEFAULT_PROFILE_ID);
        settings.setValue(SER_PROFILE_VERSION, PROFILE_VERSION);
        settings.sync();
    }
    else if (!profileExists(settings, DEFAULT_PROFILE_ID)) {
        // Recover from a damaged profile tree without allowing Default to vanish.
        settings.setValue(QStringLiteral("profiles/default/name"), QStringLiteral("Default"));
        writeProfileSettings(settings, DEFAULT_PROFILE_ID, defaultProfileSettings());
    }

    QString activeId = settings.value(SER_ACTIVE_PROFILE, DEFAULT_PROFILE_ID).toString();
    if (!profileExists(settings, activeId)) {
        activeId = DEFAULT_PROFILE_ID;
        settings.setValue(SER_ACTIVE_PROFILE, activeId);
    }

    m_ActiveProfileId = activeId;
    refreshProfiles(settings);
    if (!loadProfile(settings, activeId, false)) {
        m_ActiveProfileId = DEFAULT_PROFILE_ID;
        settings.setValue(SER_ACTIVE_PROFILE, m_ActiveProfileId);
        loadProfile(settings, m_ActiveProfileId, false);
        refreshProfiles(settings);
    }
}

void StreamingPreferences::refreshProfiles(QSettings& settings)
{
    struct ProfileEntry {
        QString id;
        QString name;
    };
    QList<ProfileEntry> entries;
    QStringList usedNames;

    settings.beginGroup(QStringLiteral("profiles"));
    QStringList groups = settings.childGroups();
    groups.removeAll(DEFAULT_PROFILE_ID);
    groups.prepend(DEFAULT_PROFILE_ID);
    for (const QString& id : groups) {
        settings.beginGroup(id);
        QString name = settings.value(QStringLiteral("name")).toString().trimmed();
        settings.endGroup();

        if (id == DEFAULT_PROFILE_ID) {
            name = QStringLiteral("Default");
        }
        if (name.isEmpty() || name.size() > 64 ||
                name.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")))) {
            name = tr("Profile");
        }

        const QString baseName = name;
        int suffix = 2;
        while (std::any_of(usedNames.cbegin(), usedNames.cend(), [&name](const QString& existing) {
            return existing.compare(name, Qt::CaseInsensitive) == 0;
        })) {
            name = tr("%1 (%2)").arg(baseName).arg(suffix++);
        }
        usedNames.append(name);
        settings.setValue(id + QStringLiteral("/name"), name);
        entries.append({id, name});
    }
    settings.endGroup();

    std::sort(entries.begin(), entries.end(), [](const ProfileEntry& left, const ProfileEntry& right) {
        if (left.id == DEFAULT_PROFILE_ID) return true;
        if (right.id == DEFAULT_PROFILE_ID) return false;
        return QString::localeAwareCompare(left.name, right.name) < 0;
    });

    m_ProfileIds.clear();
    m_ProfileNames.clear();
    m_ActiveProfileName.clear();
    for (const ProfileEntry& entry : entries) {
        m_ProfileIds.append(entry.id);
        m_ProfileNames.append(entry.name);
        if (entry.id == m_ActiveProfileId) {
            m_ActiveProfileName = entry.name;
        }
    }
}

bool StreamingPreferences::loadProfile(QSettings& settings, const QString& profileId, bool notify)
{
    if (!profileExists(settings, profileId)) {
        return false;
    }

    const QVariantMap oldValues = profileSettings();
    applyProfileSettings(readProfileSettings(settings, profileId));
    if (notify) {
        emitProfileSettingChanges(oldValues, profileSettings());
        emit profileLoaded();
    }
    return true;
}

void StreamingPreferences::emitProfileSettingChanges(const QVariantMap& oldValues,
                                                       const QVariantMap& newValues)
{
    m_ApplyingProfile = true;
    emit applyingProfileChanged();

    auto changed = [&oldValues, &newValues](const char* key) {
        const QString stringKey = QString::fromLatin1(key);
        return oldValues.value(stringKey) != newValues.value(stringKey);
    };

    if (changed(SER_WIDTH) || changed(SER_HEIGHT) || changed(SER_FPS)) emit displayModeChanged();
    if (changed(SER_BITRATE)) emit bitrateChanged();
    if (changed(SER_UNLOCK_BITRATE)) emit unlockBitrateChanged();
    if (changed(SER_AUTOADJUSTBITRATE)) emit autoAdjustBitrateChanged();
    if (changed(SER_VSYNC)) emit enableVsyncChanged();
    if (changed(SER_GAMEOPTS)) emit gameOptimizationsChanged();
    if (changed(SER_HOSTAUDIO)) emit playAudioOnHostChanged();
    if (changed(SER_FRAMEPACING)) emit framePacingChanged();
    if (changed(SER_SHOWPERFOVERLAY)) emit showPerformanceOverlayChanged();
    if (changed(SER_AUDIOCFG)) emit audioConfigChanged();
    if (changed(SER_AUDIOQUALITY)) emit audioQualityChanged();
    if (changed(SER_MICROPHONE)) emit enableMicrophoneChanged();
    if (changed(SER_MICECHOCANCEL)) emit micEchoCancelChanged();
    if (changed(SER_PACKETSIZE)) emit packetSizeChanged();
    if (changed(SER_HDR)) emit enableHdrChanged();
    if (changed(SER_YUV444)) emit enableYUV444Changed();
    if (changed(SER_VIDEOCFG)) emit videoCodecConfigChanged();
    if (changed(SER_VIDEODEC)) emit videoDecoderSelectionChanged();
    if (changed(SER_RENDERER)) emit rendererSelectionChanged();
    if (changed(SER_WINDOWMODE)) emit windowModeChanged();
    if (changed(SER_MUTEONFOCUSLOSS)) emit muteOnFocusLossChanged();
    if (changed(SER_ENABLEVRR)) emit enableVrrChanged();
    if (changed(SER_VRRLATENCYMODE)) emit vrrLatencyModeChanged();
    if (changed(SER_VRRBUFFERPERMILLE) || changed(SER_VRRTARGETHUNDREDTHS) ||
        changed(SER_VRRHISTORYSECONDS) || changed(SER_VRRTOLERANCEUS)) emit vrrTimingChanged();
    if (changed(SER_SMOOTHVRRFRAMETIMING)) emit smoothVrrFrameTimingChanged();

    m_ApplyingProfile = false;
    emit applyingProfileChanged();
}

void StreamingPreferences::applyVrrPreset(int mode)
{
    if (mode < VLM_SMOOTHEST || mode > VLM_LOWEST_LATENCY) {
        return;
    }
    vrrLatencyMode = mode;
    m_VrrTimingOptions = VrrTimingOptions::preset(mode);
    emit vrrLatencyModeChanged();
    emit vrrTimingChanged();
}

int StreamingPreferences::vrrRateForRefresh(int refreshHz)
{
    return VrrRatePolicy::vrrRateForRefresh(refreshHz);
}

int StreamingPreferences::lowLatencyVrrRateForRefresh(int refreshHz)
{
    return VrrRatePolicy::lowLatencyRateForRefresh(refreshHz);
}

bool StreamingPreferences::vrrTimingCustomized() const
{
    return !(m_VrrTimingOptions == VrrTimingOptions::preset(vrrLatencyMode));
}

void StreamingPreferences::setVrrBufferPerMille(int value)
{
    VrrTimingOptions options = m_VrrTimingOptions;
    options.bufferPerMille = value;
    options = options.resolved(vrrLatencyMode);
    if (!(options == m_VrrTimingOptions)) {
        m_VrrTimingOptions = options;
        emit vrrTimingChanged();
    }
}

void StreamingPreferences::setVrrTargetHundredths(int value)
{
    VrrTimingOptions options = m_VrrTimingOptions;
    options.targetHundredths = value;
    options = options.resolved(vrrLatencyMode);
    if (!(options == m_VrrTimingOptions)) {
        m_VrrTimingOptions = options;
        emit vrrTimingChanged();
    }
}

void StreamingPreferences::setVrrHistorySeconds(int value)
{
    VrrTimingOptions options = m_VrrTimingOptions;
    options.historySeconds = value;
    options = options.resolved(vrrLatencyMode);
    if (!(options == m_VrrTimingOptions)) {
        m_VrrTimingOptions = options;
        emit vrrTimingChanged();
    }
}

void StreamingPreferences::setVrrToleranceUs(int value)
{
    VrrTimingOptions options = m_VrrTimingOptions;
    options.toleranceUs = value;
    options = options.resolved(vrrLatencyMode);
    if (!(options == m_VrrTimingOptions)) {
        m_VrrTimingOptions = options;
        emit vrrTimingChanged();
    }
}

void StreamingPreferences::setProfileError(const QString& error)
{
    if (m_LastProfileError != error) {
        m_LastProfileError = error;
        emit lastProfileErrorChanged();
    }
}

bool StreamingPreferences::validateProfileName(const QString& name, const QString& excludingId)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        setProfileError(tr("Profile name cannot be empty."));
        return false;
    }
    if (trimmed.size() > 64) {
        setProfileError(tr("Profile names cannot be longer than 64 characters."));
        return false;
    }
    if (trimmed.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")))) {
        setProfileError(tr("Profile name contains invalid characters."));
        return false;
    }
    for (int i = 0; i < m_ProfileIds.size(); ++i) {
        if (m_ProfileIds.at(i) != excludingId &&
                m_ProfileNames.at(i).compare(trimmed, Qt::CaseInsensitive) == 0) {
            setProfileError(tr("A profile with that name already exists."));
            return false;
        }
    }
    setProfileError(QString());
    return true;
}

bool StreamingPreferences::createProfile(const QString& name)
{
    if (!validateProfileName(name)) return false;

    QSettings settings;
    saveActiveProfile(settings);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    settings.setValue(QStringLiteral("profiles/") + id + QStringLiteral("/name"), name.trimmed());
    writeProfileSettings(settings, id, profileSettings());
    m_ActiveProfileId = id;
    settings.setValue(SER_ACTIVE_PROFILE, id);
    refreshProfiles(settings);
    emit profilesChanged();
    emit activeProfileChanged();
    emit profileLoaded();
    return true;
}

bool StreamingPreferences::duplicateProfile(const QString& sourceProfileId, const QString& name)
{
    if (!m_ProfileIds.contains(sourceProfileId)) {
        setProfileError(tr("The selected profile no longer exists."));
        return false;
    }
    if (!validateProfileName(name)) return false;

    QSettings settings;
    saveActiveProfile(settings);
    const QVariantMap oldValues = profileSettings();
    const QVariantMap copiedValues = sourceProfileId == m_ActiveProfileId ?
                                     oldValues : readProfileSettings(settings, sourceProfileId);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    settings.setValue(QStringLiteral("profiles/") + id + QStringLiteral("/name"), name.trimmed());
    writeProfileSettings(settings, id, copiedValues);
    applyProfileSettings(copiedValues);
    m_ActiveProfileId = id;
    settings.setValue(SER_ACTIVE_PROFILE, id);
    refreshProfiles(settings);
    emitProfileSettingChanges(oldValues, profileSettings());
    emit profilesChanged();
    emit activeProfileChanged();
    emit profileLoaded();
    return true;
}

bool StreamingPreferences::renameProfile(const QString& profileId, const QString& name)
{
    if (profileId == DEFAULT_PROFILE_ID) {
        setProfileError(tr("The Default profile cannot be renamed."));
        return false;
    }
    if (!m_ProfileIds.contains(profileId)) {
        setProfileError(tr("The selected profile no longer exists."));
        return false;
    }
    if (!validateProfileName(name, profileId)) return false;

    QSettings settings;
    settings.setValue(QStringLiteral("profiles/") + profileId + QStringLiteral("/name"), name.trimmed());
    refreshProfiles(settings);
    emit profilesChanged();
    if (profileId == m_ActiveProfileId) emit activeProfileChanged();
    return true;
}

bool StreamingPreferences::deleteProfile(const QString& profileId)
{
    if (profileId == DEFAULT_PROFILE_ID) {
        setProfileError(tr("The Default profile cannot be deleted."));
        return false;
    }
    if (!m_ProfileIds.contains(profileId)) {
        setProfileError(tr("The selected profile no longer exists."));
        return false;
    }

    if (profileId == m_ActiveProfileId && !activateProfile(DEFAULT_PROFILE_ID)) {
        return false;
    }
    QSettings settings;
    settings.remove(QStringLiteral("profiles/") + profileId);
    refreshProfiles(settings);
    setProfileError(QString());
    emit profilesChanged();
    return true;
}

bool StreamingPreferences::activateProfile(const QString& profileId)
{
    if (!m_ProfileIds.contains(profileId)) {
        setProfileError(tr("The selected profile no longer exists."));
        return false;
    }
    if (profileId == m_ActiveProfileId) {
        setProfileError(QString());
        return true;
    }

    QSettings settings;
    saveActiveProfile(settings);
    const QVariantMap oldValues = profileSettings();
    if (!loadProfile(settings, profileId, false)) {
        setProfileError(tr("The selected profile could not be loaded."));
        return false;
    }
    m_ActiveProfileId = profileId;
    settings.setValue(SER_ACTIVE_PROFILE, profileId);
    refreshProfiles(settings);
    setProfileError(QString());
    emitProfileSettingChanges(oldValues, profileSettings());
    emit activeProfileChanged();
    emit profileLoaded();
    return true;
}

int StreamingPreferences::getDefaultBitrate(int width, int height, int fps, bool yuv444,
                                            int videoCodecConfig, bool hdr)
{
    if (videoCodecConfig == VCC_FORCE_PYROWAVE) {
        // PyroWave is intra-only, so quality is governed by bytes per frame.
        // 400 kB at 4K 4:2:0 SDR is an intentionally quality-oriented
        // starting point which should be refined with empirical quality data.
        return BitrateCalculator::pyroWaveDefaultKbps(width, height, fps, yuv444, hdr);
    }

    // Don't scale bitrate linearly beyond 60 FPS. It's definitely not a linear
    // bitrate increase for frame rate once we get to values that high.
    float frameRateFactor = (fps <= 60 ? fps : (qSqrt(fps / 60.f) * 60.f)) / 30.f;

    // TODO: Collect some empirical data to see if these defaults make sense.
    // We're just using the values that the Shield used, as we have for years.
    static const struct resTable {
        int pixels;
        int factor;
    } resTable[] {
        { 640 * 360, 1 },
        { 854 * 480, 2 },
        { 1280 * 720, 5 },
        { 1920 * 1080, 10 },
        { 2560 * 1440, 20 },
        { 3840 * 2160, 40 },
        { -1, -1 },
    };

    // Calculate the resolution factor by linear interpolation of the resolution table
    float resolutionFactor;
    int pixels = width * height;
    for (int i = 0;; i++) {
        if (pixels == resTable[i].pixels) {
            // We can bail immediately for exact matches
            resolutionFactor = resTable[i].factor;
            break;
        }
        else if (pixels < resTable[i].pixels) {
            if (i == 0) {
                // Never go below the lowest resolution entry
                resolutionFactor = resTable[i].factor;
            }
            else {
                // Interpolate between the entry greater than the chosen resolution (i) and the entry less than the chosen resolution (i-1)
                resolutionFactor = ((float)(pixels - resTable[i-1].pixels) / (resTable[i].pixels - resTable[i-1].pixels)) * (resTable[i].factor - resTable[i-1].factor) + resTable[i-1].factor;
            }
            break;
        }
        else if (resTable[i].pixels == -1) {
            // Never go above the highest resolution entry
            resolutionFactor = resTable[i-1].factor;
            break;
        }
    }

    if (yuv444) {
        // This is rough estimation based on the fact that 4:4:4 doubles the amount of raw YUV data compared to 4:2:0
        resolutionFactor *= 2;
    }

    return qRound(resolutionFactor * frameRateFactor) * 1000;
}

#ifdef HAVE_PYROWAVE
#include <vulkan/vulkan.h>
#include <pyrowave/pyrowave.h>
#endif

bool StreamingPreferences::isPyroWaveAvailable()
{
#ifdef HAVE_PYROWAVE
    static const bool supported = [] {
        pyrowave_device device = nullptr;
        if (pyrowave_create_default_device(&device) != PYROWAVE_SUCCESS) return false;
        pyrowave_decoder_create_info info {};
        info.device = device;
        info.width = info.height = 64;
        pyrowave_decoder decoder = nullptr;
        const bool created = pyrowave_decoder_create(&info, &decoder) == PYROWAVE_SUCCESS;
        if (decoder) pyrowave_decoder_destroy(decoder);
        pyrowave_device_destroy(device);
        return created;
    }();
    return supported;
#else
    return false;
#endif
}


namespace {
const QSet<QString> globalIntegrationSettings {
    "multiController", "enableMdns", "quitAppAfter", "absoluteMouseMode",
    "absoluteTouchMode", "connectionWarnings", "configurationWarnings", "richPresence",
    "gamepadMouse", "detectNetworkBlocking", "uiDisplayMode", "swapMouseButtons",
    "backgroundGamepad", "reverseScrollDirection", "swapFaceButtons", "keepAwake",
    "captureSysKeysMode", "language"
};

QString integrationCategory(const QString& name)
{
    if (QSet<QString>{"audioConfig", "audioQuality", "playAudioOnHost", "muteOnFocusLoss",
                     "enableMicrophone", "micEchoCancel"}.contains(name))
        return "Audio";
    if (QSet<QString>{"multiController", "absoluteMouseMode", "absoluteTouchMode", "gamepadMouse",
                     "swapMouseButtons", "backgroundGamepad", "reverseScrollDirection", "swapFaceButtons",
                     "captureSysKeysMode"}.contains(name)) return "Controller & Input";
    if (name.startsWith("vrr") || name == "smoothVrrFrameTiming") return "VRR Advanced";
    if (QSet<QString>{"width", "height", "fps", "bitrateKbps", "autoAdjustBitrate", "enableVsync",
                     "framePacing", "videoCodecConfig", "enableHdr", "enableYUV444", "windowMode",
                     "enableVrr"}.contains(name)) return "Video & Display";
    if (QSet<QString>{"enableMdns", "connectionWarnings", "configurationWarnings", "detectNetworkBlocking"}.contains(name))
        return "Connection & Diagnostics";
    return "Advanced Streaming";
}

QPair<int, int> integrationBounds(const QString& name)
{
    if (name == "width" || name == "height") return {1, 16384};
    if (name == "fps") return {1, 1000};
    if (name == "bitrateKbps") return {1, 10000000};
    if (name == "packetSize") return {0, 65535};
    if (name == "vrrLatencyMode") return {0, 2};
    if (name == "micEchoCancel") return {0, 2};
    if (name == "vrrBufferPerMille") return {250, 4000};
    if (name == "vrrTargetHundredths") return {9000, 9999};
    if (name == "vrrHistorySeconds") return {10, 300};
    if (name == "vrrToleranceUs") return {250, 2000};
    return {0, 0};
}
}

QJsonObject StreamingPreferences::integrationSnapshot() const
{
    QJsonObject values;
    QJsonArray registry;
    const QMetaObject* meta = metaObject();
    for (int i = meta->propertyOffset(); i < meta->propertyCount(); ++i) {
        const QMetaProperty property = meta->property(i);
        if (!property.isWritable()) continue;
        const QString name = QString::fromLatin1(property.name());
        const QVariant value = property.read(this);
        values[name] = property.isEnumType() ? QJsonValue(value.toInt()) : QJsonValue::fromVariant(value);
        const auto bounds = integrationBounds(name);
        QJsonObject descriptor {
            {"key", name}, {"category", integrationCategory(name)},
            {"scope", globalIntegrationSettings.contains(name) ? "global" : "profile"},
            // No setting is advertised as live until Session has a safe runtime setter.
            {"application", "reconnect"}, {"type", property.userType() == QMetaType::Bool ? "boolean" : "integer"},
            {"minimum", bounds.first}, {"maximum", bounds.second}
        };
        QJsonArray choices;
        if (property.isEnumType()) {
            const QMetaEnum enumeration = property.enumerator();
            for (int e = 0; e < enumeration.keyCount(); ++e) {
                const int id = enumeration.value(e);
                if (id < 0 || (name == "videoCodecConfig" && id == VCC_FORCE_HEVC_HDR_DEPRECATED)) continue;
                choices.append(QJsonObject{{"value", id}, {"label", QString::fromLatin1(enumeration.key(e))},
                    {"supported", name != "videoCodecConfig" || id != VCC_FORCE_PYROWAVE || isPyroWaveAvailable()}});
            }
        }
        if (!choices.isEmpty()) descriptor["choices"] = choices;
#ifndef Q_OS_DARWIN
        if (name == "rendererSelection") descriptor["supported"] = false;
#endif
        if (name == "vrrLatencyMode") descriptor["choices"] = QJsonArray{
            QJsonObject{{"value", 0}, {"label", "Smoothest"}, {"supported", true}},
            QJsonObject{{"value", 1}, {"label", "Balanced"}, {"supported", true}},
            QJsonObject{{"value", 2}, {"label", "Lowest latency"}, {"supported", true}}};
        if (name.startsWith("vrr") || name == "smoothVrrFrameTiming") descriptor["requires"] = "enableVrr";
        if (name == "micEchoCancel") descriptor["requires"] = "enableMicrophone";
        if (name == "bitrateKbps") descriptor["disabledWhen"] = "autoAdjustBitrate";
        registry.append(descriptor);
    }
    QJsonArray profiles;
    for (int i = 0; i < m_ProfileIds.size(); ++i)
        profiles.append(QJsonObject{{"id", m_ProfileIds[i]}, {"name", m_ProfileNames[i]}});
    // Include inactive profiles too: rename/delete elsewhere must invalidate old transactions.
    QSettings settings;
    settings.sync();
    QJsonObject persisted;
    for (const QString& key : settings.allKeys())
        // INI values are typed in the writer's cache and strings in a fresh
        // process. Canonicalize them so reading never changes the revision.
        if (key.startsWith("profiles/")) persisted[key] = settings.value(key).toString();
    QJsonObject revisionState{{"values", values}, {"profiles", persisted}, {"activeProfileId", m_ActiveProfileId}};
    const QString revision = QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(revisionState).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    return {{"schema", 1}, {"ok", true}, {"revision", revision}, {"activeProfileId", m_ActiveProfileId},
            {"activeProfileName", m_ActiveProfileName}, {"profiles", profiles}, {"values", values},
            {"registry", registry}, {"capabilities", QJsonObject{{"pyrowave", isPyroWaveAvailable()},
                {"displayProbe", "requires streaming display"}, {"liveUpdates", false}, {"windowReadiness", true}}}};
}

QJsonObject StreamingPreferences::integrationRequest(const QJsonObject& request)
{
    auto error = [](const QString& code, const QString& message) -> QJsonObject {
        return {{"schema", 1}, {"ok", false}, {"error", QJsonObject{{"code", code}, {"message", message}}}};
    };
    if (request.value("schema").toInt() != 1) return error("schema", "Unsupported settings schema");
    const QString operation = request.value("operation").toString();
    if (operation == "read") return integrationSnapshot();
    if (operation == "defaults") {
        StreamingPreferences defaults(nullptr);
        defaults.applyProfileSettings(defaultProfileSettings());
        QJsonObject values;
        for (int i = metaObject()->propertyOffset(); i < metaObject()->propertyCount(); ++i) {
            const auto property = metaObject()->property(i);
            if (!property.isWritable() || globalIntegrationSettings.contains(property.name())) continue;
            values[property.name()] = property.isEnumType() ? QJsonValue(property.read(&defaults).toInt()) :
                QJsonValue::fromVariant(property.read(&defaults));
        }
        auto result = integrationSnapshot();
        result["profileDefaults"] = values;
        return result;
    }
    if (operation == "prepare-launch") {
        // Include edits in the standalone GUI even if its settings page is
        // still open. The next process must start from this exact snapshot.
        save();
        QSettings settings;
        settings.sync();
        if (settings.status() != QSettings::NoError) return error("storage", "Could not persist launch settings");
        return integrationSnapshot();
    }
    const QJsonObject snapshot = integrationSnapshot();
    if (request.value("revision").toString().isEmpty() || request.value("revision") != snapshot.value("revision"))
        return error("conflict", "Pyrolight settings changed. Refresh before applying this draft.");
    if (operation == "patch" || operation == "validate") {
        if (!request.value("values").isObject()) return error("validation", "Expected settings object");
        const QJsonObject changes = request.value("values").toObject();
        QJsonObject proposed = snapshot.value("values").toObject();
        // Validate the whole patch before writing any property or emitting a signal.
        for (auto it = changes.begin(); it != changes.end(); ++it) {
            const QByteArray key = it.key().toLatin1();
            const int index = metaObject()->indexOfProperty(key.constData());
            if (index < metaObject()->propertyOffset()) return error("validation", "Unknown setting: " + it.key());
            const QMetaProperty property = metaObject()->property(index);
            if (!property.isWritable()) return error("validation", "Read-only setting: " + it.key());
            if (property.userType() == QMetaType::Bool) {
                if (!it->isBool()) return error("validation", "Expected boolean: " + it.key());
            } else {
                if (!it->isDouble() || !std::isfinite(it->toDouble()) || std::floor(it->toDouble()) != it->toDouble())
                    return error("validation", "Expected integer: " + it.key());
                const auto bounds = integrationBounds(it.key());
                if (property.isEnumType()) {
                    if (!property.enumerator().valueToKey(it->toInt(-999))) return error("validation", "Invalid choice: " + it.key());
                    if (it->toInt() < 0) return error("validation", "Probe-only choice");
                } else if (it->toDouble() < bounds.first || it->toDouble() > bounds.second)
                    return error("validation", "Out of range: " + it.key());
            }
            proposed[it.key()] = it.value();
        }
        const int codec = proposed.value("videoCodecConfig").toInt();
        if (codec == VCC_FORCE_HEVC_HDR_DEPRECATED) return error("validation", "Use HEVC and the HDR option");
        if (codec == VCC_FORCE_PYROWAVE && !isPyroWaveAvailable()) return error("capability", "PyroWave decoder is unavailable");
        if (proposed.value("enableVrr").toBool() && !proposed.value("enableVsync").toBool())
            return error("validation", "VRR requires V-Sync");
#ifndef Q_OS_DARWIN
        if (changes.contains("rendererSelection") && changes.value("rendererSelection").toInt() != RS_AUTO)
            return error("capability", "Renderer selection is available on macOS only");
#endif
        if (operation == "validate") return snapshot;
        const QVariantMap previous = profileSettings();
        {
            QSignalBlocker blocker(this);
            if (changes.contains("vrrLatencyMode")) applyVrrPreset(changes.value("vrrLatencyMode").toInt());
            for (auto it = changes.begin(); it != changes.end(); ++it) {
                const QMetaProperty property = metaObject()->property(metaObject()->indexOfProperty(it.key().toLatin1().constData()));
                if (it.key() != "vrrLatencyMode") property.write(this, it->toVariant());
            }
            // Reuse profile normalization and the authoritative bitrate calculation.
            applyProfileSettings(profileSettings());
        }
        emitProfileSettingChanges(previous, profileSettings());
        for (auto it = changes.begin(); it != changes.end(); ++it) {
            if (!globalIntegrationSettings.contains(it.key())) continue;
            const QMetaProperty property = metaObject()->property(metaObject()->indexOfProperty(it.key().toLatin1().constData()));
            if (property.hasNotifySignal()) property.notifySignal().invoke(this, Qt::DirectConnection);
        }
        save();
    } else {
        const QString id = request.value("profileId").toString();
        const QString name = request.value("name").toString();
        bool ok = false;
        if (operation == "create") ok = createProfile(name);
        else if (operation == "duplicate") ok = duplicateProfile(id, name);
        else if (operation == "rename") ok = renameProfile(id, name);
        else if (operation == "delete") ok = deleteProfile(id);
        else if (operation == "activate") ok = activateProfile(id);
        else if (operation == "restore") {
            const QVariantMap previous = profileSettings();
            applyProfileSettings(defaultProfileSettings());
            emitProfileSettingChanges(previous, profileSettings());
            save();
            ok = true;
        } else return error("operation", "Unknown settings operation");
        if (!ok) return error("profile", lastProfileError());
    }
    QSettings settings;
    settings.sync();
    if (settings.status() != QSettings::NoError) return error("storage", "Could not persist Pyrolight settings");
    return integrationSnapshot();
}
