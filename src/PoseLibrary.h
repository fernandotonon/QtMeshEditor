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
#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

#include <OgreQuaternion.h>
#include <OgreVector.h>

namespace Ogre { class Entity; class Skeleton; }

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

signals:
    /// Emitted after savePose / deletePose changes the per-entity
    /// pose list visible to the Inspector / dope-sheet / MCP.
    void posesChanged(Ogre::Entity* entity);

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

    static PoseLibrary* s_instance;
};

#endif // POSELIBRARY_H
