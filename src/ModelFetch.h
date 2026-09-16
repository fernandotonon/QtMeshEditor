#ifndef MODEL_FETCH_H
#define MODEL_FETCH_H

#include <QString>

// #1037: the ONE blocking "make sure this model file is on disk" primitive.
//
// Twenty consumers each hand-rolled the same ~25 lines around
// ModelDownloader: nested QEventLoop, completion/error connects filtered by
// label, a QTimer timeout, cancel-on-timeout. Copied twenty times, the copies
// drifted in two ways that matter:
//
//  1. Eighteen of them DISCARD the downloader's error text (the lambda takes
//     `const QString&` unnamed), so the precise reason — "refusing http://",
//     "SHA-256 mismatch, file discarded", "already in progress" — never
//     reaches the user, who is told "unavailable (offline?)" and sent
//     debugging their network.
//  2. Only ONE of them (TextureInpaint) guards the synchronous-rejection
//     race: ModelDownloader::startDownload emits downloadError SYNCHRONOUSLY
//     when another download is active, so a handler's loop.quit() runs
//     BEFORE loop.exec() and is lost — the caller then hangs for its full
//     timeout. The other nineteen have that bug latent.
//
// Consumers keep what genuinely varies — base-URL resolution (env var,
// QSettings key, default), the *_NO_DOWNLOAD guard, the timeout — and pass
// the resolved URL here. This owns only the blocking wait, and returns the
// downloader's own words.
//
// Blocking, MAIN THREAD ONLY (nested QEventLoop), like every ensureModelBlocking.
namespace ModelFetch {

struct Request {
    QString url;             ///< fully resolved download URL (caller applies base/env/QSettings)
    QString destination;     ///< final on-disk path
    QString label;           ///< ModelDownloader model name — the signal filter
    int timeoutMs = 300000;  ///< 5 min default; large models pass more
    QString expectedSha256;  ///< optional (#1029); empty = no integrity check
};

struct Outcome {
    bool ok = false;
    bool timedOut = false;
    QString path;            ///< == Request::destination when ok
    /// The downloader's own reason on failure ("Refusing to download …",
    /// "Integrity check failed …", "A download is already in progress"), or
    /// this module's ("download timed out after N s"). Empty only when ok.
    QString error;
};

/// If `destination` already exists: ok immediately, nothing touched.
/// Otherwise download it and block until completed / failed / timed out.
Outcome ensureBlocking(const Request& req);

} // namespace ModelFetch

#endif // MODEL_FETCH_H
