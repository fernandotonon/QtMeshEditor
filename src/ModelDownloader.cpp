#include "ModelDownloader.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QUrl>
#include <QMetaObject>
#include <QDir>
#include <QDebug>

namespace {
// #1029: streamed SHA-256 of a file. Deliberately NOT the updater's
// sha256HexOfFile — that lives in qtmesh_updater, which is only built and
// linked when ENABLE_AUTO_UPDATER is ON, while ModelDownloader compiles
// unconditionally; reusing it broke the -DENABLE_AUTO_UPDATER=OFF link
// (review on #1038). Six lines of Qt API beat coupling every model download
// to the optional updater and its libsodium dependency.
QString sha256HexOfFile(const QString& path, QString* err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = QStringLiteral("cannot open %1 for hashing").arg(path);
        return {};
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) {
        if (err) *err = QStringLiteral("read error while hashing %1").arg(path);
        return {};
    }
    return QString::fromLatin1(h.result().toHex());
}
} // namespace

ModelDownloader* ModelDownloader::s_instance = nullptr;

ModelDownloader* ModelDownloader::instance()
{
    if (!s_instance) {
        s_instance = new ModelDownloader();
    }
    return s_instance;
}

ModelDownloader* ModelDownloader::qmlInstance(QQmlEngine *engine, QJSEngine *scriptEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(scriptEngine)
    return instance();
}

ModelDownloader::ModelDownloader(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_speedTimer(new QTimer(this))
{
    m_speedTimer->setInterval(1000); // Update speed every second
    connect(m_speedTimer, &QTimer::timeout, this, &ModelDownloader::onSpeedTimerTimeout);
}

ModelDownloader::~ModelDownloader()
{
    cancelDownload();
}

namespace {
// Empty = allowed; otherwise the reason, phrased for the user.
QString downloadUrlRejectionReason(const QString &url)
{
    const QUrl parsed(url);
    if (!parsed.isValid())
        return QStringLiteral("the URL is malformed");
    const QString scheme = parsed.scheme().toLower();
    if (scheme == QLatin1String("file")) {
        // file:// is for tests and a LOCAL mirror only. A non-empty host
        // ("file://server/share/model.onnx") is a UNC path on Windows — an
        // SMB fetch over the network wearing a local scheme, which would walk
        // straight around the https-only rule (review on #1038).
        const QString host = parsed.host().toLower();
        if (host.isEmpty() || host == QLatin1String("localhost")) return {};
        return QStringLiteral("file:// URLs must be local — 'file://%1/…' would "
                              "fetch from a remote share").arg(parsed.host());
    }
    if (scheme == QLatin1String("https")) {
        if (parsed.host().isEmpty())
            return QStringLiteral("the https:// URL has no host");
        return {};
    }
    // Everything else — notably plain http, which a MITM can rewrite
    // byte-for-byte — is refused.
    return QStringLiteral("scheme '%1' is not https:// (plain http can be tampered "
                          "with in transit)")
        .arg(scheme.isEmpty() ? QStringLiteral("(none)") : scheme);
}
} // namespace

bool ModelDownloader::isAllowedDownloadUrl(const QString &url)
{
    return downloadUrlRejectionReason(url).isEmpty();
}

void ModelDownloader::startDownload(const QString &url, const QString &destinationPath,
                                    const QString &modelName,
                                    const QString &expectedSha256)
{
    if (m_isDownloading) {
        emit downloadError(modelName, "A download is already in progress");
        return;
    }

    // #1029: refuse before touching the filesystem, so a rejected URL leaves
    // no .part file and no created directory behind.
    const QString rejection = downloadUrlRejectionReason(url);
    if (!rejection.isEmpty()) {
        // qCritical, not qWarning: the CLI message handler drops warnings unless
        // --verbose, and every consumer discards downloadError's text, so a
        // warning here would leave the user with a generic "offline?" and no
        // way to learn the real cause. A security refusal must always surface.
        qCritical() << "ModelDownloader: refusing" << modelName << "—" << rejection;
        // QUEUED, not direct: every consumer connects its handlers, calls
        // startDownload, then enters a nested QEventLoop. A synchronous emit
        // here would run their loop.quit() BEFORE exec() and be lost, hanging
        // them for their full timeout (the #1017 review race). Deferring one
        // event-loop turn lands the error inside exec() for all 21 consumers
        // without touching any of them.
        // Redact before it leaves this class (review: CWE-200). Every base URL
        // is user-configurable, so an override like
        // https://user:token@mirror/…?token=… is plausible, and this text
        // reaches the GUI status line via AIModelCatalog. Keep scheme, host and
        // path — that is what a user needs to see what they misconfigured.
        const QString shownUrl = QUrl(url).toString(
            QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment);
        const QString err = QString("Refusing to download %1: %2. URL: %3")
                                .arg(modelName, rejection,
                                     shownUrl.isEmpty() ? QStringLiteral("(malformed)") : shownUrl);
        QMetaObject::invokeMethod(this, [this, modelName, err]() {
            emit downloadError(modelName, err);
        }, Qt::QueuedConnection);
        return;
    }
    m_expectedSha256 = expectedSha256.trimmed();

    m_currentUrl = url;
    m_currentDestinationPath = destinationPath;
    m_currentModelName = modelName;
    m_tempFilePath = destinationPath + ".part";
    m_resumeOffset = 0;
    m_bytesReceived = 0;
    m_bytesTotal = 0;
    m_progress = 0.0f;
    m_downloadSpeed = 0.0f;
    m_lastBytesReceived = 0;

    // Create destination directory if it doesn't exist
    QFileInfo fileInfo(destinationPath);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // Check if partial download exists
    QFileInfo tempFileInfo(m_tempFilePath);
    if (tempFileInfo.exists()) {
        m_resumeOffset = tempFileInfo.size();
        qDebug() << "ModelDownloader: Resuming download from byte" << m_resumeOffset;
    }

    // Open output file
    m_outputFile = new QFile(m_tempFilePath, this);
    QIODevice::OpenMode mode = (m_resumeOffset > 0) ? QIODevice::Append : QIODevice::WriteOnly;
    if (!m_outputFile->open(mode)) {
        emit downloadError(modelName, QString("Cannot open file for writing: %1").arg(m_tempFilePath));
        delete m_outputFile;
        m_outputFile = nullptr;
        return;
    }

    // Create request with range header for resume support
    QUrl requestUrl(url);
    QNetworkRequest request(requestUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    if (m_resumeOffset > 0) {
        QString rangeHeader = QString("bytes=%1-").arg(m_resumeOffset);
        request.setRawHeader("Range", rangeHeader.toUtf8());
    }

    m_currentReply = m_networkManager->get(request);

    connect(m_currentReply, &QNetworkReply::readyRead, this, &ModelDownloader::onReadyRead);
    connect(m_currentReply, &QNetworkReply::downloadProgress, this, &ModelDownloader::onDownloadProgress);
    connect(m_currentReply, &QNetworkReply::finished, this, &ModelDownloader::onDownloadFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred, this, &ModelDownloader::onDownloadError);

    m_isDownloading = true;
    m_isPaused = false;
    m_speedTimer->start();

    emit isDownloadingChanged();
    emit currentModelNameChanged();
    emit downloadStarted(modelName);

    qDebug() << "ModelDownloader: Started download of" << modelName << "from" << url;
}

void ModelDownloader::pauseDownload()
{
    if (!m_isDownloading || m_isPaused || !m_currentReply) {
        return;
    }

    m_isPaused = true;
    m_speedTimer->stop();

    // Abort the current reply - data already written will be preserved
    m_currentReply->abort();
    m_currentReply->deleteLater();
    m_currentReply = nullptr;

    if (m_outputFile) {
        m_outputFile->flush();
        m_outputFile->close();
        delete m_outputFile;
        m_outputFile = nullptr;
    }

    emit downloadPaused(m_currentModelName);
    qDebug() << "ModelDownloader: Download paused at byte" << m_bytesReceived;
}

void ModelDownloader::resumeDownload()
{
    if (!m_isPaused || m_currentUrl.isEmpty()) {
        return;
    }

    m_resumeOffset = m_bytesReceived;

    // Reopen output file for appending
    m_outputFile = new QFile(m_tempFilePath, this);
    if (!m_outputFile->open(QIODevice::Append)) {
        emit downloadError(m_currentModelName, QString("Cannot open file for writing: %1").arg(m_tempFilePath));
        delete m_outputFile;
        m_outputFile = nullptr;
        return;
    }

    // Create request with range header
    QUrl requestUrl(m_currentUrl);
    QNetworkRequest request(requestUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QString rangeHeader = QString("bytes=%1-").arg(m_resumeOffset);
    request.setRawHeader("Range", rangeHeader.toUtf8());

    m_currentReply = m_networkManager->get(request);

    connect(m_currentReply, &QNetworkReply::readyRead, this, &ModelDownloader::onReadyRead);
    connect(m_currentReply, &QNetworkReply::downloadProgress, this, &ModelDownloader::onDownloadProgress);
    connect(m_currentReply, &QNetworkReply::finished, this, &ModelDownloader::onDownloadFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred, this, &ModelDownloader::onDownloadError);

    m_isPaused = false;
    m_speedTimer->start();

    emit downloadResumed(m_currentModelName);
    qDebug() << "ModelDownloader: Download resumed from byte" << m_resumeOffset;
}

void ModelDownloader::cancelDownload()
{
    m_speedTimer->stop();

    if (m_currentReply) {
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

    if (m_outputFile) {
        m_outputFile->close();
        m_outputFile->remove(); // Remove partial download
        delete m_outputFile;
        m_outputFile = nullptr;
    }

    // Also try to remove temp file
    QFile::remove(m_tempFilePath);

    QString canceledModel = m_currentModelName;

    m_isDownloading = false;
    m_isPaused = false;
    m_currentUrl.clear();
    m_currentDestinationPath.clear();
    m_currentModelName.clear();
    m_tempFilePath.clear();
    m_expectedSha256.clear();   // #1029: never let one download's digest leak into the next
    m_bytesReceived = 0;
    m_bytesTotal = 0;
    m_progress = 0.0f;
    m_downloadSpeed = 0.0f;
    m_resumeOffset = 0;

    emit isDownloadingChanged();
    emit currentModelNameChanged();
    emit downloadProgressChanged();
    emit bytesReceivedChanged();
    emit bytesTotalChanged();
    emit downloadSpeedChanged();

    if (!canceledModel.isEmpty()) {
        emit downloadCanceled(canceledModel);
        qDebug() << "ModelDownloader: Download canceled";
    }
}

void ModelDownloader::onReadyRead()
{
    if (m_outputFile && m_currentReply) {
        QByteArray data = m_currentReply->readAll();
        m_outputFile->write(data);
    }
}

void ModelDownloader::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    // Adjust for resume offset
    m_bytesReceived = m_resumeOffset + bytesReceived;

    // Handle Content-Range header for resumed downloads
    if (m_resumeOffset > 0 && bytesTotal > 0) {
        m_bytesTotal = m_resumeOffset + bytesTotal;
    } else if (bytesTotal > 0) {
        m_bytesTotal = bytesTotal;
    }

    if (m_bytesTotal > 0) {
        m_progress = static_cast<float>(m_bytesReceived) / static_cast<float>(m_bytesTotal);
    }

    emit bytesReceivedChanged();
    emit bytesTotalChanged();
    emit downloadProgressChanged();
    emit downloadProgressUpdated(m_currentModelName, m_bytesReceived, m_bytesTotal);
}

void ModelDownloader::onDownloadFinished()
{
    m_speedTimer->stop();

    if (m_currentReply && m_currentReply->error() == QNetworkReply::NoError) {
        // Flush and close the file
        if (m_outputFile) {
            m_outputFile->flush();
            m_outputFile->close();
            delete m_outputFile;
            m_outputFile = nullptr;
        }

        // #1029: verify the WHOLE finished .part file on disk, never a running
        // hash in onReadyRead — a resumed download appends to bytes this
        // process never saw, so only the file itself is authoritative.
        // Verify BEFORE the rename: a mismatched file must never reach the
        // destination path, where the next launch would trust and load it.
        bool integrityOk = true;
        if (!m_expectedSha256.isEmpty()) {
            QString why;
            const QString actual = sha256HexOfFile(m_tempFilePath, &why);
            integrityOk = !actual.isEmpty()
                          && actual.compare(m_expectedSha256, Qt::CaseInsensitive) == 0;
            if (!integrityOk && why.isEmpty())
                why = QStringLiteral("expected %1…, got %2…")
                          .arg(m_expectedSha256.left(12), actual.left(12));
            if (!integrityOk) {
                qCritical() << "ModelDownloader: SHA-256 mismatch for" << m_currentModelName
                           << "—" << why << "— deleting partial file";
                // Never leave a poisoned .part to resume from. QFile::remove CAN
                // fail (locked file, read-only directory); if it does, TRUNCATE
                // the file so a later resume starts from byte 0 instead of
                // appending to garbage — a later legacy (no-digest) download
                // would otherwise rename the resumed result unverified
                // (review: CWE-459). Report whichever cleanup happened.
                QString cleanup;
                if (!QFile::remove(m_tempFilePath)) {
                    QFile trunc(m_tempFilePath);
                    if (trunc.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        trunc.close();
                        cleanup = QStringLiteral(" Could not delete the partial file; it was "
                                                 "truncated to 0 bytes so it cannot be resumed from.");
                    } else {
                        cleanup = QStringLiteral(" WARNING: could not delete or truncate the "
                                                 "partial file at %1 — remove it manually before "
                                                 "retrying.").arg(m_tempFilePath);
                        qCritical() << "ModelDownloader: could not delete or truncate poisoned partial"
                                    << m_tempFilePath;
                    }
                }
                emit downloadError(m_currentModelName,
                    QString("Integrity check failed for %1: %2. The downloaded file was "
                            "discarded — it did not match the expected SHA-256, so it was "
                            "either corrupted in transit or is not the published model.%3")
                        .arg(m_currentModelName, why, cleanup));
            }
        }

        // Rename temp file to final destination
        if (integrityOk && QFile::exists(m_currentDestinationPath)) {
            QFile::remove(m_currentDestinationPath);
        }

        if (!integrityOk) {
            // handled above; fall through to the shared cleanup
        } else if (QFile::rename(m_tempFilePath, m_currentDestinationPath)) {
            qDebug() << "ModelDownloader: Download completed:" << m_currentDestinationPath;
            emit downloadCompleted(m_currentModelName, m_currentDestinationPath);
        } else {
            emit downloadError(m_currentModelName, "Failed to rename downloaded file");
        }
    }

    // Cleanup
    if (m_currentReply) {
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

    QString completedModel = m_currentModelName;
    m_isDownloading = false;
    m_isPaused = false;
    m_currentUrl.clear();
    m_currentDestinationPath.clear();
    m_currentModelName.clear();
    m_tempFilePath.clear();
    m_expectedSha256.clear();   // #1029: never let one download's digest leak into the next
    m_resumeOffset = 0;
    m_downloadSpeed = 0.0f;

    emit isDownloadingChanged();
    emit currentModelNameChanged();
    emit downloadSpeedChanged();
}

void ModelDownloader::onDownloadError(QNetworkReply::NetworkError error)
{
    if (error == QNetworkReply::OperationCanceledError && m_isPaused) {
        // This is expected when pausing
        return;
    }

    QString errorString = m_currentReply ? m_currentReply->errorString() : "Unknown error";
    qWarning() << "ModelDownloader: Download error:" << errorString;
    emit downloadError(m_currentModelName, errorString);

    // Don't cancel completely on error - allow resume
    m_speedTimer->stop();

    if (m_outputFile) {
        m_outputFile->flush();
        m_outputFile->close();
        delete m_outputFile;
        m_outputFile = nullptr;
    }

    if (m_currentReply) {
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

    m_isPaused = true;
    m_downloadSpeed = 0.0f;
    emit downloadSpeedChanged();
}

void ModelDownloader::onSpeedTimerTimeout()
{
    qint64 bytesThisSecond = m_bytesReceived - m_lastBytesReceived;
    m_downloadSpeed = static_cast<float>(bytesThisSecond);
    m_lastBytesReceived = m_bytesReceived;
    emit downloadSpeedChanged();
}
