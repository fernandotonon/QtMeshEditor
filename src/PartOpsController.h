#ifndef PARTOPSCONTROLLER_H
#define PARTOPSCONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>

#include <atomic>
#include <memory>

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
    Q_PROPERTY(bool canExplode READ canExplode NOTIFY selectionChanged)
    Q_PROPERTY(bool canJoin READ canJoin NOTIFY selectionChanged)
    // Generic split (#1179) runs on a worker: busy / progress for the panel.
    Q_PROPERTY(bool splitBusy READ splitBusy NOTIFY splitProgressChanged)
    Q_PROPERTY(bool splitDownloading READ splitDownloading NOTIFY splitProgressChanged)
    Q_PROPERTY(int splitProgress READ splitProgress NOTIFY splitProgressChanged)
    Q_PROPERTY(int splitTotal READ splitTotal NOTIFY splitProgressChanged)

public:
    static PartOpsController* instance();
    static PartOpsController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /** True when exactly one mesh entity is selected (the split target). */
    bool hasSelection() const;

    /** True when exactly one multi-submesh mesh entity is selected (an explode
     *  target — a single-submesh mesh has nothing to explode). */
    bool canExplode() const;

    /** True when two or more mesh entities are selected (a join target). */
    bool canJoin() const;

    /** Split the selected mesh into per-part submeshes (undoable).
     *  @param upAxis    "x"|"y"|"z" (default "y") — forwarded to segmentation.
     *  @param category  "auto"|"body"|"vegetation"|"vehicle"|"building".
     *  @param noModel   force the offline geometric/rig-prior path.
     *  Emits splitFinished(status, isError). No-op (error) without a single
     *  selected mesh. */
    /**  "generic" (#1179): any object, unnamed parts part_01… via SAM 2.1.
     *  It takes about a minute, so it segments on a worker thread (progress
     *  via splitBusy/splitProgress, cancelSplit()) and then pushes the same
     *  undoable SplitMeshCommand with the computed labels. `granularity`
     *  ("coarse"|"medium"|"fine") applies to it. */
    Q_INVOKABLE void splitSelectedIntoParts(const QString& upAxis = QStringLiteral("y"),
                                            const QString& category = QStringLiteral("auto"),
                                            bool noModel = false,
                                            bool solidify = false,
                                            const QString& granularity = QStringLiteral("medium"));
    Q_INVOKABLE void cancelSplit();
    bool splitBusy() const { return m_splitBusy; }
    bool splitDownloading() const { return m_splitDownloading; }
    int splitProgress() const { return m_splitProgress; }
    int splitTotal() const { return m_splitTotal; }

    /** Explode the selected multi-submesh mesh into one scene node per part
     *  (undoable). Each part is pushed outward by `distance` × the assembly
     *  diagonal. Emits explodeFinished(status, isError). No-op (error) without
     *  a single multi-submesh selection. */
    Q_INVOKABLE void explodeSelected(double distance = 0.5);

    /** Join the selected part entities (2+) back into one fused mesh, baking
     *  their world transforms into vertices (undoable). Emits
     *  joinFinished(status, isError). No-op (error) with fewer than 2 selected. */
    Q_INVOKABLE void joinSelected();

signals:
    void selectionChanged();
    void splitFinished(const QString& status, bool isError);
    void explodeFinished(const QString& status, bool isError);
    void joinFinished(const QString& status, bool isError);
    void splitProgressChanged();

private:
    PartOpsController();
    void splitGeneric(const std::string& entityName, int axis, bool solidify,
                      const QString& granularity);
    void setSplitState(bool busy, bool downloading, int progress, int total);
    static PartOpsController* m_pSingleton;
    bool m_splitBusy = false;
    bool m_splitDownloading = false;
    int m_splitProgress = 0;
    int m_splitTotal = 0;
    std::shared_ptr<std::atomic_bool> m_splitCancel;
};

#endif // PARTOPSCONTROLLER_H
