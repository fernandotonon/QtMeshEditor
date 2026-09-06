#include "AppStorage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSettings>
#include <QStandardPaths>

#include <QtGlobal>

namespace {

QStringList heavyNames()
{
    // Keep in sync with AppStorage::heavySubdirNames().
    return {
        QStringLiteral("ai_models"),
        QStringLiteral("sd_models"),
        QStringLiteral("models"),
        QStringLiteral("hdri"),
        QStringLiteral("hdr_cache"),
        QStringLiteral("generated_textures"),
        QStringLiteral("generated_sources"),
        QStringLiteral("texture_previews"),
        QStringLiteral("trellis2"), // Python sidecar env (large)
        QStringLiteral("gamification"),
    };
}

/// Move src → dst when dst is missing/empty. Never recursive-copy (too big).
bool moveDirIfNeeded(const QString& src, const QString& dst)
{
    QDir srcDir(src);
    if (!srcDir.exists())
        return false;
    QDir dstDir(dst);
    if (dstDir.exists()) {
        // Destination already populated — leave src for the user to delete
        // after verifying; do not merge (would risk a second multi-GB copy).
        const QStringList entries =
            dstDir.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
        if (!entries.isEmpty())
            return false;
        // Empty placeholder — remove and rename over it.
        dstDir.removeRecursively();
    }
    QDir().mkpath(QFileInfo(dst).absolutePath());
    if (QFile::rename(src, dst))
        return true;
    // Cross-device rename can fail; refuse to copy multi-GB payloads.
    qWarning("AppStorage: could not rename %s → %s (leaving in place)",
             qUtf8Printable(src), qUtf8Printable(dst));
    return false;
}

void migrateFromRoot(const QString& fromRoot, const QString& toRoot)
{
    if (fromRoot.isEmpty() || toRoot.isEmpty() || fromRoot == toRoot)
        return;
    for (const QString& name : heavyNames()) {
        moveDirIfNeeded(QDir(fromRoot).filePath(name),
                        QDir(toRoot).filePath(name));
    }
}

bool heavyDirPresent(const QString& root)
{
    for (const QString& name : heavyNames()) {
        if (QDir(QDir(root).filePath(name)).exists())
            return true;
        if (QDir(QDir(root).filePath(QStringLiteral("QtMeshEditor/%1").arg(name)))
                .exists())
            return true;
    }
    return false;
}

/// True for historical default model dirs under Snap AppData
/// (…/QtMeshEditor/QtMeshEditor/{models,sd_models} or one QtMeshEditor segment).
bool isLegacyDefaultModelsPath(const QString& path, const QString& leaf)
{
    const QString cleaned = QDir::cleanPath(path);
    if (QFileInfo(cleaned).fileName() != leaf)
        return false;
    return cleaned.endsWith(QStringLiteral("/QtMeshEditor/QtMeshEditor/") + leaf)
        || cleaned.endsWith(QStringLiteral("/QtMeshEditor/") + leaf);
}

/// Rewrite LLM/SD modelsDirectory when they still point at revision-scoped
/// defaults after the on-disk rename into $SNAP_USER_COMMON.
void retargetPersistedModelDirectories()
{
    const QString common = qEnvironmentVariable("SNAP_USER_COMMON");
    const QString snapUserData = qEnvironmentVariable("SNAP_USER_DATA");
    if (common.isEmpty() || snapUserData.isEmpty())
        return;

    const QString snapApp = QDir::cleanPath(QFileInfo(snapUserData).absolutePath());
    const QString commonClean = QDir::cleanPath(common);

    auto retarget = [&](const QString& group, const QString& key, const QString& leaf,
                        const QString& newPath) {
        QSettings settings;
        const QString fullKey = group + QLatin1Char('/') + key;
        const QString saved = settings.value(fullKey).toString();
        if (saved.isEmpty())
            return;
        const QString cleaned = QDir::cleanPath(saved);
        // Already under common (default or custom) — leave alone.
        if (cleaned.startsWith(commonClean))
            return;
        // Explicit custom path outside the snap app tree — leave alone.
        if (!cleaned.startsWith(snapApp))
            return;
        if (!isLegacyDefaultModelsPath(cleaned, leaf))
            return;
        settings.setValue(fullKey, newPath);
        settings.sync();
    };

    retarget(QStringLiteral("LLM"), QStringLiteral("modelsDirectory"),
             QStringLiteral("models"), AppStorage::llmModelsRoot());
    retarget(QStringLiteral("StableDiffusion"), QStringLiteral("modelsDirectory"),
             QStringLiteral("sd_models"), AppStorage::sdModelsRoot());
}

} // namespace

namespace AppStorage {

bool isSnap()
{
    return !qEnvironmentVariableIsEmpty("SNAP")
        || !qEnvironmentVariableIsEmpty("SNAP_USER_COMMON");
}

QString revisionScopedRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString persistentRoot()
{
    const QString common = qEnvironmentVariable("SNAP_USER_COMMON");
    if (!common.isEmpty()) {
        // Stable across refreshes. Flat name — do NOT nest org/app again
        // (AppDataLocation already does QtMeshEditor/QtMeshEditor).
        return QDir(common).filePath(QStringLiteral("QtMeshEditor"));
    }
    return revisionScopedRoot();
}

QString aiModelsRoot()
{
    return QDir(persistentRoot()).filePath(QStringLiteral("ai_models"));
}

QString sdModelsRoot()
{
    return QDir(persistentRoot()).filePath(QStringLiteral("sd_models"));
}

QString llmModelsRoot()
{
    return QDir(persistentRoot()).filePath(QStringLiteral("models"));
}

QString hdriRoot()
{
    return QDir(persistentRoot()).filePath(QStringLiteral("hdri"));
}

QString hdrCacheRoot()
{
    return QDir(persistentRoot()).filePath(QStringLiteral("hdr_cache"));
}

QStringList heavySubdirNames()
{
    return heavyNames();
}

void migrateHeavyDataFromRevisionScopedStorage()
{
    const QString persistent = persistentRoot();
    const QString revision = revisionScopedRoot();
    if (persistent.isEmpty() || persistent == revision)
        return;

    const QString markerPath =
        QDir(persistent).filePath(QStringLiteral(".snap-persistent-root"));
    const bool markerExists = QFileInfo::exists(markerPath);

    // After the first successful migration, skip the multi-revision scan
    // unless the current revision still has heavy dirs (e.g. a refresh that
    // re-copied leftovers before this build).
    if (markerExists && !heavyDirPresent(revision)) {
        retargetPersistedModelDirectories();
        return;
    }

    QDir().mkpath(persistent);

    auto scoop = [&](const QString& fromRoot) {
        migrateFromRoot(fromRoot, persistent);
        migrateFromRoot(QDir(fromRoot).filePath(QStringLiteral("QtMeshEditor")),
                        persistent);
    };

    // Primary: current AppDataLocation heavy dirs (+ accidental nested layout).
    scoop(revision);

    // Also scoop older snap revisions still on disk. Snap keeps
    // ~/snap/<app>/<N>/ until forget; each may still hold a full ai_models
    // copy from before this migration. Prefer rename into common when the
    // destination is empty; skip when common already has data.
    const QString snapUserData = qEnvironmentVariable("SNAP_USER_DATA");
    if (!snapUserData.isEmpty()) {
        const QDir snapAppDir(QFileInfo(snapUserData).absolutePath());
        const QStringList revs =
            snapAppDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& rev : revs) {
            if (rev == QLatin1String("common") || rev == QLatin1String("current"))
                continue;
            // Match Qt's AppDataLocation under each revision:
            //   <rev>/.local/share/<Org>/<App>/
            const QString candidate = QDir(snapAppDir.filePath(rev))
                .filePath(QStringLiteral(".local/share/QtMeshEditor/QtMeshEditor"));
            if (QDir(candidate).exists())
                scoop(candidate);
        }
    }

    // Marker so support logs can confirm the durable root / skip rescans.
    if (!markerExists) {
        QFile marker(markerPath);
        if (marker.open(QIODevice::WriteOnly | QIODevice::Text)) {
            marker.write("QtMeshEditor heavy data root (SNAP_USER_COMMON)\n");
            marker.close();
        }
    }

    retargetPersistedModelDirectories();
}

} // namespace AppStorage
