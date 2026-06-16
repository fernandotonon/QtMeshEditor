#include "UpdaterController.h"
#include "UpdaterWorker.h"
#include "SentryReporter.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QUrl>

namespace {

constexpr const char* kDefaultReleasesApi =
    "https://api.github.com/repos/fernandotonon/QtMeshEditor/releases?per_page=20";

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

UpdaterController::UpdaterController(QObject* parent)
    : QObject(parent)
{
    qRegisterMetaType<GitHubReleaseParser::Channel>("GitHubReleaseParser::Channel");
    refreshInstallFlavor();

    m_workerThread = new QThread(this);
    m_worker = new UpdaterWorker();
    m_worker->moveToThread(m_workerThread);

    connect(m_worker, &UpdaterWorker::checkFinished, this,
            [this](const GitHubReleaseParser::CheckResult& result, const QString& networkError) {
                if (!networkError.isEmpty()) {
                    setState(State::Error);
                    setError(networkError);
                    SentryReporter::addBreadcrumb(
                        QStringLiteral("updater.check.error"),
                        networkError,
                        QStringLiteral("error"));
                    emit checkError(networkError);
                    presentCheckOutcome();
                    return;
                }

                applyCheckResult(result);
                if (!result.parseOk) {
                    setState(State::Error);
                    setError(result.errorMessage);
                    SentryReporter::addBreadcrumb(
                        QStringLiteral("updater.check.error"),
                        result.errorMessage,
                        QStringLiteral("error"));
                    emit checkError(result.errorMessage);
                    presentCheckOutcome();
                    return;
                }

                SentryReporter::addBreadcrumb(
                    QStringLiteral("updater.check.success"),
                    QStringLiteral("remote=%1 comparison=%2")
                        .arg(result.release.tagName)
                        .arg(static_cast<int>(result.comparison)));
                presentCheckOutcome();
            });

    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_workerThread->start();
}

UpdaterController::~UpdaterController()
{
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, &UpdaterWorker::cancelActiveRequest,
                                  Qt::BlockingQueuedConnection);
    }
    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait();
    }
}

void UpdaterController::refreshInstallFlavor()
{
    m_flavor = InstallFlavor::detect(QCoreApplication::applicationFilePath());
    const QString slug = InstallFlavor::toSlug(m_flavor);
    if (slug != m_installFlavor) {
        m_installFlavor = slug;
        emit installFlavorChanged();
    }
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
    emit channelChanged();
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

void UpdaterController::applyCheckResult(const GitHubReleaseParser::CheckResult& result)
{
    m_lastComparison = result.comparison;
    m_latestVersion = result.release.tagName;
    m_latestUrl = result.release.htmlUrl;
    m_changelog = result.release.body;
    emit latestVersionChanged();
    emit latestUrlChanged();
    emit changelogChanged();

    switch (result.comparison) {
    case UpdateVersion::Comparison::Older:
        setState(State::UpdateAvailable);
        emit updateAvailable(m_localVersion, m_latestVersion);
        break;
    case UpdateVersion::Comparison::Same:
    case UpdateVersion::Comparison::Newer:
        setState(State::UpToDate);
        emit noUpdate();
        break;
    case UpdateVersion::Comparison::Invalid:
        setState(State::Error);
        setError(QStringLiteral("Could not compare versions '%1' / '%2'")
                     .arg(m_localVersion, m_latestVersion));
        emit checkError(m_error);
        break;
    }
}

void UpdaterController::presentCheckOutcome()
{
    if (InstallFlavor::isPackageManagerManaged(m_flavor)) {
        const QString hint = InstallFlavor::updateCommandHint(m_flavor);
        QMessageBox::information(
            nullptr,
            tr("Update"),
            tr("Updates for %1 installs are managed by your package manager.\n\nRun:\n%2")
                .arg(InstallFlavor::displayName(m_flavor), hint));
        setState(State::Idle);
        return;
    }

    switch (m_state) {
    case State::UpdateAvailable: {
        const QMessageBox::StandardButton choice = QMessageBox::question(
            nullptr,
            tr("Update"),
            tr("A new version is available (%1 → %2). Do you want to open the release page?")
                .arg(m_localVersion, m_latestVersion),
            QMessageBox::Yes | QMessageBox::No);
        if (choice == QMessageBox::Yes) {
            SentryReporter::addBreadcrumb(QStringLiteral("updater.check"),
                                          QStringLiteral("Open release page"));
            QDesktopServices::openUrl(QUrl(m_latestUrl));
        }
        setState(State::Idle);
        break;
    }
    case State::UpToDate:
        QMessageBox::information(nullptr, tr("Update"), tr("You're using the latest release."));
        setState(State::Idle);
        break;
    case State::Error:
        QMessageBox::warning(
            nullptr,
            tr("Update"),
            tr("Could not check for updates.\n\n%1").arg(m_error));
        setState(State::Idle);
        break;
    default:
        break;
    }
}

void UpdaterController::checkForUpdates()
{
    refreshInstallFlavor();
    m_localVersion = QApplication::applicationVersion();
    setError(QString());

    if (InstallFlavor::isPackageManagerManaged(m_flavor)) {
        SentryReporter::addBreadcrumb(
            QStringLiteral("updater.check.start"),
            QStringLiteral("package-manager flavor=%1").arg(m_installFlavor));
        presentCheckOutcome();
        return;
    }

    if (m_state == State::Checking) {
        return;
    }

    SentryReporter::addBreadcrumb(
        QStringLiteral("updater.check.start"),
        QStringLiteral("channel=%1 local=%2").arg(m_channel, m_localVersion));
    setState(State::Checking);

    const QString localVersion = m_localVersion;
    const auto channel = activeChannel();
    QMetaObject::invokeMethod(
        m_worker,
        "checkForUpdates",
        Qt::QueuedConnection,
        Q_ARG(QString, QString::fromUtf8(kDefaultReleasesApi)),
        Q_ARG(QString, localVersion),
        Q_ARG(GitHubReleaseParser::Channel, channel));
}

void UpdaterController::downloadAndInstall()
{
    SentryReporter::addBreadcrumb(QStringLiteral("updater.download"),
                                  QStringLiteral("downloadAndInstall not implemented yet"));
    setError(tr("Automatic download and install is not available yet."));
    setState(State::Error);
    emit checkError(m_error);
}

void UpdaterController::cancel()
{
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, &UpdaterWorker::cancelActiveRequest,
                                  Qt::QueuedConnection);
    }
    if (m_state == State::Checking) {
        setState(State::Idle);
    }
}

void UpdaterController::dismiss()
{
    setState(State::Idle);
    setError(QString());
}
