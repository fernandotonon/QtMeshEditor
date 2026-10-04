#include "ModelFetch.h"

#include "ModelDownloader.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QObject>
#include <QTimer>

namespace {

// Hashing a 1.2 GB decoder on every ensureBlocking() call would cost seconds
// per rig; remember the digest per path with the size+mtime it was computed
// for, so an unchanged file is verified once per process and a re-downloaded
// one (new mtime) is re-hashed.
struct VerifiedDigest { qint64 size = -1; QDateTime mtime; QString digest; };
QHash<QString, VerifiedDigest>& verifiedDigests()
{
    static QHash<QString, VerifiedDigest> cache;
    return cache;
}

bool existingFileMatchesDigest(const QString& path, const QString& expected, QString* why)
{
    const QFileInfo fi(path);
    auto& cache = verifiedDigests();
    QString digest;
    const auto it = cache.constFind(path);
    if (it != cache.constEnd() && it->size == fi.size() && it->mtime == fi.lastModified()) {
        digest = it->digest;
    } else {
        QString err;
        digest = ModelDownloader::sha256HexOfFile(path, &err);
        if (digest.isEmpty()) { if (why) *why = err; return false; }
        cache.insert(path, {fi.size(), fi.lastModified(), digest});
    }
    if (digest.compare(expected.trimmed(), Qt::CaseInsensitive) == 0) return true;
    if (why) *why = QStringLiteral("SHA-256 mismatch: published %1…, on disk %2…")
                        .arg(expected.trimmed().left(12), digest.left(12));
    return false;
}

} // namespace

namespace ModelFetch {

Outcome ensureBlocking(const Request& req)
{
    Outcome out;
    out.path = req.destination;

    if (req.destination.isEmpty()) {
        out.error = QStringLiteral("no destination path for the model");
        return out;
    }
    if (QFileInfo::exists(req.destination)) {
        if (req.expectedSha256.trimmed().isEmpty()) {
            out.ok = true;
            return out;
        }
        QString why;
        if (existingFileMatchesDigest(req.destination, req.expectedSha256, &why)) {
            out.ok = true;
            return out;
        }
        // #1025: the file on disk is NOT the published model. Never hand it to
        // a consumer — the UniRig encoder loaded fine and produced NaN for
        // months. Delete it (and any stale .part that would be resumed from)
        // and fall through to a fresh, digest-verified download.
        qCritical().noquote() << "ModelFetch:" << req.label << "on disk is corrupt —" << why
                              << "— deleting" << req.destination << "and fetching it again";
        if (!QFile::remove(req.destination)) {
            out.error = QStringLiteral("%1 on disk is corrupt (%2) and could not be deleted: %3")
                            .arg(req.label, why, req.destination);
            return out;
        }
        QFile::remove(req.destination + QStringLiteral(".part"));
        out.replacedCorrupt = true;
        if (req.url.isEmpty()) {
            out.error = QStringLiteral("%1 on disk was corrupt (%2) and was deleted, but no "
                                       "download URL is configured to fetch it again")
                            .arg(req.label, why);
            return out;
        }
    }
    if (req.url.isEmpty()) {
        out.error = QStringLiteral("no download URL configured");
        return out;
    }

    auto* dl = ModelDownloader::instance();
    if (!dl) {
        out.error = QStringLiteral("model downloader unavailable");
        return out;
    }

    QDir().mkpath(QFileInfo(req.destination).absolutePath());

    QEventLoop loop;
    bool ok = false;
    // `settled` guards the synchronous-rejection race (see the header): if
    // startDownload emits before we reach exec(), record it and skip exec().
    bool settled = false;

    auto onDone = QObject::connect(dl, &ModelDownloader::downloadCompleted, &loop,
        [&](const QString& name, const QString&) {
            if (name != req.label) return;
            ok = true; settled = true; loop.quit();
        });
    auto onErr = QObject::connect(dl, &ModelDownloader::downloadError, &loop,
        [&](const QString& name, const QString& err) {
            if (name != req.label) return;
            ok = false; settled = true;
            out.error = err;          // the downloader's own words — never discarded
            loop.quit();
        });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        out.timedOut = true; settled = true; loop.quit();
    });
    const int effectiveTimeout = req.timeoutMs > 0 ? req.timeoutMs : 300000;
    timeout.start(effectiveTimeout);

    dl->startDownload(req.url, req.destination, req.label, req.expectedSha256);
    if (!settled)
        loop.exec();

    QObject::disconnect(onDone);
    QObject::disconnect(onErr);

    if (out.timedOut) {
        // Only OUR timeout cancels: a synchronous rejection means someone
        // else's download owns the downloader.
        dl->cancelDownload();
        out.error = QStringLiteral("download timed out after %1 s")
                        .arg(effectiveTimeout / 1000);
    }

    out.ok = ok && !out.timedOut && QFileInfo::exists(req.destination);
    if (!out.ok && out.error.isEmpty())
        out.error = QStringLiteral("download did not complete");
    if (out.ok) out.error.clear();
    return out;
}

} // namespace ModelFetch
