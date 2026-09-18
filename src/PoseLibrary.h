/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef POSELIBRARY_H
#define POSELIBRARY_H

#include <QHash>
#include <QList>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <OgreQuaternion.h>
#include <OgreVector.h>

namespace Ogre { class Entity; class Skeleton; class SkeletonInstance; }

/**
 * @brief QML_SINGLETON storing named skeleton-pose snapshots.
 *
 * A pose is a frozen capture of every bone's TRS at one moment in
 * time. Use cases: T-pose / A-pose / neutral resting positions,
 * named facial expressions ("smile_l", "frown"), reference frames
 * authors snap to before keying.
 *
 * Slice D1 (this file) ships the data layer:
 *
 *   - `savePose(entity, name)` — captures every Bone TRS into a
 *     `BonePoseSnapshot` keyed by bone name. Storage is per-entity
 *     so two characters that share a skeleton mesh but have
 *     different posed states each carry their own library.
 *
 *   - `applyPose(entity, name)` — writes the snapshot's TRS values
 *     back onto the entity's `SkeletonInstance`. Snap-apply only —
 *     time-blended apply lands in D2 alongside the Inspector UI.
 *
 *   - `listPoses(entity)`, `deletePose(entity, name)`, `hasPose(...)`.
 *
 * Authoring (mirror, blend two, apply-with-mask), thumbnails, and
 * project-file persistence land in subsequent sub-slices.
 *
 * Pose storage is in-memory only for D1. The library lives for
 * the editor session; closing the project drops it. Persistence
 * to a `.poselib` sidecar arrives with D-Project.
 */
class PoseLibrary : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    static PoseLibrary* instance();
    static PoseLibrary* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// Capture the current bone TRS state on `entity` under `name`.
    /// Overwrites any existing pose of the same name (the contract
    /// matches "save as current" — re-saving updates in place).
    /// Returns false when entity is null, has no skeleton, or
    /// `name` is empty.
    bool savePose(Ogre::Entity* entity, const QString& name);

    /// Hold `boneHandles` against the animation system so an applied pose
    /// survives: manual control (excludes them from Skeleton::reset, which
    /// would snap them to BIND) plus a zeroed per-state blend-mask entry
    /// (manual control alone does NOT stop tracks applying). Returns the
    /// number of bones held.
    static int holdPosedBones(Ogre::Entity* entity,
                              const QList<unsigned short>& boneHandles);

    /// Release every held bone on `entity` — clears manual control, restores
    /// full blend-mask weight and resets the skeleton, handing the rig back
    /// to normal clip playback. Returns the number of bones released.
    static int releasePosedBones(Ogre::Entity* entity);

    /// True when any bone on `entity` is currently held by an applied pose
    /// (see holdPosedBones). Callers that force a full skeleton refresh must
    /// check this first — a blanket reset(true) wipes a held pose.
    static bool hasHeldBones(Ogre::Entity* entity);


    /// Apply a saved pose to `entity` — sets every captured bone's
    /// TRS back to the snapshotted values. Bones present on the
    /// snapshot but missing from the current skeleton are skipped
    /// silently (handles partial skeletons / future LOD changes).
    /// Returns false when the pose name isn't found on `entity`.
    bool applyPose(Ogre::Entity* entity, const QString& name);

    /// Apply a saved pose to a SUBSET of the entity's bones —
    /// only bones whose names appear in `boneFilter` are touched.
    /// Use case: apply a facial expression without disturbing the
    /// body pose, or apply an arm gesture without re-posing the
    /// legs. Empty `boneFilter` is treated as "no bones at all"
    /// (matches the strict-filter interpretation; pass the full
    /// `applyPose` for "everything"). Returns false when the pose
    /// name isn't found.
    bool applyPoseMasked(Ogre::Entity* entity,
                          const QString& name,
                          const QSet<QString>& boneFilter);

    /// Drop a saved pose. Returns false when the name doesn't exist.
    bool deletePose(Ogre::Entity* entity, const QString& name);

    /// Has `entity` got a pose called `name`?
    bool hasPose(Ogre::Entity* entity, const QString& name) const;

    /// All pose names on `entity` in save-order.
    QStringList listPoses(Ogre::Entity* entity) const;

    /// Persist this entity's pose library to a `.poselib` sidecar
    /// JSON file. Returns false on write error (path unwritable,
    /// no entity, no poses to save). The file format is
    /// `qtmesheditor.poselib.v1` — see `loadPoseLibrary` for the
    /// shape contract. Side-by-side with the source asset is the
    /// recommended location so the library follows the asset
    /// through version control.
    Q_INVOKABLE bool savePoseLibrary(Ogre::Entity* entity, const QString& filePath) const;

    /// Load a `.poselib` sidecar JSON file and replace this
    /// entity's in-memory library with its contents. Returns false
    /// on read / parse error (file missing, JSON malformed, schema
    /// mismatch). Existing poses on the entity are wiped first so
    /// the result reflects the file 1:1 (no partial-overlay).
    /// Emits `posesChanged` so the Inspector / dope-sheet refresh.
    Q_INVOKABLE bool loadPoseLibrary(Ogre::Entity* entity, const QString& filePath);

    /// Selection wrappers — same pattern as save/apply/delete.
    Q_INVOKABLE bool savePoseLibraryForSelection(const QString& filePath) const;
    Q_INVOKABLE bool loadPoseLibraryForSelection(const QString& filePath);

    /// Scene-level persistence (#521 follow-up). A `.poselib` written
    /// by `savePoseLibrary` holds ONE entity's poses; a scene export
    /// has many entities, so the scene sidecar nests each entity's
    /// library under the name of the SCENE NODE it hangs from:
    ///
    ///   { "schema": "qtmesheditor.poselib.v1",
    ///     "entities": [ { "node": "Hero", "poses": [ ... ] },
    ///                   { "node": "Guard", "poses": [ ... ] } ] }
    ///
    /// The node name is the key because that is what `sceneExporter`
    /// writes into the glTF and what `sceneImporter` recreates — the
    /// Entity POINTER obviously can't survive a reload, and entity
    /// names are not guaranteed unique across a scene the way node
    /// names are.
    ///
    /// `entities` and the single-entity `poses` are BOTH optional and
    /// may coexist, so the two writers share one schema version and an
    /// old single-entity sidecar still loads (see `loadPoseLibrary`).
    ///
    /// `nodesToEntities` maps scene-node name → the entity to read
    /// from / write to. Returns false on write error or when no
    /// entity in the map has any poses (nothing to persist).
    bool saveSceneLibraries(const QHash<QString, Ogre::Entity*>& nodesToEntities,
                            const QString& filePath) const;

    /// Read a scene sidecar and restore each named node's library onto
    /// the matching entity. Nodes present in the file but absent from
    /// `nodesToEntities` are skipped (the scene changed since export);
    /// entities present in the map but absent from the file are left
    /// untouched. Returns the number of entities whose library was
    /// restored, or -1 on read/parse/schema error.
    int loadSceneLibraries(const QHash<QString, Ogre::Entity*>& nodesToEntities,
                           const QString& filePath);

    /// Mirror a saved pose across the YZ plane (X = symmetry axis,
    /// the convention every common rig follows). Reads `srcName`
    /// from the library on `entity`, flips each bone's TRS by:
    ///   - mapping the bone name via the `_l`/`_r`, `.L`/`.R`,
    ///     `Left`/`Right` heuristic (so a left-hand keyframe lands
    ///     on the right hand, and vice-versa);
    ///   - reflecting the position's X component (pos.x → -pos.x);
    ///   - flipping the Y/Z parts of the rotation quaternion
    ///     (w,x,y,z → w,x,-y,-z), which is the X-axis-symmetric
    ///     reflection of an orientation;
    ///   - negating scale.x so the volume stays right and the
    ///     mirrored bone aligns with the mirrored axis.
    ///
    /// Bones whose names don't match the heuristic (centre-line
    /// bones like Spine, Hips) keep their TRS reflected in place.
    ///
    /// Writes the result under `dstName`. Returns false if `entity`
    /// is null, has no skeleton, `srcName` doesn't exist, or
    /// `dstName` is empty. `srcName == dstName` is allowed and
    /// overwrites the source.
    bool mirrorPose(Ogre::Entity* entity,
                    const QString& srcName,
                    const QString& dstName);

    /// Heuristic name flip for mirror-pose. Recognises three
    /// common rig conventions:
    ///   - suffix `_l` ↔ `_r` (`Mixamo_Hand_l` ↔ `Mixamo_Hand_r`)
    ///   - suffix `.L` ↔ `.R` (Blender convention, case-preserved)
    ///   - prefix `Left` ↔ `Right` (Maya convention, case-preserved)
    /// Returns the original name unchanged when no rule matches —
    /// centre-line bones (`Spine`, `Hips`, `Head`) flow through
    /// untouched and just have their TRS reflected in place.
    /// Pure function with no Ogre dependency — exposed publicly
    /// for tests and so future apply-with-mask code can reuse it.
    static QString flipBoneName(const QString& boneName);

    /// Blend two saved poses and write the result under `dstName`.
    ///
    /// `weight` is the position on the A→B axis: 0 = pure `aName`,
    /// 1 = pure `bName`, 0.5 = the halfway pose. Values outside
    /// [0,1] are clamped (extrapolation past the endpoints produces
    /// wild rotations on real rigs, so we refuse rather than
    /// surprise the author).
    ///
    /// Per bone: translation and scale interpolate linearly;
    /// rotation uses `Quaternion::Slerp` with `shortestPath=true`,
    /// which is the "dual-quat-correct" behaviour the issue asks for
    /// — without shortest-path a 350° blend spins the long way round.
    ///
    /// Bone-set handling: the result covers the UNION of both poses'
    /// bones. A bone present in only one pose is taken from that
    /// pose verbatim (there's nothing to interpolate against, and
    /// falling back to the live skeleton would make the result
    /// depend on the current pose — non-deterministic).
    ///
    /// Returns false when `entity` is null, either source pose is
    /// missing, or `dstName` is empty. `dstName` may equal `aName`
    /// or `bName` (overwrites in place).
    bool blendPoses(Ogre::Entity* entity,
                    const QString& aName,
                    const QString& bName,
                    float weight,
                    const QString& dstName);

    /// Start a time-blended apply: over `durationSeconds` the live
    /// skeleton eases from wherever it is NOW to the saved pose
    /// `name`. Call `tickBlend(dt)` each frame to advance it (the
    /// MainWindow render loop does this).
    ///
    /// The starting pose is snapshotted at call time from the live
    /// bones, so the blend is stable even if something else writes
    /// to the skeleton mid-transition — each tick recomputes from
    /// (captured start, target, elapsed) rather than accumulating.
    ///
    /// `durationSeconds <= 0` applies instantly (equivalent to
    /// `applyPose`) and leaves no active blend. Starting a new blend
    /// replaces any in-flight one on the same entity.
    ///
    /// Returns false when the pose isn't found / entity has no
    /// skeleton.
    bool applyPoseBlended(Ogre::Entity* entity,
                          const QString& name,
                          float durationSeconds);

    /// Advance every in-flight `applyPoseBlended` transition by `dt`
    /// seconds and write the interpolated TRS onto the live bones.
    /// Completed blends land exactly on the target pose and are
    /// removed. Returns the number of blends still running after the
    /// tick (0 = nothing to do, so the render loop can skip cheaply).
    int tickBlend(float dt);

    /// Is a time-blend currently running on `entity`?
    bool isBlending(Ogre::Entity* entity) const;

    /// Cancel an in-flight blend WITHOUT snapping to the target —
    /// the skeleton keeps whatever partial pose it reached. Used by
    /// the undo path (which restores its own snapshot) and when an
    /// entity is torn down. Returns true if a blend was cancelled.
    bool cancelBlend(Ogre::Entity* entity);

    /// Drop every entry on `entity` (called when an entity is
    /// destroyed or a scene closes). No-op if `entity` was never
    /// saved. Returns `true` when something was actually erased so
    /// the caller can tell whether to refresh the UI.
    bool forgetEntity(Ogre::Entity* entity);

    /// Drop every entry across every entity. Used by tests to
    /// isolate cases that share the singleton, and by the future
    /// "close project" path to wipe the library.
    void clearAll();

    /// QML-friendly variants that resolve `entity` from
    /// SelectionSet's first entity. Used by the future Inspector
    /// "Pose Library" subgroup.
    Q_INVOKABLE bool savePoseForSelection(const QString& name);
    Q_INVOKABLE bool applyPoseForSelection(const QString& name);

    /// Selection wrapper for D5 apply-with-mask. `boneNames` is a
    /// QML/MCP-friendly QStringList; internally converted to QSet
    /// for the underlying `applyPoseMasked` call.
    Q_INVOKABLE bool applyPoseMaskedForSelection(const QString& name,
                                                  const QStringList& boneNames);
    Q_INVOKABLE bool deletePoseForSelection(const QString& name);
    Q_INVOKABLE bool mirrorPoseForSelection(const QString& srcName,
                                             const QString& dstName);
    Q_INVOKABLE QStringList listPosesForSelection() const;

    /// Selection wrappers for the D2 blend surface. These are the
    /// entry points the Inspector's Pose Library panel calls; each
    /// routes through the undo stack (see `PoseLibraryCommands`) so
    /// the acceptance criterion "all operations are undoable" holds
    /// for the GUI path.
    Q_INVOKABLE bool blendPosesForSelection(const QString& aName,
                                             const QString& bName,
                                             double weight,
                                             const QString& dstName);
    Q_INVOKABLE bool applyPoseBlendedForSelection(const QString& name,
                                                   double durationSeconds);

    /// Undoable variants used by the GUI. They push the matching
    /// command onto `UndoManager` and return whether the command was
    /// accepted (i.e. the operation's preconditions held).
    Q_INVOKABLE bool savePoseUndoable(const QString& name);
    Q_INVOKABLE bool applyPoseUndoable(const QString& name);
    Q_INVOKABLE bool applyPoseBlendedUndoable(const QString& name,
                                               double durationSeconds);
    Q_INVOKABLE bool deletePoseUndoable(const QString& name);
    Q_INVOKABLE bool mirrorPoseUndoable(const QString& srcName,
                                         const QString& dstName);
    Q_INVOKABLE bool blendPosesUndoable(const QString& aName,
                                         const QString& bName,
                                         double weight,
                                         const QString& dstName);
    Q_INVOKABLE bool applyPoseMaskedUndoable(const QString& name,
                                              const QStringList& boneNames);

    /// Bone names on the current selection's skeleton, in skeleton
    /// order. Feeds the panel's apply-with-mask bone picker.
    Q_INVOKABLE QStringList boneNamesForSelection() const;

    /// A `data:image/png;base64,…` thumbnail of `name` as it looks on
    /// the current selection, or an empty string when unavailable
    /// (no pose, no entity, headless/no-GL environment).
    ///
    /// Rendering strategy: the entity is temporarily posed to the
    /// snapshot, rendered offscreen via `ModelTurntableRenderer`, and
    /// the pre-existing live pose is restored — so asking for a
    /// thumbnail never disturbs what the author is looking at.
    /// Results are cached per (entity, pose); the cache entry is
    /// dropped when the pose is re-saved, mirrored over, blended
    /// over, or deleted.
    Q_INVOKABLE QString poseThumbnailForSelection(const QString& name);

    /// Edge length of a rendered pose thumbnail, in pixels. Small
    /// enough that a library of 30 poses stays cheap to hold as
    /// base64 in the QML list.
    static constexpr int kThumbnailSize = 96;

signals:
    /// Emitted after savePose / deletePose changes the per-entity
    /// pose list visible to the Inspector / dope-sheet / MCP.
    void posesChanged(Ogre::Entity* entity);

    /// Emitted when a time-blended apply finishes (or is cancelled),
    /// so the panel can drop its "blending…" affordance.
    void blendFinished(Ogre::Entity* entity, const QString& name);

    /// Emitted when the panel's Export/Import .poselib buttons are
    /// pressed. MainWindow opens the native QFileDialog (it owns a
    /// QWidget parent; QML can't supply one) and calls back into
    /// `savePoseLibraryForSelection` / `loadPoseLibraryForSelection`.
    /// Mirrors HdrEnvironmentController::browseRequested.
    void exportLibraryRequested();
    void importLibraryRequested();

public slots:
    /// QML-callable triggers for the two signals above.
    void requestExportLibrary();
    void requestImportLibrary();

private:
    explicit PoseLibrary(QObject* parent = nullptr);
    ~PoseLibrary() override;

    /// Per-bone snapshot. We deliberately don't store the bone
    /// handle (handles can change across LOD / skeleton variants);
    /// the bone NAME is the stable identifier.
    struct BonePoseSnapshot {
        Ogre::Vector3 translate{Ogre::Vector3::ZERO};
        Ogre::Quaternion rotation{Ogre::Quaternion::IDENTITY};
        Ogre::Vector3 scale{Ogre::Vector3(1, 1, 1)};
    };

    /// One named pose = bone name → TRS snapshot.
    using PoseSnapshot = QHash<QString, BonePoseSnapshot>;

    /// Per-entity storage. QHash is intentionally unordered for the
    /// inner pose-name → snapshot lookup (fast), and we maintain a
    /// parallel insertion-ordered QStringList for `listPoses` so
    /// the UI sees stable save-order rather than hash buckets.
    struct EntityPoses {
        QHash<QString, PoseSnapshot> byName;
        QStringList order;  // matches savePose() insertion order
    };
    QHash<Ogre::Entity*, EntityPoses> m_byEntity;

    /// One in-flight `applyPoseBlended` transition. `from` is the
    /// live bone state captured when the blend started; `to` is the
    /// (copied, not referenced) target snapshot — copying means a
    /// delete/overwrite of the source pose mid-blend can't dangle.
    struct ActiveBlend {
        QString name;
        PoseSnapshot from;
        PoseSnapshot to;
        float elapsed = 0.0f;
        float duration = 0.0f;
    };
    QHash<Ogre::Entity*, ActiveBlend> m_blends;

    /// Cached thumbnails, keyed "<entity-ptr>/<pose name>". Values are
    /// `data:image/png;base64,…` URIs.
    QHash<QString, QString> m_thumbCache;

    /// Build the cache key and drop any cached thumbnail for a pose
    /// whose content just changed.
    static QString thumbKey(Ogre::Entity* entity, const QString& name);
    void invalidateThumbnail(Ogre::Entity* entity, const QString& name);

    /// Shared implementation behind `savePose` / `mirrorPose` /
    /// `blendPoses` — inserts `snapshot` under `name`, maintains the
    /// insertion-ordered `order` list, drops the stale thumbnail and
    /// emits `posesChanged`. Returns true when it overwrote an
    /// existing pose (callers use it for the breadcrumb text).
    bool storePose(Ogre::Entity* entity,
                   const QString& name,
                   const PoseSnapshot& snapshot);

    /// Serialise one entity's library to the `poses` JSON array shape
    /// shared by the single-entity and scene sidecars. Empty array
    /// when the entity has nothing saved.
    QJsonArray posesToJson(Ogre::Entity* entity) const;

    /// Inverse of `posesToJson`. Returns the parsed library; the
    /// caller decides whether to commit it (so a partly-bad file
    /// can't half-overwrite an existing library).
    static EntityPoses posesFromJson(const QJsonArray& poses);

    /// Swap `staging` in as `entity`'s library, drop that entity's
    /// stale thumbnails + any in-flight blend, and emit posesChanged.
    /// Shared by the single-entity and scene loaders so both get the
    /// same all-or-nothing replacement semantics.
    void commitLoadedLibrary(Ogre::Entity* entity, const EntityPoses& staging);

    /// Read + parse a sidecar and verify its schema. Returns false
    /// (leaving `root` untouched) on any read/parse/schema failure so
    /// callers can bail before mutating in-memory state.
    static bool readSidecarRoot(const QString& filePath, QJsonObject& root);

    /// Read the live skeleton into a snapshot. Returns an empty
    /// snapshot when the entity has no skeleton.
    static PoseSnapshot captureLive(Ogre::Entity* entity);

    /// Write `snapshot` onto the live skeleton, skipping bones the
    /// skeleton doesn't have. Returns the number of bones written.
    static int applySnapshot(Ogre::Entity* entity, const PoseSnapshot& snapshot);

    /// Push freshly-written bone locals into derived transforms so the
    /// skin / debug visuals / TagPoints update in the same frame. Bone
    /// TRS writes alone leave everything downstream on the old pose.
    static void flushSkeletonPose(Ogre::Entity* entity,
                                  Ogre::SkeletonInstance* skel);

    static PoseLibrary* s_instance;
};

#endif // POSELIBRARY_H
