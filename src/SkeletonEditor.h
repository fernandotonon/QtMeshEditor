#pragma once

#include <OgreAnimation.h>
#include <OgreMesh.h>
#include <OgreQuaternion.h>
#include <OgreVector3.h>

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Ogre {
class Bone;
class Entity;
class Mesh;
class Skeleton;
}

/// Bone-level CRUD + hierarchy + rest-pose editing for skeletal rigs
/// (epic #554: slice A #555 CRUD, slice B #556 hierarchy, slice C #557 rest pose).
class SkeletonEditor : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool editRestPoseMode READ editRestPoseMode WRITE setEditRestPoseMode
               NOTIFY editRestPoseModeChanged)
    Q_PROPERTY(bool showRestPoseGhost READ showRestPoseGhost WRITE setShowRestPoseGhost
               NOTIFY showRestPoseGhostChanged)

public:
    struct CreateOptions {
        QString parentBoneName; ///< empty → attach under skeleton root (first root bone)
        QString baseName = QStringLiteral("Bone");
        QString forcedName;   ///< when redoing undo, recreate with this exact name
    };

    struct RemoveOptions {
        bool removeChildren = false;
        bool transferWeightsToParent = true;
    };

    struct ReparentOptions {
        bool keepWorld = true; ///< false → keep local TRS unchanged
    };

    struct AttachOptions {
        /// Reserved for a future geometry-transfer mode. Slice B is rig-only.
        bool transferGeometry = false;
    };

    struct Result {
        bool ok = false;
        QString error;
        QString boneName; ///< created / duplicated / split bone name
    };

    /// Captured skeleton + mesh skin state for undo of destructive edits.
    struct Snapshot {
        std::string skeletonName;
        std::string meshName;
        struct BoneData {
            std::string name;
            unsigned short handle = 0;
            std::string parentName;
            Ogre::Vector3 position = Ogre::Vector3::ZERO;
            Ogre::Quaternion orientation = Ogre::Quaternion::IDENTITY;
            Ogre::Vector3 scale = Ogre::Vector3::UNIT_SCALE;
            Ogre::Vector3 initialPosition = Ogre::Vector3::ZERO;
            Ogre::Quaternion initialOrientation = Ogre::Quaternion::IDENTITY;
            Ogre::Vector3 initialScale = Ogre::Vector3::UNIT_SCALE;
        };
        struct KeyframeData {
            float time = 0.f;
            Ogre::Vector3 translate = Ogre::Vector3::ZERO;
            Ogre::Quaternion rotation = Ogre::Quaternion::IDENTITY;
            Ogre::Vector3 scale = Ogre::Vector3::UNIT_SCALE;
        };
        struct TrackData {
            unsigned short handle = 0;
            std::string boneName;
            bool useShortestRotationPath = true;
            std::vector<KeyframeData> keyframes;
        };
        struct AnimationData {
            std::string name;
            float length = 0.f;
            Ogre::Animation::InterpolationMode interpolationMode{};
            Ogre::Animation::RotationInterpolationMode rotationInterpolationMode{};
            std::vector<TrackData> tracks;
        };
        std::vector<BoneData> bones;
        std::vector<AnimationData> animations;
        std::vector<Ogre::VertexBoneAssignment> meshAssignments;
        std::vector<std::vector<Ogre::VertexBoneAssignment>> submeshAssignments;
        /// Bind-pose mesh positions/normals (for rest-pose bake undo/reset).
        /// submeshIndex -1 = shared VertexData; else dedicated submesh verts.
        struct BindVertexBuffer {
            int submeshIndex = -1;
            std::vector<float> positions; ///< xyz packed
            std::vector<float> normals;   ///< xyz packed (empty if none)
        };
        std::vector<BindVertexBuffer> bindVertexBuffers;
    };

    static SkeletonEditor* getSingleton();
    static SkeletonEditor* getSingletonPtr();
    static SkeletonEditor* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// Resolve the first selected entity that has a skeleton.
    static Ogre::Entity* selectedSkinnedEntity();

    /// Blender-style unique name: Bone, Bone.001, Bone.002, …
    static QString uniqueBoneName(const Ogre::Skeleton* skel, const QString& base);

    static Snapshot captureSnapshot(Ogre::Entity* entity);
    static bool restoreSnapshot(Ogre::Entity* entity, const Snapshot& snapshot, QString* error = nullptr);

    static Result createBone(Ogre::Entity* entity, const CreateOptions& opts);
    static Result removeBone(Ogre::Entity* entity, const QString& boneName, const RemoveOptions& opts);
    /// Remove the ENTIRE skeleton from the entity's mesh: clears every bone
    /// assignment (shared + per-submesh), unbinds the skeleton
    /// (setSkeletonName("")), and re-initialises the entity. The mesh becomes
    /// a plain static mesh again — Auto-Rig sees it as riggable, so the user
    /// can regenerate a rig from scratch. Skeletal animations go with the
    /// skeleton; mesh-level morph/pose animations are untouched. Undo is a
    /// full Snapshot restore (RemoveSkeletonCommand).
    static Result removeSkeleton(Ogre::Entity* entity);
    static Result renameBone(Ogre::Entity* entity, const QString& oldName, const QString& newName);
    static Result duplicateBone(Ogre::Entity* entity, const QString& sourceBoneName);

    /// Reparent `boneName` under `newParentName` (empty = detach to root).
    static Result reparentBone(Ogre::Entity* entity,
                               const QString& boneName,
                               const QString& newParentName,
                               const ReparentOptions& opts);
    /// Detach chain to root (keep-world).
    static Result detachBone(Ogre::Entity* entity, const QString& boneName);

    /// Copy bone subtree(s) from src onto dst's skeleton (rig-only; no VBA copy).
    static Result attachBonesToEntity(Ogre::Entity* srcEntity,
                                      const QStringList& boneNames,
                                      Ogre::Entity* dstEntity,
                                      const AttachOptions& opts);

    /// Insert a bone at fraction `t` along the selected bone's axis (0 < t < 1).
    static Result splitBone(Ogre::Entity* entity, const QString& boneName, float t = 0.5f);

    /// Snap / unsnap bone head to parent tip (Blender-style connect).
    static Result setBoneConnected(Ogre::Entity* entity, const QString& boneName, bool connected);
    static bool isBoneConnected(Ogre::Entity* entity, const QString& boneName);

    /// Cache the mesh skeleton's current bind pose as the "imported" rest
    /// (first call wins per mesh). Used by resetRestPose.
    static void ensureImportedRestCache(Ogre::Entity* entity);
    /// Clear cached imported rest for a mesh (tests / mesh destroy).
    static void clearImportedRestCache(Ogre::Entity* entity = nullptr);

    /// Set rest pose of the listed bones (empty list = all) to their current
    /// evaluated local TRS, rebake animation tracks so world motion is
    /// unchanged, and refresh inverse binds. Mutates the mesh skeleton.
    static Result captureRestPose(Ogre::Entity* entity, const QStringList& boneNames = {});
    /// Restore bind pose (+ rebaked tracks) to the cached imported rest.
    static Result resetRestPose(Ogre::Entity* entity);
    /// Commit an explicit new rest TRS for one bone (edit-rest gizmo path).
    static Result commitBoneRestPose(Ogre::Entity* entity,
                                     const QString& boneName,
                                     const Ogre::Vector3& pos,
                                     const Ogre::Quaternion& orient,
                                     const Ogre::Vector3& scale);

    static void refreshAfterEdit(const std::string& entityName, const QString& selectBone = {});

    /// Push undo commands — used by QML and tests.
    Q_INVOKABLE bool createBoneForSelected(const QString& parentBoneName = {});
    Q_INVOKABLE bool removeSelectedBone(bool removeChildren, bool transferWeightsToParent);
    Q_INVOKABLE bool removeSelectedSkeleton();
    Q_INVOKABLE bool renameSelectedBone(const QString& newName);
    Q_INVOKABLE bool duplicateSelectedBone();
    Q_INVOKABLE bool reparentSelectedBone(const QString& newParentName, bool keepWorld = true);
    Q_INVOKABLE bool detachSelectedBone();
    Q_INVOKABLE bool splitSelectedBone(float t = 0.5f);
    Q_INVOKABLE bool setSelectedBoneConnected(bool connected);
    Q_INVOKABLE bool isSelectedBoneConnected() const;
    Q_INVOKABLE bool attachSelectedBoneToEntity(const QString& dstEntityName);

    Q_INVOKABLE bool captureRestPoseForSelected();
    Q_INVOKABLE bool resetRestPoseForSelected();
    Q_INVOKABLE bool snapSelectedBonesToCurrentPose();

    Q_INVOKABLE bool hasSkeletonSelection() const;
    Q_INVOKABLE QString selectedBoneName() const;
    /// Current parent of the selected bone; empty string if root / none.
    Q_INVOKABLE QString selectedBoneParentName() const;
    /// Candidate parents for the selected bone (excludes self + descendants).
    Q_INVOKABLE QStringList reparentCandidateParents() const;
    /// Other scene entities that can receive an attached bone.
    Q_INVOKABLE QVariantList attachTargetEntities() const;

    bool editRestPoseMode() const { return m_editRestPoseMode; }
    void setEditRestPoseMode(bool on);
    bool showRestPoseGhost() const { return m_showRestPoseGhost; }
    void setShowRestPoseGhost(bool on);

    /// Request the floating bone context menu at a global screen position
    /// (viewport right-click or Inspector bone picker).
    void requestBoneContextMenu(int globalX, int globalY);

signals:
    void boneCreated(const QString& entityName, const QString& boneName);
    void boneRemoved(const QString& entityName, const QString& boneName);
    void boneRenamed(const QString& entityName, const QString& oldName, const QString& newName);
    void boneDuplicated(const QString& entityName, const QString& boneName);
    void skeletonStructureChanged();
    void boneContextMenuRequested(int globalX, int globalY);
    void editRestPoseModeChanged();
    void showRestPoseGhostChanged();
    void restPoseChanged();

private:
    explicit SkeletonEditor(QObject* parent = nullptr);

    static unsigned short nextBoneHandle(const Ogre::Skeleton* skel);
    static Ogre::Vector3 defaultChildLocalPosition(const Ogre::Bone* parent);
    static bool rebuildSkeletonWithoutBones(Ogre::Entity* entity,
                                            const std::vector<std::string>& removeNames,
                                            bool removeChildren,
                                            bool transferWeightsToParent,
                                            QString* error);
    static bool ensureEntitySkeleton(Ogre::Entity* entity, QString* error);

    void applyEditRestAnimMute(bool mute);
    void syncRestPoseGhostOverlay();
    void ensureEditRestSelectionHook();

    static SkeletonEditor* s_singleton;
    bool m_editRestPoseMode = false;
    bool m_showRestPoseGhost = false;
    bool m_editRestSelectionHooked = false;
    QString m_editRestMutedEntity;
    QStringList m_editRestMutedAnims;
};
