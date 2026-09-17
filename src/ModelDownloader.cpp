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

QString ModelDownloader::sha256HexOfFile(const QString &path, QString *error)
{
    return ::sha256HexOfFile(path, error);
}

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
    m_expectedTotalBytes = -1;
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
    m_resumeUnverified = m_resumeOffset > 0;   // #1036: prove the Range was honoured

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
    m_resumeUnverified = true;   // #1036

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
    m_expectedTotalBytes = -1;
    m_resumeUnverified = false;

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

bool ModelDownloader::parseContentRange(const QByteArray& header,
                                        qint64& first, qint64& last, qint64& total)
{
    first = -1;
    last = -1;
    total = -1;
    const QByteArray cr = header.trimmed();
    // RFC 9110 §14.1: the range unit is CASE-INSENSITIVE — "Bytes 9-12/13" is
    // a valid honoured resume (review on #1039).
    const qsizetype sp = cr.indexOf(' ');
    if (sp <= 0 || cr.left(sp).compare(QByteArrayLiteral("bytes"), Qt::CaseInsensitive) != 0)
        return false;
    const QByteArray range = cr.mid(sp + 1).trimmed();
    const qsizetype dash = range.indexOf('-');
    const qsizetype slash = range.indexOf('/');
    if (dash <= 0 || slash <= dash)
        return false;
    bool okFirst = false;
    bool okLast = false;
    bool okTotal = false;
    const qint64 f = range.left(dash).trimmed().toLongLong(&okFirst);
    const qint64 l = range.mid(dash + 1, slash - dash - 1).trimmed().toLongLong(&okLast);
    const qint64 t = range.mid(slash + 1).trimmed().toLongLong(&okTotal);   // "*" => unknown
    if (okFirst) first = f;
    if (okLast) last = l;
    if (okTotal) total = t;
    return okFirst && okLast;
}

bool ModelDownloader::verifyResumeResponse()
{
    // #1036: a resume sends `Range: bytes=N-`, but nothing guaranteed the
    // server HONOURED it. One that ignores Range (file:// always does; any
    // proxy/CDN that strips the header will) answers 200 with the WHOLE body,
    // and appending that after the stale .part produced a corrupt model —
    // reproduced: a 29-byte stale prefix yielded a 208,044,845-byte file that
    // still LOADED and ran (ORT parsed the garbage as an unknown protobuf
    // field). A load success proves nothing; only the response can tell us
    // which bytes these are. Checked ONCE, on the first readyRead, because the
    // status/headers are available then and re-checking per chunk is waste.
    m_resumeUnverified = false;
    const int status = m_currentReply->attribute(
        QNetworkRequest::HttpStatusCodeAttribute).toInt();
    qint64 first = -1;
    qint64 last = -1;
    qint64 total = -1;
    const bool parsed = status == 206
        && parseContentRange(m_currentReply->rawHeader("Content-Range"), first, last, total);
    // Honoured = starts where we asked AND reaches the resource end (review:
    // `bytes 9-10/13` starts right but leaves 11-12 missing — appending it and
    // renaming on finish would promote an incomplete file). An unknown total
    // ("*") cannot be checked here; the size check at finish stays unknown too.
    if (parsed && first == m_resumeOffset && (total < 0 || last == total - 1)) {
        m_expectedTotalBytes = total;
        return true;   // honoured: keep appending
    }

    // A 206 whose window is the WHOLE resource (0..total-1) is a full body
    // wearing a partial status — safe to truncate and take. Any OTHER 206
    // window is a genuinely partial body we did not ask for: writing it from
    // byte 0 would yield an INCOMPLETE file, so that case must fail, not
    // "recover".
    if (const bool fullBodyDisguised = parsed && first == 0 && total > 0 && last == total - 1;
        status == 206 && !fullBodyDisguised) {
        qCritical() << "ModelDownloader: unusable 206 for" << m_currentModelName
                    << "— asked from byte" << m_resumeOffset << "got Content-Range first="
                    << first << "last=" << last << "total=" << total << "— aborting";
        discardPartialAndFail(
            QString("The server answered the resume request with a partial range that "
                    "does not match (asked from byte %1, got %2-%3/%4); the partial file "
                    "was discarded — retry to download from the start.")
                .arg(m_resumeOffset).arg(first).arg(last).arg(total));
        return false;
    }

    // Full body (200, or a 206 covering everything): discard the stale prefix
    // and restart from byte 0 — never append.
    qWarning() << "ModelDownloader: server ignored Range for" << m_currentModelName
               << "(status" << status << ") — discarding" << m_resumeOffset
               << "stale bytes and restarting from 0";
    // Reopen truncated: the Append-mode handle would keep writing after the
    // stale prefix. A failed reopen is fatal for this download — appending
    // would be worse than stopping.
    m_outputFile->close();
    if (!m_outputFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit downloadError(m_currentModelName,
            QString("Server ignored the resume request and the partial file "
                    "could not be reset: %1").arg(m_tempFilePath));
        m_currentReply->abort();
        return false;
    }
    // The whole body is arriving; progress must not add the offset.
    m_resumeOffset = 0;
    m_bytesReceived = 0;
    // Its size is what the response says it is (a 206 covering 0..total-1
    // carries the total; a 200 carries Content-Length), -1 when neither.
    if (parsed && total > 0) m_expectedTotalBytes = total;
    else {
        const QVariant cl = m_currentReply->header(QNetworkRequest::ContentLengthHeader);
        m_expectedTotalBytes = cl.isValid() && cl.toLongLong() > 0 ? cl.toLongLong() : -1;
    }
    return true;
}

void ModelDownloader::discardPartialAndFail(const QString& message)
{
    const QString name = m_currentModelName;
    m_speedTimer->stop();
    if (m_outputFile) {
        m_outputFile->close();
        delete m_outputFile;
        m_outputFile = nullptr;
    }
    // Remove the poisoned partial; if removal fails, truncate it so a later
    // attempt starts from byte 0 instead of resuming from garbage (the same
    // fallback the SHA-256 path uses). Then reset the resume bookkeeping —
    // review: resumeDownload() derives its offset from m_bytesReceived, and a
    // stale value would re-request the old range against a fresh, empty file.
    if (!m_tempFilePath.isEmpty() && !QFile::remove(m_tempFilePath)) {
        QFile trunc(m_tempFilePath);
        if (trunc.open(QIODevice::WriteOnly | QIODevice::Truncate)) trunc.close();
        else qCritical() << "ModelDownloader: could not delete or truncate" << m_tempFilePath;
    }
    m_bytesReceived = 0;
    m_resumeOffset = 0;
    m_expectedTotalBytes = -1;
    m_resumeUnverified = false;
    if (QNetworkReply* reply = m_currentReply) {
        // abort() delivers errorOccurred + finished synchronously for a
        // same-thread reply; the flag makes both handlers step aside so this
        // remains the only cleanup and the only downloadError. Detach the
        // pointer FIRST so a handler that does run can never leave us holding
        // a reply it already released.
        m_currentReply = nullptr;
        m_abortingInternally = true;
        reply->abort();
        m_abortingInternally = false;
        reply->deleteLater();
    }
    m_isDownloading = false;
    m_isPaused = false;
    m_currentUrl.clear();
    m_currentDestinationPath.clear();
    m_currentModelName.clear();
    m_tempFilePath.clear();
    m_expectedSha256.clear();
    m_downloadSpeed = 0.0f;
    emit isDownloadingChanged();
    emit currentModelNameChanged();
    emit downloadSpeedChanged();
    emit downloadError(name, message);
}

void ModelDownloader::onReadyRead()
{
    if (!m_outputFile || !m_currentReply) return;
    if (m_resumeUnverified && !verifyResumeResponse())
        return;   // aborted — an unusable partial window
    QByteArray data = m_currentReply->readAll();
    m_outputFile->write(data);
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
    if (m_abortingInternally) return;   // discardPartialAndFail() owns this cleanup
    m_speedTimer->stop();

    if (m_currentReply && m_currentReply->error() == QNetworkReply::NoError) {
        // Flush and close the file
        if (m_outputFile) {
            m_outputFile->flush();
            m_outputFile->close();
            delete m_outputFile;
            m_outputFile = nullptr;
        }

        // #1036 review, completeness before promotion. Two ways a "successful"
        // finish can still be an incomplete file:
        // (a) the resume reply finished WITHOUT ever delivering data — the
        //     verification in onReadyRead never ran, so the stale .part is
        //     unvetted (a 200/206 with an empty body, e.g. a server that treats
        //     an out-of-range Range as "nothing to send");
        // (b) the body was shorter than the size the response committed to.
        // Neither is "NoError" to QNAM, and without a digest nothing else would
        // catch it — the reproduced #1036 file LOADED. Keep the .part: it is a
        // valid prefix, so the next startDownload resumes it (and re-verifies).
        QString incomplete;
        const qint64 actual = QFileInfo(m_tempFilePath).size();
        if (m_resumeUnverified) {
            incomplete = QStringLiteral("the server answered the resume request without any "
                                        "data, so the partial file could not be verified");
        } else {
            qint64 expected = m_expectedTotalBytes;
            if (expected < 0 && m_resumeOffset == 0) {
                const QVariant cl = m_currentReply->header(QNetworkRequest::ContentLengthHeader);
                if (cl.isValid() && cl.toLongLong() > 0) expected = cl.toLongLong();
            }
            if (expected >= 0 && actual != expected)
                incomplete = QStringLiteral("received %1 of %2 bytes").arg(actual).arg(expected);
            else if (expected < 0 && m_expectedSha256.isEmpty())
                qWarning() << "ModelDownloader:" << m_currentModelName
                           << "— the server declared no size and no digest is configured; "
                              "completeness of the" << actual << "byte file cannot be verified";
        }
        if (!incomplete.isEmpty()) {
            qCritical() << "ModelDownloader: incomplete download for" << m_currentModelName
                        << "—" << incomplete << "— not promoting the partial file";
            emit downloadError(m_currentModelName,
                QString("Download of %1 is incomplete: %2. The partial file was kept and "
                        "will be resumed on the next attempt.").arg(m_currentModelName, incomplete));
            m_resumeUnverified = false;
            // shared cleanup below; the .part stays in place for a resume
            if (m_currentReply) { m_currentReply->deleteLater(); m_currentReply = nullptr; }
            m_isDownloading = false;
            m_isPaused = false;
            m_currentUrl.clear();
            m_currentDestinationPath.clear();
            m_currentModelName.clear();
            m_tempFilePath.clear();
            m_expectedSha256.clear();
            m_resumeOffset = 0;
            m_expectedTotalBytes = -1;
            m_downloadSpeed = 0.0f;
            emit isDownloadingChanged();
            emit currentModelNameChanged();
            emit downloadSpeedChanged();
            return;
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
    m_expectedTotalBytes = -1;
    m_downloadSpeed = 0.0f;

    emit isDownloadingChanged();
    emit currentModelNameChanged();
    emit downloadSpeedChanged();
}

void ModelDownloader::onDownloadError(QNetworkReply::NetworkError error)
{
    if (error == QNetworkReply::OperationCanceledError && (m_isPaused || m_abortingInternally)) {
        // Expected: pausing, or our own abort inside discardPartialAndFail()
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
