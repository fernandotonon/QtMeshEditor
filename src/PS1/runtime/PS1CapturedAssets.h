#ifndef PS1CAPTUREDASSETS_H
#define PS1CAPTUREDASSETS_H

#include "CaptureSnapshot.h"
#include "MeshReconstructor.h"

#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

/**
 * One row in the geometry inspector table (#426).
 *
 * Each row corresponds to one draw call from the captured `PrimRecord`
 * stream — tri / quad / sprite. The row also carries derived information
 * the inspector renders directly (triangle count, material name, the
 * unique mesh + instance the prim ends up in after dedupe) so the table
 * model doesn't have to walk the raw capture data on every paint.
 */
struct CapturedAssetRow
{
    /** 1-based row index. Matches the `#` column in the inspector table. */
    int rowIndex = 0;
    /** Frame number this prim was emitted on. Always 0 for single-shot
     *  `captureFrame()` captures; for `captureScene()` captures this is the
     *  worker's frame counter, which is monotonic but resets per session. */
    quint64 frameIndex = 0;
    PrimKind kind = PrimKind::MonoTri;
    int vertexCount = 0;
    uint16_t tpage = 0;
    uint16_t clut = 0;
    uint32_t matrixId = 0;
    /** Logical material name (matches `ReconstructedSubMesh::materialName`,
     *  i.e. `PS1Rip_tpage_..._clut_..._stN_dmM` for textured prims or
     *  `PS1Rip_color` for solid prims). Maps to the Ogre material the prim
     *  was reconstructed into. */
    QString materialName;
    /** 1 for triangles, 2 for quads / sprites (PS1 quads always split into
     *  two triangles in the reconstructed mesh). */
    int triangleCount = 1;
    /** Index into `CapturedAssetSet::uniqueMeshes` that contains this prim.
     *  -1 if the prim couldn't be matched (matrixId mismatch, etc.). */
    int uniqueMeshIndex = -1;
    /** Submesh index within `uniqueMeshes[uniqueMeshIndex].subMeshes` that
     *  this prim wrote into. -1 if the prim wasn't matched. Used by the
     *  inspector to scope hide / highlight / discard actions to the
     *  specific `SubEntity` instead of the whole `SceneNode`, so two rows
     *  sharing an instance but different submeshes don't clobber each
     *  other (#679 review feedback). */
    int subMeshIndex = -1;
    /** Index into `CapturedAssetSet::instances` for the placed copy this
     *  prim belongs to. -1 if no instance was created for the prim's
     *  matrix group (e.g. the camera matrix). */
    int instanceIndex = -1;
    /** Convenience flags for filter chips in the inspector / browser. */
    bool textured = false;
    bool colored = false;
    /** User toggled "Hide submesh" — UI honours by hiding the corresponding
     *  Ogre SceneNode in the live capture (#426). */
    bool hidden = false;
    /** User toggled "Discard" — UI gray-outs the row, hides the live
     *  preview, and excludes from "Promote all" actions (#426). */
    bool discarded = false;
};

/**
 * Aggregated per-capture data, owned by `PS1CapturedAssets`.
 *
 * Populated by `PS1RipManager` after every successful `meshBuilt`. The
 * inspector and asset-browser dock query against this struct via the
 * singleton's accessors; they never look at the raw `CaptureSnapshot`
 * directly, so the rest of the editor doesn't have to deal with
 * `PrimRecord` indexing or matrix-group bookkeeping.
 */
struct CapturedAssetSet
{
    QString captureId;
    QVector<CapturedAssetRow> rows;
    QVector<ReconstructedMesh> uniqueMeshes;
    QVector<ReconstructedInstance> instances;
    /** Map an instance index (into `instances`) to the Ogre SceneNode name
     *  the mesh builder created for it (`PS1Capture_<id>_inst<N>`). The
     *  inspector uses this to resolve a row click into a SceneNode to
     *  highlight, hide, or promote. */
    QHash<int, QString> instanceNodeNames;
    /** Material name → decoded texture page (256×256 RGBA), if the
     *  material was textured. Populated by the mesh builder via
     *  `setTextureImages`; non-textured prims have no entry. */
    QHash<QString, QImage> textureImages;
    /** Distinct texture identities (deduplicated by tpage/clut/semi-trans).
     *  Used to compute the "11 textures" counter in the header. */
    QSet<QString> uniqueTextureIds;
    int totalPrims = 0;
    int totalTris = 0;
};

/**
 * Singleton store that holds the latest capture's draw-call rows + unique
 * meshes / textures for the Geometry Inspector and Extracted Asset
 * Browser (#426).
 *
 * - Lives on the GUI thread (main thread) and is updated from
 *   `PS1RipManager::meshBuilt` via a direct-connect slot, so callers
 *   reading from the inspector model don't race against the worker.
 * - Stays alive across multiple captures: each new `meshBuilt` replaces
 *   the previous set so the inspector always reflects the most recent
 *   capture, but the previous one is discarded (the live capture nodes
 *   are also recreated per capture by the mesh builder, so anything older
 *   is already gone from the scene).
 * - Holds no Ogre handles directly; SceneNode pointers are resolved on
 *   demand via `Manager::getSceneNode(nodeName)`. This means the store
 *   stays valid even when the user clears the scene — accessors just
 *   return `nullptr` for the resolved nodes.
 *
 * `clear()` resets state without destroying the singleton; useful for
 * tests that need a clean slate between cases.
 */
class PS1CapturedAssets : public QObject
{
    Q_OBJECT

public:
    static PS1CapturedAssets *getSingleton();
    static PS1CapturedAssets *getSingletonPtr();
    static void kill();

    /** Drop any previously stored capture data. */
    void clear();

    /** Replace the current capture set. Fires `captureSetChanged()`.
     *  Called from `PS1RipManager` after a successful attach. */
    void setCaptureSet(CapturedAssetSet set);

    const CapturedAssetSet &captureSet() const { return m_set; }
    bool hasCapture() const { return !m_set.captureId.isEmpty(); }

    /** Mutate a single row's hidden / discarded state. Fires
     *  `rowChanged(int)` so the inspector view can repaint the row. */
    bool setRowHidden(int rowIndex, bool hidden);
    bool setRowDiscarded(int rowIndex, bool discarded);

    /** Pure-data helpers used by both the panel and the unit tests. */
    static CapturedAssetSet buildFromCapture(const QString &captureId,
                                             const CaptureSnapshot &snapshot,
                                             const ReconstructedCaptureSet &reconstructed,
                                             const QHash<QString, QImage> &textureImages = {});

signals:
    /** Fires after `setCaptureSet`. UI listens to repopulate views. */
    void captureSetChanged();
    void rowChanged(int rowIndex);

private:
    explicit PS1CapturedAssets(QObject *parent = nullptr);
    ~PS1CapturedAssets() override = default;

    static PS1CapturedAssets *s_instance;
    CapturedAssetSet m_set;
};

#endif // PS1CAPTUREDASSETS_H
