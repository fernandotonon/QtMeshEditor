#ifndef MESHLODCONTROLLER_H
#define MESHLODCONTROLLER_H

#include <QObject>
#include <QVariantList>
#include <QQmlEngine>
#include <memory>

namespace Ogre { class MeshLodGenerator; }

class MeshLodController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(int currentLodLevels READ currentLodLevels NOTIFY lodChanged)

public:
    static MeshLodController* instance();
    static MeshLodController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasSelection() const;
    int currentLodLevels() const;
    // Returns [{level, triangles, label}] for LOD 0 (base) through N
    Q_INVOKABLE QVariantList lodLevelInfo() const;
    // Force viewport to render a specific LOD index (-1 = restore normal)
    Q_INVOKABLE void previewLod(int lodIndex);

    // LOD generator backend. Exposed to QML as a string ("meshopt" |
    // "ogre") so the Inspector dropdown can flip without C++ casts.
    enum class Algorithm {
        Meshopt,   // meshoptimizer's `simplify` + vertex-cache reorder. Default.
        Ogre,      // Ogre's built-in `MeshLodGenerator`. Legacy path.
    };

    // count: number of extra LOD levels (1-4)
    // reductions: list of floats 0.0-1.0 (proportion of vertices to remove per level)
    // algo: which backend to use. Defaults to Meshopt (issue #398).
    Q_INVOKABLE void generateLods(int count, QVariantList reductions);
    // QML-facing backend-selector variant. `algo` is "meshopt" or
    // "ogre"; anything else falls back to meshopt with a warning.
    Q_INVOKABLE void generateLodsWithAlgo(int count, QVariantList reductions,
                                          const QString& algo);
    void generateLods(int count, const QVariantList& reductions, Algorithm algo);
    Q_INVOKABLE void generateAutoLods();
    Q_INVOKABLE void removeLods();
    // Called from QML — emits exportLodsRequested so MainWindow can open the
    // directory picker on the correct parent widget (reliable on macOS).
    Q_INVOKABLE void exportLods(const QString& format);
    // Called by MainWindow once the user has chosen a directory.
    void doExportLods(const QString& format, const QString& directory);

signals:
    void selectionChanged();
    void lodChanged();
    void generationSucceeded(int levels);
    void exportSucceeded(int count, const QString& directory);
    void exportLodsRequested(const QString& format);
    void error(const QString& message);

private:
    MeshLodController();
    ~MeshLodController() override;

    static MeshLodController* m_pSingleton;
    std::unique_ptr<Ogre::MeshLodGenerator> m_generator;
};

#endif // MESHLODCONTROLLER_H
