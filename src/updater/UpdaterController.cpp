#include "UpdaterController.h"
#include "ArtifactResolver.h"
#include "UpdaterInstaller.h"
#include "UpdaterTelemetry.h"
#include "UpdaterWorker.h"
#include "AppSettingsKeys.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QSettings>
#include <QStandardPaths>
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
    case UpdaterController::State::Installing: return QStringLiteral("installing");
    }
    return QStringLiteral("idle");
}

} // namespace

UpdaterController* UpdaterController::s_instance = nullptr;
bool UpdaterController::s_sessionBackgroundChecksDisabled = false;

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

void UpdaterController::setSessionBackgroundChecksDisabled(bool disabled)
{
    s_sessionBackgroundChecksDisabled = disabled;
}

bool UpdaterController::sessionBackgroundChecksDisabled()
{
    return s_sessionBackgroundChecksDisabled;
}

UpdaterController::UpdaterController(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<GitHubReleaseParser::Channel>("GitHubReleaseParser::Channel");
    qRegisterMetaType<DownloadRequest>("DownloadRequest");
    qRegisterMetaType<DownloadOutcome>("DownloadOutcome");
    qRegisterMetaType<VerifyRequest>("VerifyRequest");
    qRegisterMetaType<VerifyOutcome>("VerifyOutcome");
    loadSettings();
    refreshInstallFlavor();
    applyDefaultStartupCheckIfNeeded();

    m_workerThread = new QThread(this);
    m_worker = new UpdaterWorker();
    m_worker->moveToThread(m_workerThread);

    connect(m_worker, &UpdaterWorker::checkFinished, this,
            [this](const GitHubReleaseParser::CheckResult& result, const QString& networkError) {
                const bool wasSilent = m_silentCheck;
                recordLastChecked();

                if (!networkError.isEmpty()) {
                    if (wasSilent) {
                        setState(State::Idle);
                        UpdaterTelemetry::breadcrumb(
                            QStringLiteral("updater.background.error"),
                            QStringLiteral("network_error"),
                            QStringLiteral("error"));
                        finishSilentCheck();
                        return;
                    }
                    setState(State::Error);
                    setError(networkError);
                    UpdaterTelemetry::breadcrumb(
                        QStringLiteral("updater.check.error"),
                        QStringLiteral("network_error"),
                        QStringLiteral("error"));
                    emit checkError(networkError);
                    logDialogStateBreadcrumb();
                    finishSilentCheck();
                    return;
                }

                if (!result.parseOk) {
                    if (wasSilent) {
                        setState(State::Idle);
                        UpdaterTelemetry::breadcrumb(
                            QStringLiteral("updater.background.error"),
                            QStringLiteral("parse_error"),
                            QStringLiteral("error"));
                        finishSilentCheck();
                        return;
                    }
                    setState(State::Error);
                    setError(result.errorMessage);
                    UpdaterTelemetry::breadcrumb(
                        QStringLiteral("updater.check.error"),
                        QStringLiteral("parse_error"),
                        QStringLiteral("error"));
                    emit checkError(result.errorMessage);
                    logDialogStateBreadcrumb();
                    finishSilentCheck();
                    return;
                }

                applyCheckResult(result);
                if (result.comparison != UpdateVersion::Comparison::Invalid) {
                    UpdaterTelemetry::breadcrumb(
                        QStringLiteral("updater.check.success"),
                        QStringLiteral("remote=%1 comparison=%2")
                            .arg(result.release.tagName)
                            .arg(static_cast<int>(result.comparison)));
                }
                logDialogStateBreadcrumb();
                finishSilentCheck();
            });

    connect(m_worker, &UpdaterWorker::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                if (total <= 0) {
                    return;
                }
                if (!m_progressThrottle.isValid()) {
                    m_progressThrottle.start();
                }
                const qint64 nowMs = m_progressThrottle.elapsed();
                const int percent =
                    static_cast<int>((received * 100LL) / total);
                const int clamped = qBound(0, percent, 100);
                if (clamped >= 100 || nowMs - m_lastProgressEmitMs >= 1000) {
                    m_lastProgressEmitMs = nowMs;
                    setProgressPercent(clamped);
                    UpdaterTelemetry::breadcrumb(
                        QStringLiteral("updater.download.progress"),
                        QStringLiteral("percent=%1").arg(clamped));
                }
            });

    connect(m_worker, &UpdaterWorker::downloadFinished, this,
            &UpdaterController::handleDownloadFinished);
    connect(m_worker, &UpdaterWorker::verifyFinished, this,
            &UpdaterController::handleVerifyFinished);

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
    bool channelOk = false;
    const GitHubReleaseParser::Channel parsed = GitHubReleaseParser::channelFromString(
        settings.value(AppSettingsKeys::updaterChannel(), QStringLiteral("stable")).toString(),
        &channelOk);
    m_channel = GitHubReleaseParser::channelToString(
        channelOk ? parsed : GitHubReleaseParser::Channel::Stable);
    m_autoDownload = settings.value(AppSettingsKeys::updaterAutoDownload(), false).toBool();
    m_lastCheckedAt = settings.value(AppSettingsKeys::updaterLastCheckedAt()).toString();
}

void UpdaterController::applyDefaultStartupCheckIfNeeded()
{
    QSettings settings;
    const bool previous = m_checkOnStartup;
    if (settings.contains(AppSettingsKeys::updaterCheckOnStartup())) {
        m_checkOnStartup = settings.value(AppSettingsKeys::updaterCheckOnStartup()).toBool();
    } else {
        m_checkOnStartup = (m_flavor == InstallFlavor::Flavor::Portable);
    }
    if (m_checkOnStartup != previous) {
        emit checkOnStartupChanged();
    }
}

bool UpdaterController::isWithinRateLimit() const
{
    if (m_lastCheckedAt.isEmpty()) {
        return true;
    }
    const QDateTime lastChecked =
        QDateTime::fromString(m_lastCheckedAt, Qt::ISODate);
    if (!lastChecked.isValid()) {
        return true;
    }
    return lastChecked.secsTo(QDateTime::currentDateTimeUtc()) >= 24 * 3600;
}

bool UpdaterController::shouldRunBackgroundCheck(QString* skipReason) const
{
    if (s_sessionBackgroundChecksDisabled) {
        if (skipReason) {
            *skipReason = QStringLiteral("session_disabled");
        }
        return false;
    }
    if (!m_checkOnStartup) {
        if (skipReason) {
            *skipReason = QStringLiteral("check_on_startup_off");
        }
        return false;
    }
    if (!isWithinRateLimit()) {
        if (skipReason) {
            *skipReason = QStringLiteral("rate_limited");
        }
        return false;
    }
    if (m_flavor != InstallFlavor::Flavor::Portable) {
        if (skipReason) {
            *skipReason = QStringLiteral("flavor=%1").arg(m_installFlavor);
        }
        return false;
    }
    return true;
}

void UpdaterController::finishSilentCheck()
{
    m_silentCheck = false;
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
    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.flavor.detected"), slug);
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
    UpdaterTelemetry::breadcrumb(
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
    m_releaseAssets = result.release.assets;
    emit latestVersionChanged();
    emit latestUrlChanged();
    emit changelogChanged();
    emit publishedAtChanged();

    switch (result.comparison) {
    case UpdateVersion::Comparison::Older:
        if (isVersionSkipped(m_latestVersion)) {
            if (!m_silentCheck) {
                setState(State::UpToDate);
                emit noUpdate();
            } else {
                setState(State::Idle);
            }
            return;
        }
        setState(State::UpdateAvailable);
        if (m_silentCheck) {
            UpdaterTelemetry::breadcrumb(
                QStringLiteral("updater.background.available"),
                QStringLiteral("version=%1").arg(m_latestVersion));
            emit backgroundUpdateAvailable(m_latestVersion);
        } else {
            emit updateAvailable(m_currentVersion, m_latestVersion);
        }
        if (m_autoDownload) {
            m_showDialogWhenReady = m_silentCheck;
        }
        beginDownloadIfNeeded(false);
        break;
    case UpdateVersion::Comparison::Same:
    case UpdateVersion::Comparison::Newer:
        if (!m_silentCheck) {
            setState(State::UpToDate);
            emit noUpdate();
        } else {
            setState(State::Idle);
        }
        break;
    case UpdateVersion::Comparison::Invalid:
        if (m_silentCheck) {
            UpdaterTelemetry::breadcrumb(
                QStringLiteral("updater.background.error"),
                QStringLiteral("compare_error"),
                QStringLiteral("error"));
            return;
        }
        setState(State::Error);
        setError(QStringLiteral("Could not compare versions '%1' / '%2'")
                     .arg(m_currentVersion, m_latestVersion));
        emit checkError(m_error);
        break;
    }
}

void UpdaterController::checkForUpdatesInBackground()
{
    QString skipReason;
    if (!shouldRunBackgroundCheck(&skipReason)) {
        UpdaterTelemetry::breadcrumb(QStringLiteral("updater.background.skip"), skipReason);
        return;
    }

    UpdaterTelemetry::breadcrumb(
        QStringLiteral("updater.background.start"),
        QStringLiteral("channel=%1").arg(m_channel));
    m_silentCheck = true;
    checkForUpdates();
}

void UpdaterController::requestCheckDialog()
{
    UpdaterTelemetry::breadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Updater settings: Check now"));
    emit showDialogRequested(true);
}

void UpdaterController::openUpdateDialog()
{
    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.background.action"),
                                  QStringLiteral("open_dialog"));
    emit showDialogRequested(false);
}

void UpdaterController::checkForUpdates()
{
    refreshInstallFlavor();
    m_currentVersion = QApplication::applicationVersion();
    emit currentVersionChanged();
    setError(QString());

    if (InstallFlavor::isPackageManagerManaged(m_flavor)) {
        UpdaterTelemetry::breadcrumb(
            QStringLiteral("updater.check.start"),
            QStringLiteral("package-manager flavor=%1").arg(m_installFlavor));
        if (!m_silentCheck) {
            setState(State::PackageManaged);
            logDialogStateBreadcrumb();
        } else {
            finishSilentCheck();
        }
        return;
    }

    if (m_flavor == InstallFlavor::Flavor::Unknown && !m_unknownInstallConfirmed) {
        if (!m_silentCheck) {
            setState(State::UnknownInstall);
            logDialogStateBreadcrumb();
        } else {
            finishSilentCheck();
        }
        return;
    }

    if (m_state == State::Checking) {
        if (m_silentCheck) {
            finishSilentCheck();
        }
        return;
    }

    UpdaterTelemetry::breadcrumb(
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
    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.dialog.action"),
                                  QStringLiteral("confirm_unknown_install"));
    checkForUpdates();
}

void UpdaterController::downloadAndInstall()
{
    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.download.start"),
                                  QStringLiteral("version=%1").arg(m_latestVersion));
    beginDownloadIfNeeded(true);
}

void UpdaterController::installUpdate()
{
    if (m_state != State::ReadyToInstall) {
        return;
    }

    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.install.start"),
                                  QStringLiteral("version=%1").arg(m_latestVersion));
    setError(QString());
    setState(State::Installing);
    logDialogStateBreadcrumb();

    UpdaterInstaller::InstallContext context;
    context.stagedArtifactPath = m_stagedArtifactPath;
    context.releaseTag = m_latestVersion;
    context.executablePath = QCoreApplication::applicationFilePath();
    context.installRoot = UpdaterInstaller::resolveInstallRoot(context.executablePath);
    context.parentPid = QCoreApplication::applicationPid();

    const UpdaterInstaller::InstallPlan plan = UpdaterInstaller::prepareInstall(context);
    if (!plan.ok) {
        UpdaterTelemetry::breadcrumb(QStringLiteral("updater.install.error"),
                                      QStringLiteral("prepare_failed"),
                                      QStringLiteral("error"));
        setError(plan.errorMessage.isEmpty()
                     ? tr("Could not prepare the update for installation.")
                     : plan.errorMessage);
        setState(State::Error);
        logDialogStateBreadcrumb();
        return;
    }

    if (!UpdaterInstaller::launchRelauncher(plan)) {
        UpdaterTelemetry::breadcrumb(QStringLiteral("updater.install.error"),
                                      QStringLiteral("relauncher missing or failed to start"),
                                      QStringLiteral("error"));
        setError(tr("Could not launch the update installer. "
                    "Make sure qtmesh-relauncher is next to the application binary."));
        setState(State::Error);
        logDialogStateBreadcrumb();
        return;
    }

    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.install.relaunch"),
                                  QStringLiteral("version=%1").arg(m_latestVersion));
    QApplication::quit();
}

void UpdaterController::beginDownloadIfNeeded(bool userInitiated)
{
    if (m_state != State::UpdateAvailable) {
        return;
    }
    if (!userInitiated && !m_autoDownload) {
        return;
    }
    if (InstallFlavor::isPackageManagerManaged(m_flavor)) {
        setError(tr("Updates for this install are managed by %1.")
                     .arg(InstallFlavor::displayName(m_flavor)));
        setState(State::Error);
        return;
    }
    startDownloadJob();
}

void UpdaterController::startDownloadJob()
{
    const ArtifactResolver::ResolvedArtifact artifact =
        ArtifactResolver::resolveForCurrentPlatform(m_releaseAssets, m_flavor);
    if (!artifact.ok) {
        setError(artifact.errorMessage);
        setState(State::Error);
        UpdaterTelemetry::breadcrumb(QStringLiteral("updater.download.error"),
                                    QStringLiteral("artifact_resolve_failed"),
                                    QStringLiteral("error"));
        logDialogStateBreadcrumb();
        return;
    }

    const QString stagingRoot =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/updater/staging/")
        + m_latestVersion;
    QDir().mkpath(stagingRoot);

    DownloadRequest request;
    request.fileName = artifact.fileName;
    request.artifactUrl = artifact.downloadUrl;
    request.artifactPartPath =
        QDir(stagingRoot).filePath(artifact.fileName + QStringLiteral(".part"));
    request.artifactFinalPath = QDir(stagingRoot).filePath(artifact.fileName);
    request.signatureUrl = artifact.signatureUrl;
    request.signaturePath = QDir(stagingRoot).filePath(artifact.signatureFileName);
    if (!artifact.sha256SumsUrl.isEmpty()) {
        request.sha256SumsUrl = artifact.sha256SumsUrl;
        request.sha256SumsPath = QDir(stagingRoot).filePath(artifact.sha256SumsFileName);
    }

    m_stagedArtifactPath = request.artifactFinalPath;
    setError(QString());
    setProgressPercent(0);
    m_lastProgressEmitMs = 0;
    setState(State::Downloading);
    logDialogStateBreadcrumb();

    QMetaObject::invokeMethod(m_worker,
                              "downloadUpdate",
                              Qt::QueuedConnection,
                              Q_ARG(DownloadRequest, request));
}

void UpdaterController::handleDownloadFinished(const DownloadOutcome& outcome)
{
    if (outcome.cancelled) {
        UpdaterTelemetry::breadcrumb(QStringLiteral("updater.download.cancel"),
                                    QStringLiteral("version=%1").arg(m_latestVersion));
        setState(State::UpdateAvailable);
        setProgressPercent(0);
        logDialogStateBreadcrumb();
        return;
    }

    if (!outcome.ok) {
        UpdaterTelemetry::breadcrumb(QStringLiteral("updater.download.error"),
                                      QStringLiteral("download_failed"),
                                      QStringLiteral("error"));
        setError(outcome.errorMessage);
        setState(State::Error);
        logDialogStateBreadcrumb();
        return;
    }

    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.download.complete"),
                                  QStringLiteral("version=%1").arg(m_latestVersion));
    setProgressPercent(100);
    setState(State::Verifying);
    logDialogStateBreadcrumb();

    VerifyRequest verifyRequest;
    verifyRequest.artifactPath = outcome.artifactPath;
    verifyRequest.signaturePath = outcome.signaturePath;
    verifyRequest.sha256SumsPath = outcome.sha256SumsPath;
    verifyRequest.fileName = outcome.fileName;

    QMetaObject::invokeMethod(m_worker,
                              "verifyDownload",
                              Qt::QueuedConnection,
                              Q_ARG(VerifyRequest, verifyRequest));
}

void UpdaterController::handleVerifyFinished(const VerifyOutcome& outcome)
{
    if (!outcome.ok) {
        UpdaterTelemetry::breadcrumb(
            QStringLiteral("updater.verify.failure"),
            QStringLiteral("stage=%1").arg(outcome.failedStage),
            QStringLiteral("error"));
        setError(outcome.errorMessage.isEmpty()
                     ? tr("Download verification failed.")
                     : outcome.errorMessage);
        setState(State::Error);
        logDialogStateBreadcrumb();
        return;
    }

    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.verify.success"), m_latestVersion);
    setState(State::ReadyToInstall);
    logDialogStateBreadcrumb();
    if (m_showDialogWhenReady) {
        m_showDialogWhenReady = false;
        emit showDialogRequested(false);
    }
}

void UpdaterController::setProgressPercent(int percent)
{
    const int clamped = qBound(0, percent, 100);
    if (m_progress == clamped) {
        return;
    }
    m_progress = clamped;
    emit progressChanged();
}

void UpdaterController::cancel()
{
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, &UpdaterWorker::cancelActiveRequest,
                                  Qt::QueuedConnection);
    }
    if (m_state == State::Checking || m_state == State::Downloading
        || m_state == State::Verifying) {
        if (m_state == State::Downloading) {
            setState(State::UpdateAvailable);
        } else {
            setState(State::Idle);
        }
        setProgressPercent(0);
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
    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.dialog.action"),
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
    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.dialog.action"),
                                  QStringLiteral("skip_version=%1").arg(m_latestVersion));
    setState(State::UpToDate);
    logDialogStateBreadcrumb();
}

void UpdaterController::openReleasePage()
{
    if (m_latestUrl.isEmpty()) {
        return;
    }
    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.dialog.action"),
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
    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.dialog.action"),
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

void UpdaterController::setLastCheckedAtForTest(const QString& isoUtc)
{
    m_lastCheckedAt = isoUtc;
    emit lastCheckedAtChanged();
}

void UpdaterController::setInstallFlavorForTest(InstallFlavor::Flavor flavor)
{
    m_flavor = flavor;
    m_installFlavor = InstallFlavor::toSlug(flavor);
    emit installFlavorChanged();
}
#endif
