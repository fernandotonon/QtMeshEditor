#ifndef QUAD_RETOPO_CONTROLLER_H
#define QUAD_RETOPO_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QVariantMap>

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

public:
    static QuadRetopoController* instance();
    static QuadRetopoController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    bool hasSelection() const;
    bool busy() const { return m_busy; }

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
    void error(const QString& message);

private:
    QuadRetopoController();
    ~QuadRetopoController() override = default;

    static QuadRetopoController* m_pSingleton;
    bool m_busy = false;
};

#endif // QUAD_RETOPO_CONTROLLER_H
