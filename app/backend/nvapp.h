#pragma once

#include <QSettings>
#include <QStringList>

class NvApp
{
public:
    NvApp() {}
    explicit NvApp(QSettings& settings);

    bool operator==(const NvApp& other) const
    {
        return id == other.id &&
                name == other.name &&
                hdrSupported == other.hdrSupported &&
                isAppCollectorGame == other.isAppCollectorGame &&
                hidden == other.hidden &&
                directLaunch == other.directLaunch &&
                pyroshineSources == other.pyroshineSources;
    }

    bool operator!=(const NvApp& other) const
    {
        return !operator==(other);
    }

    bool isInitialized()
    {
        // We use isNull() instead of isEmpty() here because we want
        // to detect cases where the name is unassigned, not empty.
        return id != 0 && !name.isNull();
    }

    void
    serialize(QSettings& settings) const;

    // The host's Remote Input app (input only): the host streams no video or
    // audio for it. Rubylight lists its tile under these ids; Apollo and
    // Vibepollo name theirs "Remote Input".
    bool isRemoteInput() const
    {
        return id == 2147483506 || id == 2147483516 || id == 2147483606 ||
                name.trimmed().compare(QStringLiteral("Remote Input"), Qt::CaseInsensitive) == 0;
    }

    int id = 0;
    QString name;
    bool hdrSupported = false;
    bool isAppCollectorGame = false;
    bool hidden = false;
    bool directLaunch = false;

    // Launcher identities ("steam:892970") a Pyroshine host reports for this
    // app in <PyroshineSource>. Host metadata only: never persisted, refreshed
    // with every app list.
    QStringList pyroshineSources;
};

Q_DECLARE_METATYPE(NvApp)
