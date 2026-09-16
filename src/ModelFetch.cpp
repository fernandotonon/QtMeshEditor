#include "ModelFetch.h"

#include "ModelDownloader.h"

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QObject>
#include <QTimer>

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
        out.ok = true;
        return out;
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
