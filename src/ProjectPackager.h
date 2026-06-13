#ifndef PROJECT_PACKAGER_H
#define PROJECT_PACKAGER_H

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct PackageEntry {
    QString relativePath;
    QString absolutePath;
    qint64 size = 0;
    QString sha256;
    QString role;
};

struct PackageMetadata {
    QString projectName;
    QString sourceFormat;
    QString mainFile;
    qint64 totalSize = 0;
    QStringList detectedAssetTypes;
    QString qtMeshEditorVersion;
    QJsonObject scanSummary;
    QVector<PackageEntry> files;
    int manifestVersion = 1;
};

class ProjectPackager {
public:
    ProjectPackager() = delete;

    static PackageMetadata buildManifest(const QString& mainAssetPath,
                                         const QStringList& explicitExtras,
                                         const QString& projectName);

    static QJsonObject toJson(const PackageMetadata& metadata);

    /// Returns true when no absolute paths, usernames, or drive letters leak into JSON.
    static bool jsonPassesPathSanitisationLint(const QJsonObject& json);
};

#endif // PROJECT_PACKAGER_H
