#include "ExplodePartsCommand.h"

#include "Manager.h"
#include "PartOpsScene.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <OgreEntity.h>
#include <OgreSceneNode.h>
#include <OgreSceneManager.h>

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
    mOriginalMesh = entity->getMesh(); // resident for undo.
    mSrcPos = node->getPosition();
    mSrcOrient = node->getOrientation();
    mSrcScale = node->getScale();

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
        const Ogre::Vector3 worldOffset = mSrcOrient * (mSrcScale * p.offset);
        pn->setPosition(mSrcPos + worldOffset);
        pn->setOrientation(mSrcOrient);
        pn->setScale(mSrcScale);
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

    // Recreate the fused node bound to the original mesh at the source transform.
    Ogre::SceneNode* node = mgr->addSceneNode(QString::fromStdString(mEntityName));
    if (!node)
        return;
    node->setPosition(mSrcPos);
    node->setOrientation(mSrcOrient);
    node->setScale(mSrcScale);
    mgr->createEntity(node, mOriginalMesh);

    if (auto* sel = SelectionSet::getSingleton())
        sel->selectOne(node);
}
