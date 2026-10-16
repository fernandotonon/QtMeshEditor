#ifndef QUAD_RETOPO_CONTROLLER_H
#define QUAD_RETOPO_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QVariantMap>

#include <atomic>
#include <memory>

// QML-facing singleton for the triangle-pairing quad retopology
// (issue #401). Wraps `QuadRetopo::retopologize` plus selection-state
// property so the Inspector button can disable itself when no entity
// is selected. Headless callers (CLI / MCP) use `QuadRetopo` directly.
class QuadRetopoController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // QuadriFlow remesh progress (0..1) + stage text while `busy`.
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)
    Q_PROPERTY(bool quadriFlowAvailable READ quadriFlowAvailable CONSTANT)

public:
    static QuadRetopoController* instance();
    static QuadRetopoController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasSelection() const;
    bool busy() const { return m_busy; }
    double progress() const { return m_progress; }
    QString progressText() const { return m_progressText; }
    bool quadriFlowAvailable() const;

    // Suggested QuadriFlow face budget for the selection:
    // QuadRemesh::defaultTargetFaces (the CLI/MCP default).
    Q_INVOKABLE int suggestedQuadriFlowTarget() const;

    // Field-aligned QuadriFlow REMESH of the selected entity. Runs the
    // remesh on a worker thread (the UI stays live, progress/progressText
    // update, cancelRemesh() stops it) and applies the result as ONE undo
    // step. Options: targetFaces, preserveSharp, preserveBoundary,
    // adaptive, bakeTextures, detailNormal, voxel ("auto"|"on"|"off"),
    // seed. Returns false (and emits error) when it cannot start.
    // Emits remeshFinished(report) or error(msg).
    Q_INVOKABLE bool remeshSelectedQuadriFlow(const QVariantMap& options);
    Q_INVOKABLE void cancelRemesh();

    // Run the retopology on the first resolved selected entity. The
    // entity's mesh is rewritten in place: triangle pairs become quads
    // and the `qtme.faces.<i>` binding is updated so exporters round-
    // trip the new topology. Unlike `unwrapEntityToFile`, this does
    // NOT require an export — the in-place mutation is safe because
    // the n-gon binding leaves the triangle index buffer alone (just
    // gets fan-retriangulated by `triangulateFaces`).
    //
    // Returns a QVariantMap mirroring `QuadRetopoReport::*` so the
    // dialog can surface "N quads, M tris, 60% quad dominance" after
    // Apply. Emits `retopoApplied(report)` on success or `error(msg)`
    // on failure.
    Q_INVOKABLE QVariantMap retopologizeSelected(int targetFaces,
                                                 double maxAngleDeg,
                                                 double shapeToleranceDeg,
                                                 double maxAspectRatio);

signals:
    void selectionChanged();
    void busyChanged();
    void retopoApplied(const QVariantMap& report);
    void progressChanged();
    void remeshFinished(const QVariantMap& report);
    void error(const QString& message);

private:
    QuadRetopoController();
    ~QuadRetopoController() override = default;

    static QuadRetopoController* m_pSingleton;
    bool m_busy = false;
    double m_progress = 0.0;
    QString m_progressText;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};

#endif // QUAD_RETOPO_CONTROLLER_H
