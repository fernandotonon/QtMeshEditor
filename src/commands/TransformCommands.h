#ifndef TRANSFORM_COMMANDS_H
#define TRANSFORM_COMMANDS_H

#include <QUndoCommand>
#include <QList>
#include <vector>
#include <OgreVector.h>
#include <OgreQuaternion.h>

namespace Ogre {
    class SceneNode;
    class Entity;
    class SubEntity;
}

// Translate scene nodes by a delta
class TranslateCommand : public QUndoCommand
{
public:
    TranslateCommand(const QList<Ogre::SceneNode*>& nodes,
                     const Ogre::Vector3& delta,
                     QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    QList<Ogre::SceneNode*> mNodes;
    Ogre::Vector3 mDelta;
};

// Rotate scene nodes by a quaternion around a pivot
class RotateCommand : public QUndoCommand
{
public:
    RotateCommand(const QList<Ogre::SceneNode*>& nodes,
                  const Ogre::Quaternion& rotation,
                  const Ogre::Vector3& pivot,
                  QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    QList<Ogre::SceneNode*> mNodes;
    Ogre::Quaternion mRotation;
    Ogre::Vector3 mPivot;
    // Store per-node positions before rotation for undo
    QList<Ogre::Vector3> mOriginalPositions;
    QList<Ogre::Quaternion> mOriginalOrientations;
};

// Scale scene nodes by a factor
class ScaleCommand : public QUndoCommand
{
public:
    ScaleCommand(const QList<Ogre::SceneNode*>& nodes,
                 const Ogre::Vector3& scaleFactor,
                 QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    QList<Ogre::SceneNode*> mNodes;
    Ogre::Vector3 mScaleFactor;
};

// Delete scene nodes (stores enough info to recreate on undo)
class DeleteCommand : public QUndoCommand
{
public:
    DeleteCommand(const QList<Ogre::SceneNode*>& nodes,
                  QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    struct NodeSnapshot {
        Ogre::SceneNode* node = nullptr;
        Ogre::Vector3 position;
        Ogre::Quaternion orientation;
        Ogre::Vector3 scale;
        bool wasVisible = true;
    };
    QList<NodeSnapshot> mSnapshots;
    bool mFirstRedo = true;
};

// Duplicate scene nodes (destroys clones on undo, re-duplicates on redo)
class DuplicateCommand : public QUndoCommand
{
public:
    DuplicateCommand(const QList<Ogre::SceneNode*>& sourceNodes,
                     const QList<Ogre::SceneNode*>& clonedNodes,
                     QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    QStringList mSourceNodeNames;
    QList<Ogre::SceneNode*> mClonedNodes;
    bool mFirstRedo = true;
};

// Group selected nodes under a new parent node
class GroupCommand : public QUndoCommand
{
public:
    GroupCommand(const QList<Ogre::SceneNode*>& nodes,
                 QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    struct NodeParentInfo {
        std::string nodeName;
        std::string oldParentName;  // empty string means root scene node
        Ogre::Vector3 oldPosition;
        Ogre::Quaternion oldOrientation;
        Ogre::Vector3 oldScale;
    };
    QList<NodeParentInfo> mNodeInfos;
    std::string mGroupNodeName;
    Ogre::Vector3 mGroupPosition;
    bool mFirstRedo = true;
};

// Ungroup: move children to group's parent, destroy group node
class UngroupCommand : public QUndoCommand
{
public:
    UngroupCommand(Ogre::SceneNode* groupNode,
                   QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    struct ChildInfo {
        std::string childName;
        Ogre::Vector3 localPosition;
        Ogre::Quaternion localOrientation;
        Ogre::Vector3 localScale;
    };
    std::string mGroupNodeName;
    std::string mGroupParentName;  // empty string means root scene node
    Ogre::Vector3 mGroupPosition;
    Ogre::Quaternion mGroupOrientation;
    Ogre::Vector3 mGroupScale;
    QList<ChildInfo> mChildInfos;
    bool mFirstRedo = true;
};

// Sub-mesh vertex transform (stores full vertex snapshot for undo)
class SubMeshTransformCommand : public QUndoCommand
{
public:
    enum TransformType { Translate, Rotate, Scale };

    SubMeshTransformCommand(Ogre::SubEntity* subEntity,
                            const std::vector<Ogre::Vector3>& originalPositions,
                            const QString& description = "SubMesh Transform",
                            QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    Ogre::SubEntity* mSubEntity;
    Ogre::Entity* mEntity;
    unsigned int mSubMeshIndex;
    std::vector<Ogre::Vector3> mOriginalPositions;
    std::vector<Ogre::Vector3> mNewPositions;
    bool mFirstRedo = true;
};

#endif // TRANSFORM_COMMANDS_H
