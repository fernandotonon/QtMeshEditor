#include "AddPrintPegsCommand.h"

#include "Manager.h"
#include "PartOpsMesh.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <OgreEntity.h>
#include <OgreSceneNode.h>
#include <OgreSceneManager.h>

AddPrintPegsCommand::AddPrintPegsCommand(std::string entityName,
                                         SubMeshOps::PegOptions opts, QUndoCommand* parent)
    : QUndoCommand(parent)
    , mEntityName(std::move(entityName))
    , mOpts(opts)
{
    setText(QStringLiteral("Add Print Alignment Pegs"));
}

Ogre::Entity* AddPrintPegsCommand::resolveEntity() const
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr)
        return nullptr;
    for (Ogre::Entity* e : mgr->getEntities()) {
        if (e && e->getMovableType() == "Entity" && e->getName() == mEntityName)
            return e;
    }
    return nullptr;
}

Ogre::Entity* AddPrintPegsCommand::swapEntityMesh(const Ogre::MeshPtr& mesh)
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr || !mesh)
        return nullptr;
    Ogre::Entity* cur = resolveEntity();
    if (!cur)
        return nullptr;
    Ogre::SceneNode* node = cur->getParentSceneNode();
    if (!node)
        return nullptr;

    // Drop every selection reference before freeing the entity (SplitMeshCommand
    // rationale: dangling sub-entity refs crash the next selection query).
    if (auto* sel = SelectionSet::getSingleton()) {
        mReselectNode = sel->contains(node) ? node : nullptr;
        sel->clearList();
    }
    node->detachObject(cur);
    mgr->getSceneMgr()->destroyEntity(cur);
    Ogre::Entity* ne = mgr->createEntity(node, mesh);
    if (ne && mReselectNode) {
        if (auto* sel = SelectionSet::getSingleton())
            sel->selectOne(node);
    }
    return ne;
}

void AddPrintPegsCommand::redo()
{
    if (!mBuilt) {
        mBuilt = true;
        Ogre::Entity* entity = resolveEntity();
        if (!entity || !entity->getMesh()) {
            mError = QStringLiteral("no entity to prep");
            return;
        }
        mOriginalMesh = entity->getMesh();   // resident for undo.

        PartOpsMesh::PrintPrepOutcome po =
            PartOpsMesh::addPrintPegsToEntity(entity, mOpts,
                                              mEntityName + std::string("_pegged"));
        if (!po.ok) {
            mError = po.error.isEmpty() ? QStringLiteral("print prep failed") : po.error;
            return;
        }
        mPeggedMesh = po.mesh;
        mPeggedBoundaries = po.peggedBoundaries;
        mTotalPegs = po.totalPegs;
        mWarnings = po.warnings;
    }

    if (!mPeggedMesh) {
        mOk = false;
        return; // build failed on first redo; mError set.
    }
    Ogre::Entity* ne = swapEntityMesh(mPeggedMesh);
    mOk = (ne != nullptr);
    if (mOk)
        SentryReporter::addBreadcrumb(QStringLiteral("mesh.parts.print_pegs"),
                                      QStringLiteral("boundaries=%1 pegs=%2")
                                          .arg(mPeggedBoundaries).arg(mTotalPegs));
    else if (mError.isEmpty())
        mError = QStringLiteral("failed to swap in pegged mesh");
}

void AddPrintPegsCommand::undo()
{
    if (!mOriginalMesh)
        return;
    swapEntityMesh(mOriginalMesh);
}
