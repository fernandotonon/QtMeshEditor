#ifndef PARTOPSCONTROLLER_H
#define PARTOPSCONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>

/**
 * PartOps GUI controller (#859/#861): the Object-mode "Split into Parts"
 * action. QML_SINGLETON, mirroring MeshDecimatorController.
 *
 * splitSelectedIntoParts() segments the selected fused mesh (rig-prior / ONNX
 * / geometric, same pipeline as the CLI) and replaces it with a mesh whose
 * submeshes are the detected parts (head/torso/…), via an undoable
 * SplitMeshCommand (Ctrl+Z restores the fused mesh). Self-contained: no Edit
 * Mode required — the split changes the submesh count, which the edit-mode
 * in-place path forbids, so it swaps the whole mesh on the scene node.
 *
 * Synchronous today: the default GUI use (a rigged character, or the offline
 * geometric fallback) resolves without a model download; the ONNX path can
 * block on first-use download — a future revision can move it to a worker like
 * EditModeController::selectByPart.
 */
class PartOpsController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)

public:
    static PartOpsController* instance();
    static PartOpsController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /** True when exactly one mesh entity is selected (the split target). */
    bool hasSelection() const;

    /** Split the selected mesh into per-part submeshes (undoable).
     *  @param upAxis    "x"|"y"|"z" (default "y") — forwarded to segmentation.
     *  @param category  "auto"|"body"|"vegetation"|"vehicle"|"building".
     *  @param noModel   force the offline geometric/rig-prior path.
     *  Emits splitFinished(status, isError). No-op (error) without a single
     *  selected mesh. */
    Q_INVOKABLE void splitSelectedIntoParts(const QString& upAxis = QStringLiteral("y"),
                                            const QString& category = QStringLiteral("auto"),
                                            bool noModel = false);

signals:
    void selectionChanged();
    void splitFinished(const QString& status, bool isError);

private:
    PartOpsController();
    static PartOpsController* m_pSingleton;
};

#endif // PARTOPSCONTROLLER_H
