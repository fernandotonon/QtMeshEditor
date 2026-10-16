#include "ReplaceEntityMeshCommand.h"

#include "Manager.h"
#include "MeshImporterExporter.h"
#include "SelectionSet.h"

#include <OgreEntity.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

ReplaceEntityMeshCommand::ReplaceEntityMeshCommand(std::string entityName, Ogre::MeshPtr replacement,
                                                   const QString& text, QUndoCommand* parent)
    : QUndoCommand(parent)
    , mEntityName(std::move(entityName))
    , mReplacement(std::move(replacement))
{
    setText(text);
}

Ogre::Entity* ReplaceEntityMeshCommand::resolveEntity() const
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    for (Ogre::Entity* e : mgr->getEntities())
        if (e && e->getMovableType() == "Entity" && e->getName() == mEntityName)
            return e;
    return nullptr;
}

Ogre::Entity* ReplaceEntityMeshCommand::swapTo(const Ogre::MeshPtr& mesh)
{
    Manager* mgr = Manager::getSingletonPtr();
    Ogre::Entity* cur = resolveEntity();
    if (!mgr || !mesh || !cur) return nullptr;
    Ogre::SceneNode* node = cur->getParentSceneNode();
    if (!node) return nullptr;
    if (!mOriginal) mOriginal = cur->getMesh();   // first redo: remember the source

    // Drop EVERY selection reference before the entity (and its sub-entities)
    // is freed — see SplitMeshCommand::swapEntityMesh for the crash this avoids.
    bool reselect = false;
    if (auto* sel = SelectionSet::getSingleton()) {
        reselect = sel->contains(node);
        sel->clearList();
    }
    node->detachObject(cur);
    mgr->getSceneMgr()->destroyEntity(cur);
    Ogre::Entity* ne = mgr->createEntity(node, mesh);
    if (ne) {
        // Tangents + RTSS normal mapping when the material carries a normal
        // map — the importer's recipe (without tangents N·L collapses).
        MeshImporterExporter::applyNormalMapsToEntity(ne);
        if (reselect)
            if (auto* sel = SelectionSet::getSingleton()) sel->selectOne(node);
    }
    return ne;
}

void ReplaceEntityMeshCommand::redo()
{
    mOk = swapTo(mReplacement) != nullptr;
}

void ReplaceEntityMeshCommand::undo()
{
    if (mOriginal) swapTo(mOriginal);
}
