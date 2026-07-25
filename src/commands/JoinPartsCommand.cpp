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
            // Reject a part with child nodes: destroying it recursively removes
            // a subtree this command doesn't serialise, so undo can't restore it.
            if (node->numChildren() > 0) {
                mError = QStringLiteral("part '%1' has child nodes — ungroup or "
                                        "detach children before joining")
                             .arg(QString::fromStdString(n));
                mSources.clear();
                return;
            }
            sp.pos = node->getPosition();
            sp.orient = node->getOrientation();
            sp.scale = node->getScale();
            // Remember the part's group so undo restores it there.
            Ogre::SceneNode* parent = static_cast<Ogre::SceneNode*>(node->getParent());
            if (parent && parent != mgr->getSceneMgr()->getRootSceneNode())
                sp.parentName = parent->getName();
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

    // CREATE-THEN-DESTROY: create the fused node FIRST (its name
    // "<part0>_fused" doesn't collide with the source part names, so it can
    // coexist), and only destroy the source parts once it exists. If it can't be
    // created, leave the parts in place — the scene is never left empty
    // (CodeRabbit). The fused node sits at the ORIGIN: join baked the parts'
    // world transforms into the vertex positions, so an identity node reproduces
    // the assembled pose.
    Ogre::SceneNode* fused = mgr->addSceneNode(mFusedNameBase);
    if (!fused) {
        mOk = false;
        mError = QStringLiteral("failed to create joined node");
        return;
    }
    if (!mgr->createEntity(fused, mFusedMesh)) {
        mgr->destroySceneNode(fused, /*destroyChildrenFirst=*/true);
        mOk = false;
        mError = QStringLiteral("failed to create joined entity");
        return;
    }
    mFusedNodeName = fused->getName();

    // Fused node is live — now drop selection refs and destroy the source parts
    // (SplitMeshCommand rationale: dangling sub-entity refs crash the next
    // selection query).
    if (auto* sel = SelectionSet::getSingleton())
        sel->clearList();
    for (const auto& sp : mSources) {
        if (Ogre::SceneNode* node = mgr->getSceneNode(QString::fromStdString(sp.name)))
            mgr->destroySceneNode(node, /*destroyChildrenFirst=*/true);
    }

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

    // CREATE-THEN-DESTROY: recreate every part node FIRST (their names don't
    // collide with the fused "<part0>_fused" node), then destroy the fused node
    // once the parts are back. addSceneNode creates at root; reparentNode
    // preserves WORLD transform, so set the local TRS AFTER the reparent to
    // restore the exact pose relative to the group. Undo never leaves the scene
    // empty (CodeRabbit).
    Ogre::SceneNode* last = nullptr;
    for (const auto& sp : mSources) {
        Ogre::SceneNode* node = mgr->addSceneNode(QString::fromStdString(sp.name));
        if (!node)
            continue;
        if (!sp.parentName.empty()) {
            if (Ogre::SceneNode* parent =
                    mgr->getSceneNode(QString::fromStdString(sp.parentName)))
                mgr->reparentNode(node, parent);
        }
        node->setPosition(sp.pos);
        node->setOrientation(sp.orient);
        node->setScale(sp.scale);
        mgr->createEntity(node, sp.mesh);
        last = node;
    }

    // Parts are back — now destroy the fused node.
    if (!mFusedNodeName.empty()) {
        if (Ogre::SceneNode* fused = mgr->getSceneNode(QString::fromStdString(mFusedNodeName)))
            mgr->destroySceneNode(fused, /*destroyChildrenFirst=*/true);
        mFusedNodeName.clear();
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
