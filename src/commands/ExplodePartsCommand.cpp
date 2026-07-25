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
                                         QUndoCommand* parent)
    : QUndoCommand(parent)
    , mEntityName(std::move(entityName))
    , mDistance(distance)
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
        PartOpsScene::explodeEntity(entity, mDistance, mEntityName + std::string("_part"));
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

    // Drop selection refs before destroying the source entity (SplitMeshCommand
    // rationale: sub-entity refs would dangle through getResolvedEntities()).
    if (auto* sel = SelectionSet::getSingleton())
        sel->clearList();

    // Destroy the fused source node (frees its name so undo can recreate it).
    Ogre::Entity* src = resolveSourceEntity();
    if (src) {
        Ogre::SceneNode* node = src->getParentSceneNode();
        if (node)
            mgr->destroySceneNode(node, /*destroyChildrenFirst=*/true);
    }

    // Create one node+entity per part at the source transform, offset outward.
    // The offset is applied in the source node's LOCAL frame (orient·(scale∘off))
    // so a rotated/scaled source explodes consistently with how it sat.
    mPartNodeNames.clear();
    for (const auto& p : mParts) {
        Ogre::SceneNode* pn = mgr->addSceneNode(
            QString::fromStdString(mEntityName + "_" + p.name.toStdString()));
        if (!pn)
            continue;
        // Offset applied in the source node's LOCAL frame (orient·(scale∘off)),
        // then reparented under the source's original group so parts sit where
        // the fused mesh sat + separated. addSceneNode created it at root.
        const Ogre::Vector3 localOffset = mSrcOrient * (mSrcScale * p.offset);
        reparentAndSetLocal(mgr, pn, mParentNodeName,
                            mSrcPos + localOffset, mSrcOrient, mSrcScale);
        mgr->createEntity(pn, p.mesh);
        mPartNodeNames.push_back(pn->getName());
    }

    // Reselect the created part nodes so the user can immediately move/join.
    if (auto* sel = SelectionSet::getSingleton()) {
        sel->clearList();
        for (const auto& n : mPartNodeNames) {
            if (Ogre::SceneNode* pn = mgr->getSceneNode(QString::fromStdString(n)))
                sel->append(pn);
        }
    }

    mOk = !mPartNodeNames.empty();
    if (mOk)
        SentryReporter::addBreadcrumb(QStringLiteral("mesh.parts.explode"),
                                      QStringLiteral("parts=%1").arg(mPartNodeNames.size()));
    else if (mError.isEmpty())
        mError = QStringLiteral("failed to create part nodes");
}

void ExplodePartsCommand::undo()
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr || !mOriginalMesh)
        return;

    if (auto* sel = SelectionSet::getSingleton())
        sel->clearList();

    // Destroy the part nodes.
    for (const auto& n : mPartNodeNames) {
        if (Ogre::SceneNode* pn = mgr->getSceneNode(QString::fromStdString(n)))
            mgr->destroySceneNode(pn, /*destroyChildrenFirst=*/true);
    }
    mPartNodeNames.clear();

    // Recreate the fused node bound to the original mesh at the source transform,
    // reparented under its original group (if any).
    Ogre::SceneNode* node = mgr->addSceneNode(QString::fromStdString(mEntityName));
    if (!node)
        return;
    reparentAndSetLocal(mgr, node, mParentNodeName, mSrcPos, mSrcOrient, mSrcScale);
    mgr->createEntity(node, mOriginalMesh);

    if (auto* sel = SelectionSet::getSingleton())
        sel->selectOne(node);
}
