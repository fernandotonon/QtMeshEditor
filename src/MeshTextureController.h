#ifndef MESH_TEXTURE_CONTROLLER_H
#define MESH_TEXTURE_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>

// QML-facing singleton for mesh-aware texture generation (issue
// #403). Orchestrates: render a depth map of the selected entity
// (MeshDepthRenderer) → kick off depth-conditioned generation
// through SDManager (generateMeshTexture) → the existing
// MaterialEditorQML::onSDGenerationCompleted path applies the
// result to the active material's diffuse slot.
//
// All sd.cpp work happens on SDManager's worker thread; progress
// and completion are surfaced through SDManager's existing signals
// (the GUI already binds to sdGenerationProgress / sdIsGenerating).
//
// The whole feature is meaningful only when ENABLE_STABLE_DIFFUSION
// is compiled in; with it off, `generateForSelected` returns an
// error string explaining the build flag.
class MeshTextureController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool sdAvailable READ sdAvailable CONSTANT)

public:
    static MeshTextureController* instance();
    static MeshTextureController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasSelection() const;
    /// True when the binary was built with ENABLE_STABLE_DIFFUSION.
    bool sdAvailable() const;

    // Render the selected entity's depth map and start depth-
    // conditioned generation. `controlNetPath` may be empty (falls
    // back to plain txt2img). `depthSize` is the depth-render
    // resolution (clamped 64..2048). Returns an error string
    // (empty on success — generation then proceeds asynchronously
    // via SDManager signals).
    Q_INVOKABLE QString generateForSelected(const QString& prompt,
                                            const QString& controlNetPath,
                                            double controlStrength,
                                            int depthSize);

signals:
    void selectionChanged();

private:
    MeshTextureController();
    ~MeshTextureController() override = default;

    static MeshTextureController* m_pSingleton;
};

#endif // MESH_TEXTURE_CONTROLLER_H
