#ifndef DEPENDENCY_RESOLVER_H
#define DEPENDENCY_RESOLVER_H

#include <QString>
#include <QStringList>
#include <QVector>

struct DependencyEntry {
    QString absolutePath;
    QString role;
    bool checkedByDefault = true;
    bool exists = true;
    QString referencedBy;
};

class DependencyResolver {
public:
    DependencyResolver() = delete;

    /// Walk the main asset and any loaded scene materials; return deduped dependencies.
    static QVector<DependencyEntry> detect(const QString& mainAssetPath);

    /// File-format heuristics only (no Ogre scene walk). Used by unit tests.
    static QVector<DependencyEntry> detectFromFiles(const QString& mainAssetPath);

    /// Optional scene walk for textures/material sidecars when Ogre is initialized.
    static QVector<DependencyEntry> detectFromLoadedScene(const QString& referencedBy);
};

#endif // DEPENDENCY_RESOLVER_H
