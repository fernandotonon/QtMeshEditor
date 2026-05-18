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

    /// Drop a saved pose. Returns false when the name doesn't exist.
    bool deletePose(Ogre::Entity* entity, const QString& name);

    /// Has `entity` got a pose called `name`?
    bool hasPose(Ogre::Entity* entity, const QString& name) const;

    /// All pose names on `entity` in save-order.
    QStringList listPoses(Ogre::Entity* entity) const;

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
    Q_INVOKABLE bool deletePoseForSelection(const QString& name);
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
