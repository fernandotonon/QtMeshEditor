#include "TransformCommands.h"
#include "../Manager.h"
#include "../SelectionSet.h"
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
