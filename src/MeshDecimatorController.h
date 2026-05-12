#ifndef MESHDECIMATORCONTROLLER_H
#define MESHDECIMATORCONTROLLER_H

#include <QObject>
#include <QQmlEngine>

// Inspector-side decimation singleton. Parallels MeshLodController but for
// single-pass (base-mesh) reduction rather than the LOD chain.
//
// Workflow expected from QML:
//   1. user opens the Decimate section while a mesh is selected
//   2. slider changes — QML calls previewReduction(r). The controller
//      generates a *temporary* LOD at fraction r and swaps display to
//      it via setMeshLodBias, then emits previewChanged(triangleCount).
//   3. user clicks Apply — applyReduction(r) commits the swap to the
//      base mesh and emits applied(report).
//   4. selection changes — controller resets to "no preview" automatically.
//
// All operations require a current selection; with no selection the
// methods are no-ops and emit error().
class MeshDecimatorController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(int baseTriangleCount READ baseTriangleCount NOTIFY baseChanged)
    Q_PROPERTY(int previewTriangleCount READ previewTriangleCount NOTIFY previewChanged)
    Q_PROPERTY(bool hasActivePreview READ hasActivePreview NOTIFY previewChanged)

public:
    static MeshDecimatorController* instance();
    static MeshDecimatorController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasSelection() const;
    // Counts are refreshed on selection change; refresh once at first read
    // via the Q_INVOKABLE primeBaseline() entry point (QML calls it in
    // Component.onCompleted to handle the singleton-after-selection case
    // without const_cast tricks in the getter).
    int baseTriangleCount() const { return m_baseTriangleCount; }
    int previewTriangleCount() const { return m_previewTriangleCount; }
    bool hasActivePreview() const { return m_hasPreview; }

    // Force a baseline recount + signal. QML calls this on section load.
    Q_INVOKABLE void primeBaseline();

    // Generate a temporary LOD at fraction `reduction` and swap display
    // to it. Cheap-ish but not free — debounce on the QML side so we don't
    // regenerate on every slider sample.
    Q_INVOKABLE void previewReduction(double reduction);

    // Drop the preview LOD; restore the base mesh.
    Q_INVOKABLE void clearPreview();

    // Commit the current preview (if any) to the base mesh. Subsequent
    // operations see the reduced mesh as the new baseline. Emits applied().
    Q_INVOKABLE void applyReduction(double reduction);

signals:
    void selectionChanged();
    void baseChanged();
    void previewChanged();
    void applied(int trianglesBefore, int trianglesAfter);
    void error(const QString& message);

private:
    MeshDecimatorController();
    ~MeshDecimatorController() override = default;

    void refreshBaseline();

    static MeshDecimatorController* m_pSingleton;
    int m_baseTriangleCount = 0;
    int m_previewTriangleCount = 0;
    bool m_hasPreview = false;
};

#endif // MESHDECIMATORCONTROLLER_H
