#include "ProjectPackager.h"

#include "DependencyResolver.h"
#include "FeedbackDiagnostics.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

namespace {

QString applicationVersion()
{
    return QString::fromUtf8(QTMESHEDITOR_VERSION);
}

QString sha256File(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd())
        hash.addData(file.read(1024 * 1024));
    return QString::fromLatin1(hash.result().toHex());
}

QString commonRootDir(const QStringList& absolutePaths)
{
    if (absolutePaths.isEmpty())
        return {};

    QString common = QFileInfo(absolutePaths.first()).absolutePath();
    for (const QString& path : absolutePaths) {
        const QString dir = QFileInfo(path).absolutePath();
        while (!dir.startsWith(common) && !common.isEmpty()) {
            const int slash = common.lastIndexOf(QLatin1Char('/'));
            if (slash < 0)
                break;
            common = common.left(slash);
        }
        if (common.isEmpty())
            break;
    }
    return common;
}

QString makeRelativePath(const QString& rootDir, const QString& absolutePath, QSet<QString>& usedNames)
{
    QString relative;
    if (!rootDir.isEmpty() && absolutePath.startsWith(rootDir)) {
        relative = QDir(rootDir).relativeFilePath(absolutePath);
    } else {
        relative = QFileInfo(absolutePath).fileName();
    }

    relative = relative.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (relative.isEmpty())
        relative = QFileInfo(absolutePath).fileName();

    QString candidate = relative;
    int suffix = 1;
    while (usedNames.contains(candidate)) {
        const QFileInfo info(relative);
        candidate = QStringLiteral("%1.%2%3")
                        .arg(info.completeBaseName())
                        .arg(suffix++, 3, 10, QLatin1Char('0'))
                        .arg(info.suffix().isEmpty() ? QString() : QLatin1Char('.') + info.suffix());
    }
    usedNames.insert(candidate);
    return candidate;
}

QStringList classifyAssetTypes(const PackageMetadata& metadata)
{
    QStringList types;
    types << QStringLiteral("mesh");

    bool hasTexture = false;
    bool hasSkeleton = false;
    bool hasAnimation = false;
    int materialCount = 0;
    bool ps1Style = false;

    for (const PackageEntry& entry : metadata.files) {
        if (entry.role == QLatin1String("texture"))
            hasTexture = true;
        if (entry.role == QLatin1String("skeleton"))
            hasSkeleton = true;
        if (entry.role == QLatin1String("animation"))
            hasAnimation = true;
        if (entry.role == QLatin1String("material"))
            ++materialCount;
        if (entry.role.startsWith(QLatin1String("ps1-")))
            ps1Style = true;
    }

    if (hasSkeleton)
        types << QStringLiteral("skinned");
    if (hasAnimation)
        types << QStringLiteral("animated");
    if (materialCount > 1)
        types << QStringLiteral("multi-material");
    if (ps1Style || metadata.sourceFormat == QLatin1String("rsd")
        || metadata.sourceFormat == QLatin1String("tmd")
        || metadata.sourceFormat == QLatin1String("ply")
        || metadata.sourceFormat == QLatin1String("mat"))
        types << QStringLiteral("ps1-style");
    if (metadata.totalSize > 100LL * 1024 * 1024)
        types << QStringLiteral("large");
    if (hasTexture)
        types << QStringLiteral("textured");

    return types;
}

bool containsSensitivePathFragment(const QString& value)
{
    if (value.isEmpty())
        return false;

    static const QRegularExpression unixHome(QStringLiteral("(?:^|/)(?:Users|home)/[^/]+"));
    static const QRegularExpression windowsDrive(QStringLiteral("^[A-Za-z]:[\\\\/]"));
    static const QRegularExpression absoluteUnix(QStringLiteral("^/"));
    if (unixHome.match(value).hasMatch() || windowsDrive.match(value).hasMatch())
        return true;
    if (absoluteUnix.match(value).hasMatch() && value.count(QLatin1Char('/')) > 1)
        return true;
    return false;
}

void lintJsonValue(const QJsonValue& value, bool& bad)
{
    if (value.isString()) {
        if (containsSensitivePathFragment(value.toString()))
            bad = true;
        return;
    }
    if (value.isObject()) {
        for (auto it = value.toObject().begin(); it != value.toObject().end(); ++it)
            lintJsonValue(it.value(), bad);
        return;
    }
    if (value.isArray()) {
        for (const QJsonValue& item : value.toArray())
            lintJsonValue(item, bad);
    }
}

} // namespace

PackageMetadata ProjectPackager::buildManifest(const QString& mainAssetPath,
                                               const QStringList& explicitExtras,
                                               const QString& projectName)
{
    PackageMetadata metadata;
    const QFileInfo mainInfo(mainAssetPath);
    metadata.projectName = projectName.trimmed().isEmpty() ? mainInfo.completeBaseName() : projectName.trimmed();
    metadata.sourceFormat = mainInfo.suffix().toLower();
    metadata.qtMeshEditorVersion = applicationVersion();

    QVector<DependencyEntry> dependencies = DependencyResolver::detect(mainAssetPath);
    QSet<QString> included;
    for (const DependencyEntry& entry : dependencies)
        included.insert(entry.absolutePath);
    for (const QString& extra : explicitExtras) {
        const QString canonical = QFileInfo(extra).absoluteFilePath();
        if (!included.contains(canonical)) {
            DependencyEntry entry;
            entry.absolutePath = canonical;
            entry.role = QStringLiteral("other");
            entry.exists = QFileInfo::exists(canonical);
            entry.checkedByDefault = entry.exists;
            entry.referencedBy = QStringLiteral("manual");
            dependencies.append(entry);
            included.insert(canonical);
        }
    }

    QStringList absolutePaths;
    for (const DependencyEntry& entry : dependencies)
        absolutePaths.append(entry.absolutePath);

    const QString rootDir = commonRootDir(absolutePaths);
    QSet<QString> usedNames;
    QString mainRelative;

    for (const DependencyEntry& entry : dependencies) {
        PackageEntry packageEntry;
        packageEntry.relativePath = makeRelativePath(rootDir, entry.absolutePath, usedNames);
        packageEntry.absolutePath = entry.absolutePath;
        packageEntry.role = entry.role;
        packageEntry.size = entry.exists ? QFileInfo(entry.absolutePath).size() : 0;
        if (entry.exists)
            packageEntry.sha256 = sha256File(entry.absolutePath);
        metadata.files.append(packageEntry);
        metadata.totalSize += packageEntry.size;

        if (entry.role == QLatin1String("main"))
            mainRelative = packageEntry.relativePath;
    }

    if (mainRelative.isEmpty() && !metadata.files.isEmpty())
        mainRelative = metadata.files.first().relativePath;
    metadata.mainFile = mainRelative;
    metadata.detectedAssetTypes = classifyAssetTypes(metadata);
    return metadata;
}

QJsonObject ProjectPackager::toJson(const PackageMetadata& metadata)
{
    QJsonObject root;
    root.insert(QStringLiteral("manifestVersion"), metadata.manifestVersion);
    root.insert(QStringLiteral("projectName"), metadata.projectName);
    root.insert(QStringLiteral("sourceFormat"), metadata.sourceFormat);
    root.insert(QStringLiteral("mainFile"), metadata.mainFile);
    root.insert(QStringLiteral("totalSize"), metadata.totalSize);
    root.insert(QStringLiteral("qtMeshEditorVersion"), metadata.qtMeshEditorVersion);

    QJsonArray assetTypes;
    for (const QString& type : metadata.detectedAssetTypes)
        assetTypes.append(type);
    root.insert(QStringLiteral("detectedAssetTypes"), assetTypes);

    if (!metadata.scanSummary.isEmpty())
        root.insert(QStringLiteral("scanSummary"), metadata.scanSummary);

    QJsonArray files;
    for (const PackageEntry& entry : metadata.files) {
        QJsonObject fileObj;
        fileObj.insert(QStringLiteral("relativePath"), entry.relativePath);
        fileObj.insert(QStringLiteral("size"), entry.size);
        fileObj.insert(QStringLiteral("role"), entry.role);
        if (!entry.sha256.isEmpty())
            fileObj.insert(QStringLiteral("sha256"), entry.sha256);
        files.append(fileObj);
    }
    root.insert(QStringLiteral("files"), files);
    return root;
}

bool ProjectPackager::jsonPassesPathSanitisationLint(const QJsonObject& json)
{
    bool bad = false;
    lintJsonValue(json, bad);
    return !bad;
}
