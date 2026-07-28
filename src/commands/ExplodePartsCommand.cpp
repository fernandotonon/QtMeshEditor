#include "ExplodePartsCommand.h"

#include "Manager.h"
#include "PartOpsScene.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <OgreEntity.h>
#include <OgreSceneNode.h>
#include <OgreSceneManager.h>

namespace {
// Reparent `node` under the saved parent name (no-op when empty == scene root),
// then stamp the given LOCAL transform so it sits correctly relative to that
// parent. Manager::reparentNode preserves WORLD transform, which we don't want
// here — we want the source's original local pose under its group — so we set
// the local TRS explicitly afterward.
void reparentAndSetLocal(Manager* mgr, Ogre::SceneNode* node,
                         const std::string& parentName,
                         const Ogre::Vector3& pos, const Ogre::Quaternion& orient,
                         const Ogre::Vector3& scale)
{
    if (!mgr || !node)
        return;
    if (!parentName.empty()) {
        if (Ogre::SceneNode* parent = mgr->getSceneNode(QString::fromStdString(parentName)))
            mgr->reparentNode(node, parent);
    }
    node->setPosition(pos);
    node->setOrientation(orient);
    node->setScale(scale);
}
} // namespace

ExplodePartsCommand::ExplodePartsCommand(std::string entityName, float distance,
                                         bool capBoundaries, QUndoCommand* parent)
    : QUndoCommand(parent)
    , mEntityName(std::move(entityName))
    , mDistance(distance)
    , mCapBoundaries(capBoundaries)
{
    setText(QStringLiteral("Explode into Parts"));
}

Ogre::Entity* ExplodePartsCommand::resolveSourceEntity() const
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

void ExplodePartsCommand::buildOnce()
{
    if (mBuilt)
        return;
    mBuilt = true;

    Ogre::Entity* entity = resolveSourceEntity();
    if (!entity || !entity->getMesh()) {
        mError = QStringLiteral("no entity to explode");
        return;
    }
    Ogre::SceneNode* node = entity->getParentSceneNode();
    if (!node) {
        mError = QStringLiteral("entity has no scene node");
        return;
    }
    // Reject a node with child nodes: destroying it would recursively delete an
    // arbitrary subtree that this command doesn't serialise, so undo could never
    // restore it. (A node in a GROUP is fine — its parent is preserved below.)
    if (node->numChildren() > 0) {
        mError = QStringLiteral("cannot explode a node that has child nodes — "
                                "ungroup or detach children first");
        return;
    }
    mOriginalMesh = entity->getMesh(); // resident for undo.
    mSrcPos = node->getPosition();
    mSrcOrient = node->getOrientation();
    mSrcScale = node->getScale();
    // Preserve the source node's PARENT so parts (and the restored fused node)
    // stay in the same group. Empty == direct child of the scene root.
    Manager* mgr = Manager::getSingletonPtr();
    Ogre::SceneNode* parent = static_cast<Ogre::SceneNode*>(node->getParent());
    if (mgr && parent && parent != mgr->getSceneMgr()->getRootSceneNode())
        mParentNodeName = parent->getName();

    PartOpsScene::ExplodeResult r =
        PartOpsScene::explodeEntity(entity, mDistance, mEntityName + std::string("_part"),
                                    mCapBoundaries);
    if (!r.ok) {
        mError = r.error.isEmpty() ? QStringLiteral("explode failed") : r.error;
        return;
    }
    for (auto& p : r.parts) {
        PartCache c;
        c.mesh = p.mesh;
        c.name = p.name;
        c.offset = p.offset;
        mParts.push_back(std::move(c));
    }
}

void ExplodePartsCommand::redo()
{
    buildOnce();
    if (mParts.empty()) {
        mOk = false;
        return; // build failed; mError set.
    }

    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) {
        mOk = false;
        return;
    }

    // CREATE-THEN-DESTROY: build every part node FIRST (their names —
    // "<entity>_<part>" — don't collide with the source "<entity>", so both can
    // coexist momentarily), and only destroy the source once ALL parts exist.
    // If any part fails to create, roll back the ones already created and keep
    // the source intact — the scene is never left with neither (CodeRabbit).
    std::vector<std::string> created;
    bool allOk = true;
    for (const auto& p : mParts) {
        Ogre::SceneNode* pn = mgr->addSceneNode(
            QString::fromStdString(mEntityName + "_" + p.name.toStdString()));
        if (!pn) {
            allOk = false;
            break;
        }
        // Offset applied in the source node's LOCAL frame (orient·(scale∘off)),
        // then reparented under the source's original group so parts sit where
        // the fused mesh sat + separated. addSceneNode created it at root.
        const Ogre::Vector3 localOffset = mSrcOrient * (mSrcScale * p.offset);
        reparentAndSetLocal(mgr, pn, mParentNodeName,
                            mSrcPos + localOffset, mSrcOrient, mSrcScale);
        if (!mgr->createEntity(pn, p.mesh)) {
            mgr->destroySceneNode(pn, /*destroyChildrenFirst=*/true);
            allOk = false;
            break;
        }
        created.push_back(pn->getName());
    }

    if (!allOk) {
        // Roll back — leave the source node untouched.
        for (const auto& n : created) {
            if (Ogre::SceneNode* pn = mgr->getSceneNode(QString::fromStdString(n)))
                mgr->destroySceneNode(pn, /*destroyChildrenFirst=*/true);
        }
        mOk = false;
        if (mError.isEmpty())
            mError = QStringLiteral("failed to create part nodes");
        return;
    }

    // All parts exist — now drop selection refs and destroy the fused source
    // node (SplitMeshCommand rationale: sub-entity refs would dangle through
    // getResolvedEntities() once the entity is freed).
    if (auto* sel = SelectionSet::getSingleton())
        sel->clearList();
    if (Ogre::Entity* src = resolveSourceEntity()) {
        if (Ogre::SceneNode* node = src->getParentSceneNode())
            mgr->destroySceneNode(node, /*destroyChildrenFirst=*/true);
    }
    mPartNodeNames = std::move(created);

    // Reselect the created part nodes so the user can immediately move/join.
    if (auto* sel = SelectionSet::getSingleton()) {
        sel->clearList();
        for (const auto& n : mPartNodeNames) {
            if (Ogre::SceneNode* pn = mgr->getSceneNode(QString::fromStdString(n)))
                sel->append(pn);
        }
    }

    mOk = true;
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.parts.explode"),
                                  QStringLiteral("parts=%1").arg(mPartNodeNames.size()));
}

void ExplodePartsCommand::undo()
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr || !mOriginalMesh)
        return;

    // CREATE-THEN-DESTROY: recreate the fused node FIRST (its name "<entity>"
    // doesn't collide with the part names "<entity>_<part>", so it can coexist),
    // and only destroy the parts once the fused node + entity exist. If the
    // fused node can't be created, leave the parts in place — undo never leaves
    // the scene empty (CodeRabbit).
    Ogre::SceneNode* node = mgr->addSceneNode(QString::fromStdString(mEntityName));
    if (!node)
        return;
    reparentAndSetLocal(mgr, node, mParentNodeName, mSrcPos, mSrcOrient, mSrcScale);
    if (!mgr->createEntity(node, mOriginalMesh)) {
        mgr->destroySceneNode(node, /*destroyChildrenFirst=*/true);
        return;
    }

    if (auto* sel = SelectionSet::getSingleton())
        sel->clearList();

    // Fused node is live — now destroy the part nodes.
    for (const auto& n : mPartNodeNames) {
        if (Ogre::SceneNode* pn = mgr->getSceneNode(QString::fromStdString(n)))
            mgr->destroySceneNode(pn, /*destroyChildrenFirst=*/true);
    }
    mPartNodeNames.clear();

    if (auto* sel = SelectionSet::getSingleton())
        sel->selectOne(node);
}
