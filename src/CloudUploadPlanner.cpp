#include "CloudUploadPlanner.h"

#include "DependencyResolver.h"
#include "ScanEngine.h"

#include <QFileInfo>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSet>
#include <QDir>

namespace {

bool hasExtension(const QString& ext, const QSet<QString>& extensions)
{
    return extensions.contains(ext.toLower());
}

} // namespace

QString CloudUploadPlanner::makeProjectSlug(const QString& name, const QString& fallback)
{
    auto normalize = [](const QString& value) {
        QString slug = value.trimmed().toLower();
        slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
        slug.replace(QRegularExpression(QStringLiteral("^-+|-+$")), QString());
        slug.replace(QRegularExpression(QStringLiteral("-{2,}")), QStringLiteral("-"));
        return slug;
    };

    QString slug = normalize(name);
    if (slug.isEmpty())
        slug = normalize(fallback);
    if (slug.isEmpty())
        slug = QStringLiteral("qtmesh-project");
    if (slug.size() > 64) {
        slug.truncate(64);
        slug.replace(QRegularExpression(QStringLiteral("-+$")), QString());
    }
    return slug;
}

QString CloudUploadPlanner::inferAssetRole(const QString& fileName)
{
    const QString ext = QFileInfo(fileName).suffix().toLower();
    if (hasExtension(ext, {QStringLiteral("mesh"), QStringLiteral("obj"), QStringLiteral("fbx"),
                           QStringLiteral("gltf"), QStringLiteral("glb"), QStringLiteral("dae"),
                           QStringLiteral("collada"), QStringLiteral("stl"), QStringLiteral("tmd")}))
        return QStringLiteral("model");
    if (hasExtension(ext, {QStringLiteral("skeleton"), QStringLiteral("skel")}))
        return QStringLiteral("skeleton");
    if (hasExtension(ext, {QStringLiteral("anim"), QStringLiteral("animation")}))
        return QStringLiteral("animation");
    if (hasExtension(ext, {QStringLiteral("material"), QStringLiteral("mat"), QStringLiteral("mtl")}))
        return QStringLiteral("material");
    if (hasExtension(ext, {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                           QStringLiteral("tga"), QStringLiteral("bmp"), QStringLiteral("dds"),
                           QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("tim")}))
        return QStringLiteral("texture");
    if (hasExtension(ext, {QStringLiteral("json"), QStringLiteral("yml"), QStringLiteral("yaml"),
                           QStringLiteral("xml")}))
        return QStringLiteral("metadata");
    if (hasExtension(ext, {QStringLiteral("rsd"), QStringLiteral("ply")}))
        return QStringLiteral("sidecar");
    return QStringLiteral("file");
}

QStringList CloudUploadPlanner::selectedPathsForUpload(const QString& mainAssetPath,
                                                     const QStringList& includeGlobs,
                                                     const QStringList& excludeGlobs)
{
    const QFileInfo mainInfo(mainAssetPath);
    const QString mainCanonical = mainInfo.absoluteFilePath();
    const QString rootDir = mainInfo.absolutePath();

    QSet<QString> selected;
    selected.insert(mainCanonical);

    for (const DependencyEntry& entry : DependencyResolver::detect(mainAssetPath)) {
        if (!entry.exists || !entry.checkedByDefault)
            continue;
        const QString absolute = QFileInfo(entry.absolutePath).absoluteFilePath();
        if (!absolute.isEmpty())
            selected.insert(absolute);
    }

    if (includeGlobs.isEmpty() && excludeGlobs.isEmpty())
        return selected.values();

    QSet<QString> filtered;
    filtered.insert(mainCanonical);
    for (const QString& absolute : selected) {
        if (absolute == mainCanonical)
            continue;
        const QString rel = QDir(rootDir).relativeFilePath(absolute);
        bool keep = includeGlobs.isEmpty();
        for (const QString& pattern : includeGlobs) {
            if (ScanEngine::matchesGlob(rel, pattern)) {
                keep = true;
                break;
            }
        }
        if (!keep)
            continue;
        for (const QString& pattern : excludeGlobs) {
            if (ScanEngine::matchesGlob(rel, pattern)) {
                keep = false;
                break;
            }
        }
        if (keep)
            filtered.insert(absolute);
    }
    return filtered.values();
}

QList<QtMeshCloudClient::AssetFileDescriptor>
CloudUploadPlanner::buildAssetFileDescriptors(const QStringList& paths)
{
    QList<QtMeshCloudClient::AssetFileDescriptor> descriptors;
    QMimeDatabase mimeDatabase;
    for (const QString& path : paths) {
        const QFileInfo info(path);
        QtMeshCloudClient::AssetFileDescriptor descriptor;
        descriptor.path = path;
        descriptor.uploadName = info.fileName();
        descriptor.role = inferAssetRole(info.fileName());
        descriptor.mimeType = mimeDatabase.mimeTypeForFile(info, QMimeDatabase::MatchExtension).name();
        descriptor.sizeBytes = info.size();
        descriptors.append(descriptor);
    }
    return descriptors;
}
