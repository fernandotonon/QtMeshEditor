#ifndef UPDATERCONTROLLER_H
#define UPDATERCONTROLLER_H

#include "GitHubReleaseParser.h"
#include "InstallFlavor.h"

#include <QObject>
#include <QQmlEngine>
#include <QJSEngine>
#include <QThread>

/**
 * @brief QML-facing singleton orchestrating update checks (#441).
 *
 * Mirrors the LLMManager / SDManager pattern: main-thread controller with
 * a worker thread for HTTP. Download/install land in #444–448.
 */
class UpdaterController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY latestVersionChanged)
    Q_PROPERTY(QString latestUrl READ latestUrl NOTIFY latestUrlChanged)
    Q_PROPERTY(QString changelog READ changelog NOTIFY changelogChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString channel READ channel WRITE setChannel NOTIFY channelChanged)
    Q_PROPERTY(QString installFlavor READ installFlavor NOTIFY installFlavorChanged)

public:
    enum class State {
        Idle = 0,
        Checking,
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

    State state() const { return m_state; }
    QString latestVersion() const { return m_latestVersion; }
    QString latestUrl() const { return m_latestUrl; }
    QString changelog() const { return m_changelog; }
    int progress() const { return m_progress; }
    QString error() const { return m_error; }
    QString channel() const { return m_channel; }
    QString installFlavor() const { return m_installFlavor; }

    void setChannel(const QString& channel);

    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void downloadAndInstall();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void dismiss();

signals:
    void stateChanged();
    void latestVersionChanged();
    void latestUrlChanged();
    void changelogChanged();
    void progressChanged();
    void errorChanged();
    void channelChanged();
    void installFlavorChanged();

    void updateAvailable(const QString& currentVersion, const QString& latestVersion);
    void noUpdate();
    void checkError(const QString& message);

private:
    explicit UpdaterController(QObject* parent = nullptr);
    ~UpdaterController() override;

    void setState(State state);
    void setError(const QString& error);
    void applyCheckResult(const GitHubReleaseParser::CheckResult& result);
    void presentCheckOutcome();
    GitHubReleaseParser::Channel activeChannel() const;
    void refreshInstallFlavor();

    static UpdaterController* s_instance;

    QThread* m_workerThread = nullptr;
    class UpdaterWorker* m_worker = nullptr;

    State m_state = State::Idle;
    QString m_latestVersion;
    QString m_latestUrl;
    QString m_changelog;
    int m_progress = 0;
    QString m_error;
    QString m_channel = QStringLiteral("stable");
    QString m_installFlavor;
    InstallFlavor::Flavor m_flavor = InstallFlavor::Flavor::Unknown;
    UpdateVersion::Comparison m_lastComparison = UpdateVersion::Comparison::Invalid;
    QString m_localVersion;
};

#endif // UPDATERCONTROLLER_H
