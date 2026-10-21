#include "CharacterAssets.h"

#include "../ModelFetch.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace CharacterGen {
namespace CharacterAssets {

namespace {

constexpr const char* kBaseUrlSettingsKey = "ai/characterModelBaseUrl";
constexpr const char* kDefaultBaseUrl = "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/character/";

struct FileSpec { const char* name; const char* label; const char* sha256; };
constexpr FileSpec kFiles[] = {
    {"mhr_character.qmcb", "character body (MHR)", "ab2403eb7b1c756ab38262bd7cd49331921ba1e98ec5cf570cb7f22c0724402f"},
    {"ict_character.qmcb", "character head (ICT-FaceKit)", "f1b74cebf2bc8e21276e7f1ce717b0c52122bb1def9cb214052364952ae657f1"},
    {"LICENSE-MHR.txt", "MHR licence", "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"},
    {"LICENSE-ICT-FaceKit.txt", "ICT-FaceKit licence", "b20a78d8fc51657e14f3e00c8abb538d786ad00aa63210328c22565e583167e1"},
};

}  // namespace

QString dir()
{
    const QString env = qEnvironmentVariable("QTMESH_CHARACTER_DIR");
    if (!env.isEmpty()) return env;
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/ai_models/character");
}

bool present()
{
    return QFileInfo::exists(QDir(dir()).filePath(QStringLiteral("mhr_character.qmcb")))
           && QFileInfo::exists(QDir(dir()).filePath(QStringLiteral("ict_character.qmcb")));
}

bool ensureBlocking(QString* error)
{
    QString base = QSettings().value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
    if (base.isEmpty()) {
        const QByteArray env = qgetenv("QTMESH_CHARACTER_MODEL_BASE_URL");
        base = env.isEmpty() ? QString::fromLatin1(kDefaultBaseUrl) : QString::fromUtf8(env);
    }
    const bool defaultHosting = base == QString::fromLatin1(kDefaultBaseUrl);
    if (!base.endsWith(QLatin1Char('/'))) base += QLatin1Char('/');
    const bool noDownload = !qEnvironmentVariableIsEmpty("QTMESH_CHARACTER_NO_DOWNLOAD");
    // a user-chosen directory (QTMESH_CHARACTER_DIR) holds the user's own build: it is used as it is
    const bool ownDir = !qEnvironmentVariable("QTMESH_CHARACTER_DIR").isEmpty();

    QDir().mkpath(dir());
    for (const FileSpec& f : kFiles) {
        const QString dest = QDir(dir()).filePath(QString::fromLatin1(f.name));
        const bool licence = QString::fromLatin1(f.name).startsWith(QLatin1String("LICENSE"));
        if (QFileInfo::exists(dest) && (ownDir || !defaultHosting)) continue;
        if (noDownload && !QFileInfo::exists(dest)) {
            if (licence) continue;   // the licence texts are a courtesy copy, not needed to run
            if (error) *error = QStringLiteral("%1 is missing and downloads are disabled by QTMESH_CHARACTER_NO_DOWNLOAD")
                                    .arg(QString::fromLatin1(f.name));
            return false;
        }
        ModelFetch::Request req;
        req.url = noDownload ? QString() : base + QString::fromLatin1(f.name);
        req.destination = dest;
        req.label = QString::fromLatin1(f.label);
        req.timeoutMs = 600000;
        if (defaultHosting) req.expectedSha256 = QString::fromLatin1(f.sha256);
        const ModelFetch::Outcome fo = ModelFetch::ensureBlocking(req);
        if (!fo.ok) {
            if (error) *error = fo.error;
            return false;
        }
    }
    return true;
}

}  // namespace CharacterAssets
}  // namespace CharacterGen
