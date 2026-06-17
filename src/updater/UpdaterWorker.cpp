#include "UpdaterWorker.h"
#include "UpdateVerifier.h"

#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace {

QNetworkRequest makeRequest(const QUrl& url)
{
    QNetworkRequest httpRequest(url);
    httpRequest.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("QtMeshEditor-Updater/1.0"));
    httpRequest.setRawHeader("Accept", "*/*");
    return httpRequest;
}

} // namespace

UpdaterWorker::UpdaterWorker(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    qRegisterMetaType<DownloadRequest>("DownloadRequest");
    qRegisterMetaType<DownloadOutcome>("DownloadOutcome");
    qRegisterMetaType<VerifyRequest>("VerifyRequest");
    qRegisterMetaType<VerifyOutcome>("VerifyOutcome");
}

UpdaterWorker::~UpdaterWorker()
{
    cancelActiveRequest();
}

void UpdaterWorker::cancelActiveRequest()
{
    if (!m_activeReply) {
        m_activeJob = ActiveJob::None;
        return;
    }
    m_activeReply->abort();
    m_activeReply->deleteLater();
    m_activeReply = nullptr;
    m_activeJob = ActiveJob::None;
}

void UpdaterWorker::checkForUpdates(const QString& apiUrl,
                                    const QString& localVersion,
                                    GitHubReleaseParser::Channel channel)
{
    cancelActiveRequest();

    const QUrl url(apiUrl);
    QNetworkRequest httpRequest(url);
    httpRequest.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("QtMeshEditor-Updater/1.0"));
    httpRequest.setRawHeader("Accept", "application/vnd.github+json");

    m_activeJob = ActiveJob::Check;
    m_activeReply = m_network->get(httpRequest);
    m_activeReply->setProperty("localVersion", localVersion);
    m_activeReply->setProperty("channel", static_cast<int>(channel));
    connect(m_activeReply, &QNetworkReply::finished, this, &UpdaterWorker::onCheckReplyFinished);
}

void UpdaterWorker::onCheckReplyFinished()
{
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || reply != m_activeReply) {
        if (reply) {
            reply->deleteLater();
        }
        return;
    }

    m_activeReply = nullptr;
    m_activeJob = ActiveJob::None;

    if (reply->error() == QNetworkReply::OperationCanceledError) {
        reply->deleteLater();
        return;
    }

    const QString localVersion = reply->property("localVersion").toString();
    const auto channel =
        static_cast<GitHubReleaseParser::Channel>(reply->property("channel").toInt());

    GitHubReleaseParser::CheckResult result;
    QString networkError;

    if (reply->error() != QNetworkReply::NoError) {
        networkError = reply->errorString();
    } else {
        result = GitHubReleaseParser::pickLatestForChannel(reply->readAll(), channel, localVersion);
        if (!result.parseOk && result.errorMessage.isEmpty()) {
            result.errorMessage = QStringLiteral("Failed to parse GitHub releases response");
        }
    }

    reply->deleteLater();
    emit checkFinished(result, networkError);
}

void UpdaterWorker::downloadUpdate(const DownloadRequest& request)
{
    cancelActiveRequest();
    m_downloadRequest = request;
    m_downloadAttempt = 0;
    m_activeJob = ActiveJob::Download;
    startArtifactDownloadAttempt(0);
}

void UpdaterWorker::startArtifactDownloadAttempt(int attemptIndex)
{
    m_downloadAttempt = attemptIndex;

    const auto beginDownload = [this]() {
        if (m_activeJob != ActiveJob::Download) {
            return;
        }

    qint64 existingSize = 0;
    if (QFile::exists(m_downloadRequest.artifactPartPath)) {
        existingSize = QFileInfo(m_downloadRequest.artifactPartPath).size();
    }

    QNetworkRequest httpRequest = makeRequest(QUrl(m_downloadRequest.artifactUrl));
    if (existingSize > 0) {
        httpRequest.setRawHeader("Range",
                                 QByteArray("bytes=" + QByteArray::number(existingSize) + '-'));
    }

    m_activeReply = m_network->get(httpRequest);
    connect(m_activeReply, &QNetworkReply::downloadProgress, this, &UpdaterWorker::onDownloadProgress);
    connect(m_activeReply, &QNetworkReply::finished, this, &UpdaterWorker::onDownloadReplyFinished);

    if (existingSize > 0) {
        m_activeReply->setProperty("resumeOffset", existingSize);
    }
    };

    if (attemptIndex > 0) {
        const int delayMs = kRetryDelaysMs[qMin(attemptIndex, kMaxDownloadAttempts - 1)];
        QTimer::singleShot(delayMs, this, beginDownload);
    } else {
        beginDownload();
    }
}

void UpdaterWorker::onDownloadProgress(qint64 received, qint64 total)
{
    if (m_activeJob != ActiveJob::Download || !m_activeReply) {
        return;
    }
    const qint64 resumeOffset = m_activeReply->property("resumeOffset").toLongLong();
    const qint64 combinedReceived = received + resumeOffset;
    const qint64 combinedTotal = total > 0 ? total + resumeOffset : total;
    emit downloadProgress(combinedReceived, combinedTotal);
}

void UpdaterWorker::finishDownloadWithError(const QString& message, bool cancelled)
{
    DownloadOutcome outcome;
    outcome.cancelled = cancelled;
    outcome.errorMessage = message;
    outcome.fileName = m_downloadRequest.fileName;
    m_activeJob = ActiveJob::None;
    emit downloadFinished(outcome);
}

bool UpdaterWorker::downloadUrlBlocking(const QString& url,
                                        const QString& destPath,
                                        bool resume,
                                        QString* errorMessage)
{
    qint64 existingSize = 0;
    if (resume && QFile::exists(destPath)) {
        existingSize = QFileInfo(destPath).size();
    }

    QNetworkRequest httpRequest = makeRequest(QUrl(url));
    if (existingSize > 0) {
        httpRequest.setRawHeader("Range",
                                 QByteArray("bytes=" + QByteArray::number(existingSize) + '-'));
    }

    QNetworkReply* reply = m_network->get(httpRequest);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const bool wasCancelled = reply->error() == QNetworkReply::OperationCanceledError;
    const bool ok = reply->error() == QNetworkReply::NoError;
    const QByteArray payload = reply->readAll();
    const QString errorString = reply->errorString();
    reply->deleteLater();

    if (wasCancelled) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Download cancelled");
        }
        return false;
    }

    if (!ok) {
        if (errorMessage) {
            *errorMessage = errorString;
        }
        return false;
    }

    QIODevice::OpenMode mode = QIODevice::WriteOnly;
    if (existingSize > 0) {
        mode |= QIODevice::Append;
    }
    QFile out(destPath);
    if (!out.open(mode)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot write %1").arg(destPath);
        }
        return false;
    }
    if (out.write(payload) != payload.size()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Write failed for %1").arg(destPath);
        }
        return false;
    }
    return true;
}

void UpdaterWorker::onDownloadReplyFinished()
{
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || reply != m_activeReply) {
        if (reply) {
            reply->deleteLater();
        }
        return;
    }

    m_activeReply = nullptr;

    if (reply->error() == QNetworkReply::OperationCanceledError) {
        reply->deleteLater();
        finishDownloadWithError(QStringLiteral("Download cancelled"), true);
        return;
    }

    const qint64 resumeOffset = reply->property("resumeOffset").toLongLong();
    const bool append = resumeOffset > 0;
    const QByteArray payload = reply->readAll();
    const bool httpOk = reply->error() == QNetworkReply::NoError;
    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    if (!httpOk) {
        if (m_downloadAttempt + 1 < kMaxDownloadAttempts) {
            startArtifactDownloadAttempt(m_downloadAttempt + 1);
            return;
        }
        finishDownloadWithError(QStringLiteral("Download failed after %1 attempts")
                                    .arg(kMaxDownloadAttempts));
        return;
    }

    if (httpStatus == 416 && append) {
        QFile::remove(m_downloadRequest.artifactPartPath);
        startArtifactDownloadAttempt(0);
        return;
    }

    QIODevice::OpenMode mode = QIODevice::WriteOnly;
    if (append) {
        mode |= QIODevice::Append;
    }
    QFile partFile(m_downloadRequest.artifactPartPath);
    if (!partFile.open(mode)) {
        finishDownloadWithError(QStringLiteral("Cannot write download to disk"));
        return;
    }
    if (partFile.write(payload) != payload.size()) {
        finishDownloadWithError(QStringLiteral("Download write failed"));
        return;
    }
    partFile.close();

    if (QFile::exists(m_downloadRequest.artifactFinalPath)) {
        QFile::remove(m_downloadRequest.artifactFinalPath);
    }
    if (!QFile::rename(m_downloadRequest.artifactPartPath, m_downloadRequest.artifactFinalPath)) {
        finishDownloadWithError(QStringLiteral("Could not finalize downloaded file"));
        return;
    }

    QString sidecarError;
    if (!downloadUrlBlocking(m_downloadRequest.signatureUrl,
                             m_downloadRequest.signaturePath,
                             false,
                             &sidecarError)) {
        finishDownloadWithError(sidecarError);
        return;
    }

    if (!m_downloadRequest.sha256SumsUrl.isEmpty()) {
        if (!downloadUrlBlocking(m_downloadRequest.sha256SumsUrl,
                                 m_downloadRequest.sha256SumsPath,
                                 false,
                                 &sidecarError)) {
            finishDownloadWithError(sidecarError);
            return;
        }
    }

    DownloadOutcome outcome;
    outcome.ok = true;
    outcome.artifactPath = m_downloadRequest.artifactFinalPath;
    outcome.signaturePath = m_downloadRequest.signaturePath;
    outcome.sha256SumsPath = m_downloadRequest.sha256SumsPath;
    outcome.fileName = m_downloadRequest.fileName;
    m_activeJob = ActiveJob::None;
    emit downloadFinished(outcome);
}

void UpdaterWorker::verifyDownload(const VerifyRequest& request)
{
    VerifyOutcome outcome;
    const UpdateVerifier::Outcome verify = UpdateVerifier::verifyDownloadedArtifact(
        request.artifactPath,
        request.signaturePath,
        request.sha256SumsPath,
        request.fileName);
    outcome.ok = verify.ok;
    outcome.errorMessage = verify.errorMessage;
    outcome.failedStage = verify.failedStage;
    emit verifyFinished(outcome);
}
