#include "JoinPartsCommand.h"

#include "Manager.h"
#include "PartOpsScene.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <OgreEntity.h>
#include <OgreSceneNode.h>
#include <OgreSceneManager.h>

JoinPartsCommand::JoinPartsCommand(std::vector<std::string> entityNames, QString fusedName,
                                   QUndoCommand* parent)
    : QUndoCommand(parent)
    , mEntityNames(std::move(entityNames))
    , mFusedNameBase(std::move(fusedName))
{
    setText(QStringLiteral("Join Parts"));
}

namespace {
Ogre::Entity* findEntity(Manager* mgr, const std::string& name)
{
    if (!mgr)
        return nullptr;
    for (Ogre::Entity* e : mgr->getEntities()) {
        if (e && e->getMovableType() == "Entity" && e->getName() == name)
            return e;
    }
    return nullptr;
}
} // namespace

void JoinPartsCommand::buildOnce()
{
    if (mBuilt)
        return;
    mBuilt = true;

    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) {
        mError = QStringLiteral("no scene manager");
        return;
    }

    // Resolve the entities and capture each one's original mesh + node transform
    // (needed to recreate the parts on undo).
    std::vector<Ogre::Entity*> entities;
    for (const auto& n : mEntityNames) {
        Ogre::Entity* e = findEntity(mgr, n);
        if (!e || !e->getMesh()) {
            mError = QStringLiteral("part '%1' not found").arg(QString::fromStdString(n));
            return;
        }
        SourcePart sp;
        sp.name = n;
        sp.mesh = e->getMesh();
        if (Ogre::SceneNode* node = e->getParentSceneNode()) {
            sp.pos = node->getPosition();
            sp.orient = node->getOrientation();
            sp.scale = node->getScale();
        }
        mSources.push_back(std::move(sp));
        entities.push_back(e);
    }

    PartOpsScene::JoinResult r = PartOpsScene::joinEntities(
        entities, mFusedNameBase.toStdString() + std::string("_joined"));
    if (!r.ok) {
        mError = r.error.isEmpty() ? QStringLiteral("join failed") : r.error;
        mSources.clear();
        return;
    }
    mFusedMesh = r.mesh;
    mCreatedSubMeshes = r.createdSubMeshes;
}

void JoinPartsCommand::redo()
{
    buildOnce();
    if (!mFusedMesh) {
        mOk = false;
        return; // build failed; mError set.
    }

    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) {
        mOk = false;
        return;
    }

    // Drop selection refs before destroying source entities (SplitMeshCommand
    // rationale: dangling sub-entity refs crash the next selection query).
    if (auto* sel = SelectionSet::getSingleton())
        sel->clearList();

    // Destroy the source part nodes (frees their names for undo to recreate).
    for (const auto& sp : mSources) {
        if (Ogre::SceneNode* node = mgr->getSceneNode(QString::fromStdString(sp.name)))
            mgr->destroySceneNode(node, /*destroyChildrenFirst=*/true);
    }

    // Create the fused node at the ORIGIN — join baked world transforms into the
    // vertex positions, so an identity node reproduces the assembled pose.
    Ogre::SceneNode* fused = mgr->addSceneNode(mFusedNameBase);
    if (!fused) {
        mOk = false;
        mError = QStringLiteral("failed to create joined node");
        return;
    }
    mFusedNodeName = fused->getName();
    mgr->createEntity(fused, mFusedMesh);

    if (auto* sel = SelectionSet::getSingleton())
        sel->selectOne(fused);

    mOk = true;
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.parts.join"),
                                  QStringLiteral("parts=%1 submeshes=%2")
                                      .arg(mSources.size()).arg(mCreatedSubMeshes));
}

void JoinPartsCommand::undo()
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr)
        return;

    if (auto* sel = SelectionSet::getSingleton())
        sel->clearList();

    // Destroy the fused node.
    if (!mFusedNodeName.empty()) {
        if (Ogre::SceneNode* fused = mgr->getSceneNode(QString::fromStdString(mFusedNodeName)))
            mgr->destroySceneNode(fused, /*destroyChildrenFirst=*/true);
        mFusedNodeName.clear();
    }

    // Recreate each part node with its captured transform + original mesh.
    Ogre::SceneNode* last = nullptr;
    for (const auto& sp : mSources) {
        Ogre::SceneNode* node = mgr->addSceneNode(QString::fromStdString(sp.name));
        if (!node)
            continue;
        node->setPosition(sp.pos);
        node->setOrientation(sp.orient);
        node->setScale(sp.scale);
        mgr->createEntity(node, sp.mesh);
        last = node;
    }

    // Reselect the restored parts.
    if (auto* sel = SelectionSet::getSingleton()) {
        sel->clearList();
        for (const auto& sp : mSources) {
            if (Ogre::SceneNode* node = mgr->getSceneNode(QString::fromStdString(sp.name)))
                sel->append(node);
        }
        if (mSources.size() == 1 && last)
            sel->selectOne(last);
    }
}
