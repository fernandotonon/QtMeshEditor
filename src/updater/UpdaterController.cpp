#include "UpdaterController.h"
#include "UpdaterWorker.h"
#include "AppSettingsKeys.h"
#include "SentryReporter.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QSettings>
#include <QUrl>

namespace {

constexpr const char* kDefaultReleasesApi =
    "https://api.github.com/repos/fernandotonon/QtMeshEditor/releases?per_page=20";

QString stateToString(UpdaterController::State state)
{
    switch (state) {
    case UpdaterController::State::Idle: return QStringLiteral("idle");
    case UpdaterController::State::Checking: return QStringLiteral("checking");
    case UpdaterController::State::UnknownInstall: return QStringLiteral("unknown_install");
    case UpdaterController::State::PackageManaged: return QStringLiteral("package_managed");
    case UpdaterController::State::UpdateAvailable: return QStringLiteral("update_available");
    case UpdaterController::State::UpToDate: return QStringLiteral("up_to_date");
    case UpdaterController::State::Error: return QStringLiteral("error");
    case UpdaterController::State::Downloading: return QStringLiteral("downloading");
    case UpdaterController::State::Verifying: return QStringLiteral("verifying");
    case UpdaterController::State::ReadyToInstall: return QStringLiteral("ready_to_install");
    }
    return QStringLiteral("idle");
}

} // namespace

UpdaterController* UpdaterController::s_instance = nullptr;

UpdaterController* UpdaterController::instance()
{
    if (!s_instance) {
        s_instance = new UpdaterController();
    }
    return s_instance;
}

UpdaterController* UpdaterController::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(scriptEngine)
    return instance();
}

void UpdaterController::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

UpdaterController::UpdaterController(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<GitHubReleaseParser::Channel>("GitHubReleaseParser::Channel");
    loadSettings();
    refreshInstallFlavor();

    m_workerThread = new QThread(this);
    m_worker = new UpdaterWorker();
    m_worker->moveToThread(m_workerThread);

    connect(m_worker, &UpdaterWorker::checkFinished, this,
            [this](const GitHubReleaseParser::CheckResult& result, const QString& networkError) {
                recordLastChecked();

                if (!networkError.isEmpty()) {
                    setState(State::Error);
                    setError(networkError);
                    SentryReporter::addBreadcrumb(
                        QStringLiteral("updater.check.error"),
                        networkError,
                        QStringLiteral("error"));
                    emit checkError(networkError);
                    logDialogStateBreadcrumb();
                    return;
                }

                if (!result.parseOk) {
                    setState(State::Error);
                    setError(result.errorMessage);
                    SentryReporter::addBreadcrumb(
                        QStringLiteral("updater.check.error"),
                        result.errorMessage,
                        QStringLiteral("error"));
                    emit checkError(result.errorMessage);
                    logDialogStateBreadcrumb();
                    return;
                }

                applyCheckResult(result);
                SentryReporter::addBreadcrumb(
                    QStringLiteral("updater.check.success"),
                    QStringLiteral("remote=%1 comparison=%2")
                        .arg(result.release.tagName)
                        .arg(static_cast<int>(result.comparison)));
                logDialogStateBreadcrumb();
            });

    m_workerThread->start();
}

UpdaterController::~UpdaterController()
{
    if (m_worker && m_workerThread) {
        if (m_workerThread->isRunning()) {
            QMetaObject::invokeMethod(m_worker, &UpdaterWorker::cancelActiveRequest,
                                      Qt::BlockingQueuedConnection);
            m_workerThread->quit();
            m_workerThread->wait();
        }
        m_worker->moveToThread(QThread::currentThread());
        delete m_worker;
        m_worker = nullptr;
    }
}

void UpdaterController::loadSettings()
{
    QSettings settings;
    setChannel(settings.value(AppSettingsKeys::updaterChannel(), QStringLiteral("stable")).toString());
    m_checkOnStartup = settings.value(AppSettingsKeys::updaterCheckOnStartup(), true).toBool();
    m_autoDownload = settings.value(AppSettingsKeys::updaterAutoDownload(), false).toBool();
    m_lastCheckedAt = settings.value(AppSettingsKeys::updaterLastCheckedAt()).toString();
}

void UpdaterController::saveSettings()
{
    QSettings settings;
    settings.setValue(AppSettingsKeys::updaterChannel(), m_channel);
    settings.setValue(AppSettingsKeys::updaterCheckOnStartup(), m_checkOnStartup);
    settings.setValue(AppSettingsKeys::updaterAutoDownload(), m_autoDownload);
    settings.setValue(AppSettingsKeys::updaterLastCheckedAt(), m_lastCheckedAt);
}

void UpdaterController::recordLastChecked()
{
    m_lastCheckedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    saveSettings();
    emit lastCheckedAtChanged();
}

bool UpdaterController::isVersionSkipped(const QString& tag) const
{
    QSettings settings;
    return settings.value(AppSettingsKeys::updaterSkippedVersion()).toString() == tag;
}

void UpdaterController::refreshInstallFlavor()
{
    m_flavor = InstallFlavor::detect(QCoreApplication::applicationFilePath());
    const QString slug = InstallFlavor::toSlug(m_flavor);
    if (slug != m_installFlavor) {
        m_installFlavor = slug;
        emit installFlavorChanged();
    }
    SentryReporter::addBreadcrumb(QStringLiteral("updater.flavor.detected"), slug);
}

void UpdaterController::setChannel(const QString& channel)
{
    bool ok = false;
    const GitHubReleaseParser::Channel parsed = GitHubReleaseParser::channelFromString(channel, &ok);
    const QString normalized =
        GitHubReleaseParser::channelToString(ok ? parsed : GitHubReleaseParser::Channel::Stable);
    if (normalized == m_channel) {
        return;
    }
    m_channel = normalized;
    saveSettings();
    emit channelChanged();
}

void UpdaterController::setCheckOnStartup(bool value)
{
    if (m_checkOnStartup == value) {
        return;
    }
    m_checkOnStartup = value;
    saveSettings();
    emit checkOnStartupChanged();
}

void UpdaterController::setAutoDownload(bool value)
{
    if (m_autoDownload == value) {
        return;
    }
    m_autoDownload = value;
    saveSettings();
    emit autoDownloadChanged();
}

GitHubReleaseParser::Channel UpdaterController::activeChannel() const
{
    return GitHubReleaseParser::channelFromString(m_channel);
}

void UpdaterController::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged();
}

void UpdaterController::setError(const QString& error)
{
    if (m_error == error) {
        return;
    }
    m_error = error;
    emit errorChanged();
}

void UpdaterController::logDialogStateBreadcrumb()
{
    SentryReporter::addBreadcrumb(
        QStringLiteral("updater.dialog.state"),
        stateToString(m_state));
}

void UpdaterController::applyCheckResult(const GitHubReleaseParser::CheckResult& result)
{
    m_lastComparison = result.comparison;
    m_latestVersion = result.release.tagName;
    m_latestUrl = result.release.htmlUrl;
    m_changelog = result.release.body;
    m_publishedAt = result.release.publishedAt;
    emit latestVersionChanged();
    emit latestUrlChanged();
    emit changelogChanged();
    emit publishedAtChanged();

    switch (result.comparison) {
    case UpdateVersion::Comparison::Older:
        if (isVersionSkipped(m_latestVersion)) {
            setState(State::UpToDate);
            emit noUpdate();
            return;
        }
        setState(State::UpdateAvailable);
        emit updateAvailable(m_currentVersion, m_latestVersion);
        break;
    case UpdateVersion::Comparison::Same:
    case UpdateVersion::Comparison::Newer:
        setState(State::UpToDate);
        emit noUpdate();
        break;
    case UpdateVersion::Comparison::Invalid:
        setState(State::Error);
        setError(QStringLiteral("Could not compare versions '%1' / '%2'")
                     .arg(m_currentVersion, m_latestVersion));
        emit checkError(m_error);
        break;
    }
}

void UpdaterController::requestCheckDialog()
{
    emit showDialogRequested(true);
}

void UpdaterController::checkForUpdates()
{
    refreshInstallFlavor();
    m_currentVersion = QApplication::applicationVersion();
    emit currentVersionChanged();
    setError(QString());

    if (InstallFlavor::isPackageManagerManaged(m_flavor)) {
        SentryReporter::addBreadcrumb(
            QStringLiteral("updater.check.start"),
            QStringLiteral("package-manager flavor=%1").arg(m_installFlavor));
        setState(State::PackageManaged);
        logDialogStateBreadcrumb();
        return;
    }

    if (m_flavor == InstallFlavor::Flavor::Unknown && !m_unknownInstallConfirmed) {
        setState(State::UnknownInstall);
        logDialogStateBreadcrumb();
        return;
    }

    if (m_state == State::Checking) {
        return;
    }

    SentryReporter::addBreadcrumb(
        QStringLiteral("updater.check.start"),
        QStringLiteral("channel=%1 local=%2").arg(m_channel, m_currentVersion));
    setState(State::Checking);

    const QString localVersion = m_currentVersion;
    const auto channel = activeChannel();
    QMetaObject::invokeMethod(
        m_worker,
        "checkForUpdates",
        Qt::QueuedConnection,
        Q_ARG(QString, QString::fromUtf8(kDefaultReleasesApi)),
        Q_ARG(QString, localVersion),
        Q_ARG(GitHubReleaseParser::Channel, channel));
}

void UpdaterController::confirmUnknownInstall()
{
    m_unknownInstallConfirmed = true;
    SentryReporter::addBreadcrumb(QStringLiteral("updater.dialog.action"),
                                  QStringLiteral("confirm_unknown_install"));
    checkForUpdates();
}

void UpdaterController::downloadAndInstall()
{
    SentryReporter::addBreadcrumb(QStringLiteral("updater.download"),
                                  QStringLiteral("downloadAndInstall not implemented yet"));
    setError(tr("Automatic download and install is not available yet."));
    setState(State::Error);
    emit checkError(m_error);
    logDialogStateBreadcrumb();
}

void UpdaterController::cancel()
{
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, &UpdaterWorker::cancelActiveRequest,
                                  Qt::QueuedConnection);
    }
    if (m_state == State::Checking || m_state == State::Downloading) {
        setState(State::Idle);
        logDialogStateBreadcrumb();
    }
}

void UpdaterController::dismiss()
{
    setState(State::Idle);
    setError(QString());
    logDialogStateBreadcrumb();
}

void UpdaterController::remindLater()
{
    SentryReporter::addBreadcrumb(QStringLiteral("updater.dialog.action"),
                                  QStringLiteral("remind_later"));
    dismiss();
}

void UpdaterController::skipThisVersion()
{
    if (m_latestVersion.isEmpty()) {
        dismiss();
        return;
    }
    QSettings settings;
    settings.setValue(AppSettingsKeys::updaterSkippedVersion(), m_latestVersion);
    SentryReporter::addBreadcrumb(QStringLiteral("updater.dialog.action"),
                                  QStringLiteral("skip_version=%1").arg(m_latestVersion));
    setState(State::UpToDate);
    logDialogStateBreadcrumb();
}

void UpdaterController::openReleasePage()
{
    if (m_latestUrl.isEmpty()) {
        return;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("updater.dialog.action"),
                                  QStringLiteral("open_release_page"));
    QDesktopServices::openUrl(QUrl(m_latestUrl));
}

void UpdaterController::copyUpdateCommand()
{
    const QString hint = updateCommandHint();
    if (hint.isEmpty()) {
        return;
    }
    if (QGuiApplication::clipboard()) {
        QGuiApplication::clipboard()->setText(hint);
    }
    SentryReporter::addBreadcrumb(QStringLiteral("updater.dialog.action"),
                                  QStringLiteral("copy_update_command"));
}

#ifdef QTMESH_UNIT_TESTS
void UpdaterController::setLatestVersionForTest(const QString& tag)
{
    if (m_latestVersion == tag) {
        return;
    }
    m_latestVersion = tag;
    emit latestVersionChanged();
}
#endif
