#pragma once

#include <OgreAnimation.h>
#include <OgreMesh.h>
#include <OgreQuaternion.h>
#include <OgreVector3.h>

#include <QObject>
#include <QQmlEngine>
#include <QString>

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

/// Bone-level CRUD for skeletal rigs (epic #554, slice A #555).
/// Owns create / remove / rename / duplicate on the selected entity's
/// skeleton resource, with undo via SkeletonBoneCommands.
class SkeletonEditor : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

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

    struct Result {
        bool ok = false;
        QString error;
        QString boneName; ///< created / duplicated bone name
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
    static Result renameBone(Ogre::Entity* entity, const QString& oldName, const QString& newName);
    static Result duplicateBone(Ogre::Entity* entity, const QString& sourceBoneName);

    static void refreshAfterEdit(const std::string& entityName, const QString& selectBone = {});

    /// Push undo commands — used by QML and tests.
    Q_INVOKABLE bool createBoneForSelected(const QString& parentBoneName = {});
    Q_INVOKABLE bool removeSelectedBone(bool removeChildren, bool transferWeightsToParent);
    Q_INVOKABLE bool renameSelectedBone(const QString& newName);
    Q_INVOKABLE bool duplicateSelectedBone();

    Q_INVOKABLE bool hasSkeletonSelection() const;
    Q_INVOKABLE QString selectedBoneName() const;

signals:
    void boneCreated(const QString& entityName, const QString& boneName);
    void boneRemoved(const QString& entityName, const QString& boneName);
    void boneRenamed(const QString& entityName, const QString& oldName, const QString& newName);
    void boneDuplicated(const QString& entityName, const QString& boneName);
    void skeletonStructureChanged();

private:
    explicit SkeletonEditor(QObject* parent = nullptr);

    static unsigned short nextBoneHandle(const Ogre::Skeleton* skel);
    static Ogre::Vector3 defaultChildLocalPosition(const Ogre::Bone* parent);
    static bool rebuildSkeletonWithoutBones(Ogre::Entity* entity,
                                            const std::vector<std::string>& removeNames,
                                            bool removeChildren,
                                            bool transferWeightsToParent,
                                            QString* error);

    static SkeletonEditor* s_singleton;
};
