#include "TransformCommands.h"
#include "../Manager.h"
#include "../SelectionSet.h"
#include "../SubMeshTransform.h"
#include <Ogre.h>

// Check if a scene node pointer is still valid (not destroyed)
static bool isNodeValid(Ogre::SceneNode* node)
{
    if (!node) return false;
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return false;
    for (auto* n : mgr->getSceneNodes())
        if (n == node) return true;
    return false;
}

// ---- TranslateCommand ----

TranslateCommand::TranslateCommand(const QList<Ogre::SceneNode*>& nodes,
                                   const Ogre::Vector3& delta,
                                   QUndoCommand* parent)
    : QUndoCommand("Translate", parent), mNodes(nodes), mDelta(delta)
{
}

void TranslateCommand::undo()
{
    for (Ogre::SceneNode* node : mNodes)
        if (isNodeValid(node)) node->translate(-mDelta);
}

void TranslateCommand::redo()
{
    for (Ogre::SceneNode* node : mNodes)
        if (isNodeValid(node)) node->translate(mDelta);
}

// ---- RotateCommand ----

RotateCommand::RotateCommand(const QList<Ogre::SceneNode*>& nodes,
                             const Ogre::Quaternion& rotation,
                             const Ogre::Vector3& pivot,
                             QUndoCommand* parent)
    : QUndoCommand("Rotate", parent), mNodes(nodes), mRotation(rotation), mPivot(pivot)
{
    for (Ogre::SceneNode* node : mNodes)
    {
        mOriginalPositions.append(node->getPosition());
        mOriginalOrientations.append(node->getOrientation());
    }
}

void RotateCommand::undo()
{
    for (int i = 0; i < mNodes.size(); ++i)
        if (isNodeValid(mNodes[i]))
        {
            mNodes[i]->setPosition(mOriginalPositions[i]);
            mNodes[i]->setOrientation(mOriginalOrientations[i]);
        }
}

void RotateCommand::redo()
{
    for (Ogre::SceneNode* node : mNodes)
        if (isNodeValid(node))
        {
            Ogre::Vector3 offset = node->_getDerivedPosition() - mPivot;
            node->setPosition(mPivot);
            node->rotate(mRotation, Ogre::Node::TS_WORLD);
            node->setPosition(node->getPosition() + mRotation * offset);
        }
}

// ---- ScaleCommand ----

ScaleCommand::ScaleCommand(const QList<Ogre::SceneNode*>& nodes,
                           const Ogre::Vector3& scaleFactor,
                           QUndoCommand* parent)
    : QUndoCommand("Scale", parent), mNodes(nodes), mScaleFactor(scaleFactor)
{
}

void ScaleCommand::undo()
{
    Ogre::Vector3 inverse(1.0f / mScaleFactor.x,
                          1.0f / mScaleFactor.y,
                          1.0f / mScaleFactor.z);
    for (Ogre::SceneNode* node : mNodes)
        if (isNodeValid(node)) node->scale(inverse);
}

void ScaleCommand::redo()
{
    for (Ogre::SceneNode* node : mNodes)
        if (isNodeValid(node)) node->scale(mScaleFactor);
}

// ---- DeleteCommand ----

DeleteCommand::DeleteCommand(const QList<Ogre::SceneNode*>& nodes,
                             QUndoCommand* parent)
    : QUndoCommand("Delete", parent), mFirstRedo(true)
{
    for (Ogre::SceneNode* node : nodes)
    {
        NodeSnapshot snap;
        snap.node = node;
        snap.position = node->getPosition();
        snap.orientation = node->getOrientation();
        snap.scale = node->getScale();
        snap.wasVisible = (node->numAttachedObjects() == 0)
            || node->getAttachedObject(0)->getVisible();
        mSnapshots.append(snap);
    }
}

void DeleteCommand::undo()
{
    // Re-show the hidden nodes
    for (auto& snap : mSnapshots)
    {
        if (snap.node)
        {
            snap.node->setVisible(snap.wasVisible, true);
            snap.node->setPosition(snap.position);
            snap.node->setOrientation(snap.orientation);
            snap.node->setScale(snap.scale);
        }
    }
}

void DeleteCommand::redo()
{
    if (mFirstRedo)
    {
        // First redo is the initial deletion — handled by caller
        mFirstRedo = false;
        return;
    }
    // Hide nodes instead of destroying (allows undo)
    for (auto& snap : mSnapshots)
    {
        if (snap.node)
            snap.node->setVisible(false, true);
    }
}

// ---- DuplicateCommand ----

DuplicateCommand::DuplicateCommand(const QList<Ogre::SceneNode*>& sourceNodes,
                                   const QList<Ogre::SceneNode*>& clonedNodes,
                                   QUndoCommand* parent)
    : QUndoCommand("Duplicate", parent), mClonedNodes(clonedNodes), mFirstRedo(true)
{
    // Store source node names so we can re-duplicate on redo
    for (Ogre::SceneNode* src : sourceNodes)
        mSourceNodeNames << QString::fromStdString(src->getName());
}

void DuplicateCommand::undo()
{
    // Destroy the cloned nodes (same as removeSelected pattern)
    for (Ogre::SceneNode* node : mClonedNodes) {
        if (node)
            Manager::getSingleton()->destroySceneNode(node);
    }
    mClonedNodes.clear();
    SelectionSet::getSingleton()->clearList();
}

void DuplicateCommand::redo()
{
    if (mFirstRedo) {
        mFirstRedo = false;
        return;
    }
    // Re-duplicate from the source nodes
    mClonedNodes.clear();
    for (const QString& name : mSourceNodeNames) {
        if (!Manager::getSingleton()->hasSceneNode(name)) continue;
        Ogre::SceneNode* src = Manager::getSingleton()->getSceneMgr()
            ->getSceneNode(name.toStdString());
        Ogre::SceneNode* clone = Manager::getSingleton()->duplicateSceneNode(src);
        if (clone) mClonedNodes.append(clone);
    }
    // Select the new clones
    SelectionSet* sel = SelectionSet::getSingleton();
    sel->clearList();
    for (Ogre::SceneNode* clone : mClonedNodes)
        sel->append(clone);
}

// ---- GroupCommand ----

GroupCommand::GroupCommand(const QList<Ogre::SceneNode*>& nodes,
                           QUndoCommand* parent)
    : QUndoCommand("Group", parent), mFirstRedo(true)
{
    // Compute centroid for group position
    Ogre::Vector3 centroid = Ogre::Vector3::ZERO;
    for (Ogre::SceneNode* node : nodes)
        centroid += node->_getDerivedPosition();
    centroid /= static_cast<Ogre::Real>(nodes.size());
    mGroupPosition = centroid;

    // Store original parent info for each node
    for (Ogre::SceneNode* node : nodes)
    {
        NodeParentInfo info;
        info.nodeName = node->getName();
        Ogre::Node* p = node->getParent();
        auto* mgr = Manager::getSingleton()->getSceneMgr();
        info.oldParentName = (p && p != mgr->getRootSceneNode()) ? p->getName() : "";
        info.oldPosition = node->getPosition();
        info.oldOrientation = node->getOrientation();
        info.oldScale = node->getScale();
        mNodeInfos.append(info);
    }
}

void GroupCommand::undo()
{
    auto* mgr = Manager::getSingleton();
    auto* sceneMgr = mgr->getSceneMgr();
    if (!sceneMgr) return;

    // Find the group node
    if (!sceneMgr->hasSceneNode(mGroupNodeName)) return;
    Ogre::SceneNode* groupNode = sceneMgr->getSceneNode(mGroupNodeName);

    // Reparent children back to original parents with original local transforms
    for (const auto& info : mNodeInfos) {
        if (!sceneMgr->hasSceneNode(info.nodeName)) continue;
        Ogre::SceneNode* child = sceneMgr->getSceneNode(info.nodeName);

        groupNode->removeChild(child);

        Ogre::SceneNode* oldParent = info.oldParentName.empty()
            ? sceneMgr->getRootSceneNode()
            : sceneMgr->getSceneNode(info.oldParentName);
        oldParent->addChild(child);

        child->setPosition(info.oldPosition);
        child->setOrientation(info.oldOrientation);
        child->setScale(info.oldScale);
    }

    // Destroy the group node
    emit mgr->sceneNodeDestroyed(groupNode);
    mgr->destroyAllAttachedMovableObjects(groupNode);
    sceneMgr->destroySceneNode(groupNode);
}

void GroupCommand::redo()
{
    if (mFirstRedo) {
        // First redo is the initial grouping — done by caller (Manager::groupNodes)
        // We just need to capture the group node name
        auto* mgr = Manager::getSingleton();
        QList<Ogre::SceneNode*> nodes;
        for (const auto& info : mNodeInfos) {
            if (mgr->getSceneMgr()->hasSceneNode(info.nodeName))
                nodes.append(mgr->getSceneMgr()->getSceneNode(info.nodeName));
        }
        // The group node was created by the caller before pushing this command
        // Find it by looking for the parent of the first node
        if (!nodes.isEmpty()) {
            Ogre::SceneNode* parent = static_cast<Ogre::SceneNode*>(nodes.first()->getParent());
            if (parent && parent != mgr->getSceneMgr()->getRootSceneNode())
                mGroupNodeName = parent->getName();
        }
        mFirstRedo = false;
        return;
    }

    // Re-do: recreate group and reparent
    auto* mgr = Manager::getSingleton();
    auto* sceneMgr = mgr->getSceneMgr();
    if (!sceneMgr) return;

    // Recreate the group node
    Ogre::SceneNode* groupNode = sceneMgr->getRootSceneNode()->createChildSceneNode(mGroupNodeName);
    groupNode->setPosition(mGroupPosition);

    for (const auto& info : mNodeInfos) {
        if (!sceneMgr->hasSceneNode(info.nodeName)) continue;
        Ogre::SceneNode* child = sceneMgr->getSceneNode(info.nodeName);

        // Save world transform
        Ogre::Vector3 worldPos = child->_getDerivedPosition();
        Ogre::Quaternion worldOrient = child->_getDerivedOrientation();
        Ogre::Vector3 worldScale = child->_getDerivedScale();

        // Reparent
        Ogre::SceneNode* oldParent = static_cast<Ogre::SceneNode*>(child->getParent());
        if (oldParent) oldParent->removeChild(child);
        groupNode->addChild(child);

        // Restore world transform as local transform under group
        Ogre::Quaternion groupWorldOrient = groupNode->_getDerivedOrientation();
        Ogre::Vector3 groupWorldScale = groupNode->_getDerivedScale();
        Ogre::Vector3 groupDerivedPos = groupNode->_getDerivedPosition();

        child->setOrientation(groupWorldOrient.Inverse() * worldOrient);
        child->setScale(worldScale / groupWorldScale);
        child->setPosition(groupWorldOrient.Inverse() *
            ((worldPos - groupDerivedPos) / groupWorldScale));
    }

    emit mgr->sceneNodeCreated(groupNode);
    SelectionSet::getSingleton()->selectOne(groupNode);
}

// ---- UngroupCommand ----

UngroupCommand::UngroupCommand(Ogre::SceneNode* groupNode,
                                QUndoCommand* parent)
    : QUndoCommand("Ungroup", parent), mFirstRedo(true)
{
    mGroupNodeName = groupNode->getName();
    mGroupPosition = groupNode->getPosition();
    mGroupOrientation = groupNode->getOrientation();
    mGroupScale = groupNode->getScale();

    Ogre::Node* p = groupNode->getParent();
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    mGroupParentName = (p && p != sceneMgr->getRootSceneNode()) ? p->getName() : "";

    // Capture children info (local transforms relative to group)
    for (auto& child : groupNode->getChildren()) {
        Ogre::SceneNode* childNode = static_cast<Ogre::SceneNode*>(child);
        if (Manager::getSingleton()->isForbiddenNodeName(QString::fromStdString(childNode->getName())))
            continue;
        ChildInfo ci;
        ci.childName = childNode->getName();
        ci.localPosition = childNode->getPosition();
        ci.localOrientation = childNode->getOrientation();
        ci.localScale = childNode->getScale();
        mChildInfos.append(ci);
    }
}

void UngroupCommand::undo()
{
    // Re-create the group node and reparent children back
    auto* mgr = Manager::getSingleton();
    auto* sceneMgr = mgr->getSceneMgr();
    if (!sceneMgr) return;

    Ogre::SceneNode* parentNode = mGroupParentName.empty()
        ? sceneMgr->getRootSceneNode()
        : sceneMgr->getSceneNode(mGroupParentName);

    Ogre::SceneNode* groupNode = parentNode->createChildSceneNode(mGroupNodeName);
    groupNode->setPosition(mGroupPosition);
    groupNode->setOrientation(mGroupOrientation);
    groupNode->setScale(mGroupScale);

    for (const auto& ci : mChildInfos) {
        if (!sceneMgr->hasSceneNode(ci.childName)) continue;
        Ogre::SceneNode* child = sceneMgr->getSceneNode(ci.childName);

        Ogre::SceneNode* oldParent = static_cast<Ogre::SceneNode*>(child->getParent());
        if (oldParent) oldParent->removeChild(child);
        groupNode->addChild(child);

        // Restore original local transforms relative to group
        child->setPosition(ci.localPosition);
        child->setOrientation(ci.localOrientation);
        child->setScale(ci.localScale);
    }

    emit mgr->sceneNodeCreated(groupNode);
    SelectionSet::getSingleton()->selectOne(groupNode);
}

void UngroupCommand::redo()
{
    if (mFirstRedo) {
        // First redo is the initial ungrouping — done by caller (Manager::ungroupNode)
        mFirstRedo = false;
        return;
    }

    // Re-ungroup
    auto* mgr = Manager::getSingleton();
    auto* sceneMgr = mgr->getSceneMgr();
    if (!sceneMgr || !sceneMgr->hasSceneNode(mGroupNodeName)) return;

    Ogre::SceneNode* groupNode = sceneMgr->getSceneNode(mGroupNodeName);
    mgr->ungroupNode(groupNode);
}

// ---- SubMeshTransformCommand ----

SubMeshTransformCommand::SubMeshTransformCommand(Ogre::SubEntity* subEntity,
                                                   const std::vector<Ogre::Vector3>& originalPositions,
                                                   const QString& description,
                                                   QUndoCommand* parent)
    : QUndoCommand(description, parent)
    , mSubEntity(subEntity)
    , mEntity(subEntity ? subEntity->getParent() : nullptr)
    , mSubMeshIndex(0)
    , mOriginalPositions(originalPositions)
    , mFirstRedo(true)
{
    // Determine the sub-mesh index within the parent entity
    if (mEntity)
    {
        for (unsigned int i = 0; i < mEntity->getNumSubEntities(); ++i)
        {
            if (mEntity->getSubEntity(i) == mSubEntity)
            {
                mSubMeshIndex = i;
                break;
            }
        }
    }
}

void SubMeshTransformCommand::undo()
{
    if (mEntity)
    {
        SubMeshTransform::writePositions(mEntity, mSubMeshIndex, mOriginalPositions);
    }
}

void SubMeshTransformCommand::redo()
{
    if (mFirstRedo)
    {
        // Capture the current (post-transform) positions on first redo
        if (mEntity)
            mNewPositions = SubMeshTransform::readPositions(mEntity, mSubMeshIndex);
        mFirstRedo = false;
        return;
    }

    if (mEntity)
    {
        SubMeshTransform::writePositions(mEntity, mSubMeshIndex, mNewPositions);
    }
}
