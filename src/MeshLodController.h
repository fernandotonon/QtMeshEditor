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

    // count: number of extra LOD levels (1-4)
    // reductions: list of floats 0.0-1.0 (proportion of vertices to remove per level)
    Q_INVOKABLE void generateLods(int count, QVariantList reductions);
    Q_INVOKABLE void generateAutoLods();
    Q_INVOKABLE void removeLods();
    // Exports each LOD level as a separate file: <name>_lod1.<ext>, _lod2, …
    // format: "mesh", "fbx", "gltf2", "obj" — opens a directory picker first.
    Q_INVOKABLE void exportLods(const QString& format);

signals:
    void selectionChanged();
    void lodChanged();
    void generationSucceeded(int levels);
    void exportSucceeded(int count, const QString& directory);
    void error(const QString& message);

private:
    MeshLodController();
    ~MeshLodController() override;

    static MeshLodController* m_pSingleton;
    std::unique_ptr<Ogre::MeshLodGenerator> m_generator;
};

#endif // MESHLODCONTROLLER_H
