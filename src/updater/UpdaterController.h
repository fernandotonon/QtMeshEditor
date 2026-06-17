#ifndef UPDATERCONTROLLER_H
#define UPDATERCONTROLLER_H

#include "GitHubReleaseParser.h"
#include "InstallFlavor.h"
#include "UpdaterWorker.h"

#include <QElapsedTimer>
#include <QList>
#include <QQmlEngine>
#include <QJSEngine>
#include <QThread>

/**
 * @brief QML-facing singleton orchestrating update checks (#441, #443, #449).
 *
 * Mirrors the LLMManager / SDManager pattern: main-thread controller with
 * a worker thread for HTTP. Install/relaunch land in #446–448.
 */
class UpdaterController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion NOTIFY currentVersionChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY latestVersionChanged)
    Q_PROPERTY(QString latestUrl READ latestUrl NOTIFY latestUrlChanged)
    Q_PROPERTY(QString changelog READ changelog NOTIFY changelogChanged)
    Q_PROPERTY(QString publishedAt READ publishedAt NOTIFY publishedAtChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString channel READ channel WRITE setChannel NOTIFY channelChanged)
    Q_PROPERTY(QString installFlavor READ installFlavor NOTIFY installFlavorChanged)
    Q_PROPERTY(QString flavorDisplayName READ flavorDisplayName NOTIFY installFlavorChanged)
    Q_PROPERTY(QString updateCommandHint READ updateCommandHint NOTIFY installFlavorChanged)
    Q_PROPERTY(bool isPackageManaged READ isPackageManaged NOTIFY installFlavorChanged)
    Q_PROPERTY(bool checkOnStartup READ checkOnStartup WRITE setCheckOnStartup NOTIFY checkOnStartupChanged)
    Q_PROPERTY(bool autoDownload READ autoDownload WRITE setAutoDownload NOTIFY autoDownloadChanged)
    Q_PROPERTY(QString lastCheckedAt READ lastCheckedAt NOTIFY lastCheckedAtChanged)

public:
    enum class State {
        Idle = 0,
        Checking,
        UnknownInstall,
        PackageManaged,
        UpdateAvailable,
        UpToDate,
        Error,
        Downloading,
        Verifying,
        ReadyToInstall,
    };
    Q_ENUM(State)

    static UpdaterController* instance();
    static UpdaterController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    State state() const { return m_state; }
    QString currentVersion() const { return m_currentVersion; }
    QString latestVersion() const { return m_latestVersion; }
    QString latestUrl() const { return m_latestUrl; }
    QString changelog() const { return m_changelog; }
    QString publishedAt() const { return m_publishedAt; }
    int progress() const { return m_progress; }
    QString error() const { return m_error; }
    QString channel() const { return m_channel; }
    QString installFlavor() const { return m_installFlavor; }
    QString flavorDisplayName() const { return InstallFlavor::displayName(m_flavor); }
    QString updateCommandHint() const { return InstallFlavor::updateCommandHint(m_flavor); }
    bool isPackageManaged() const { return InstallFlavor::isPackageManagerManaged(m_flavor); }
    bool checkOnStartup() const { return m_checkOnStartup; }
    bool autoDownload() const { return m_autoDownload; }
    QString lastCheckedAt() const { return m_lastCheckedAt; }

    void setChannel(const QString& channel);
    void setCheckOnStartup(bool value);
    void setAutoDownload(bool value);

    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void requestCheckDialog();
    Q_INVOKABLE void confirmUnknownInstall();
    Q_INVOKABLE void downloadAndInstall();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void dismiss();
    Q_INVOKABLE void remindLater();
    Q_INVOKABLE void skipThisVersion();
    Q_INVOKABLE void openReleasePage();
    Q_INVOKABLE void copyUpdateCommand();

#ifdef QTMESH_UNIT_TESTS
    void setLatestVersionForTest(const QString& tag);
#endif

signals:
    void stateChanged();
    void currentVersionChanged();
    void latestVersionChanged();
    void latestUrlChanged();
    void changelogChanged();
    void publishedAtChanged();
    void progressChanged();
    void errorChanged();
    void channelChanged();
    void installFlavorChanged();
    void checkOnStartupChanged();
    void autoDownloadChanged();
    void lastCheckedAtChanged();
    void showDialogRequested(bool runCheck);

    void updateAvailable(const QString& currentVersion, const QString& latestVersion);
    void noUpdate();
    void checkError(const QString& message);

private:
    explicit UpdaterController(QObject* parent = nullptr);
    ~UpdaterController() override;

    void setState(State state);
    void setError(const QString& error);
    void applyCheckResult(const GitHubReleaseParser::CheckResult& result);
    void beginDownloadIfNeeded(bool userInitiated);
    void startDownloadJob();
    void handleDownloadFinished(const DownloadOutcome& outcome);
    void handleVerifyFinished(const VerifyOutcome& outcome);
    void setProgressPercent(int percent);
    GitHubReleaseParser::Channel activeChannel() const;
    void refreshInstallFlavor();
    void loadSettings();
    void saveSettings();
    void recordLastChecked();
    bool isVersionSkipped(const QString& tag) const;
    void logDialogStateBreadcrumb();

    static UpdaterController* s_instance;

    QThread* m_workerThread = nullptr;
    class UpdaterWorker* m_worker = nullptr;

    State m_state = State::Idle;
    QString m_currentVersion;
    QString m_latestVersion;
    QString m_latestUrl;
    QString m_changelog;
    QString m_publishedAt;
    int m_progress = 0;
    QString m_error;
    QString m_channel = QStringLiteral("stable");
    QString m_installFlavor;
    InstallFlavor::Flavor m_flavor = InstallFlavor::Flavor::Unknown;
    UpdateVersion::Comparison m_lastComparison = UpdateVersion::Comparison::Invalid;
    bool m_unknownInstallConfirmed = false;
    bool m_checkOnStartup = true;
    bool m_autoDownload = false;
    QString m_lastCheckedAt;
    QList<GitHubReleaseParser::ReleaseAsset> m_releaseAssets;
    QString m_stagedArtifactPath;
    qint64 m_lastProgressEmitMs = 0;
    QElapsedTimer m_progressThrottle;
};

#endif // UPDATERCONTROLLER_H
