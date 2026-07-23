#include "SplitMeshCommand.h"

#include "Manager.h"
#include "MeshSegmenter.h"
#include "SubMeshOps.h"
#include "PartOpsMesh.h"
#include "AutoRig.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <OgreEntity.h>
#include <OgreSceneNode.h>
#include <OgreSceneManager.h>

#include <cstdint>

SplitMeshCommand::SplitMeshCommand(std::string entityName, int upAxis, QString category,
                                   bool noModel, QString namePrefix, QUndoCommand* parent)
    : QUndoCommand(parent)
    , mEntityName(std::move(entityName))
    , mUpAxis(upAxis)
    , mCategory(std::move(category))
    , mNoModel(noModel)
    , mNamePrefix(std::move(namePrefix))
{
    setText(QStringLiteral("Split Mesh into Parts"));
}

Ogre::Entity* SplitMeshCommand::resolveEntity() const
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

Ogre::Entity* SplitMeshCommand::swapEntityMesh(const Ogre::MeshPtr& mesh)
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

    // CRITICAL: drop every reference to the entity we're about to free.
    // SelectionSet tracks the selected entity + its sub-entities (and the
    // transform gizmos + Scene tree mirror that selection); it only auto-cleans
    // on Manager::sceneNodeDestroyed, but we destroy the ENTITY while keeping
    // the NODE — so without clearing the selection first, those references
    // dangle and the next gizmo/tree touch crashes on freed memory.
    if (auto* sel = SelectionSet::getSingleton()) {
        const bool wasSelected = sel->contains(node);
        sel->removeOne(cur);
        for (auto* subEnt : cur->getSubEntities())
            sel->removeOne(subEnt);
        mReselectNode = wasSelected ? node : nullptr;
    }

    // Destroy the current entity FIRST — Manager::createEntity names the new
    // entity after the node, so the old one must release that name.
    node->detachObject(cur);
    mgr->getSceneMgr()->destroyEntity(cur);

    // createEntity re-attaches to the node, emits entityCreated (so the Scene
    // tree rebuilds against the NEW entity), and re-applies light linking.
    Ogre::Entity* ne = mgr->createEntity(node, mesh);

    // Restore selection to the node so the user keeps their target selected
    // (createEntity already selects it unless we were mid-scene-init).
    if (ne && mReselectNode) {
        if (auto* sel = SelectionSet::getSingleton())
            sel->selectOne(node);
    }
    return ne;
}

void SplitMeshCommand::redo()
{
    // Build the split mesh once; later redos just re-swap the cached result.
    if (!mBuilt) {
        mBuilt = true;
        Ogre::Entity* entity = resolveEntity();
        if (!entity || !entity->getMesh()) {
            mError = QStringLiteral("no entity to split");
            return;
        }
        mOriginalMesh = entity->getMesh(); // stays resident for undo.

        // Segment: gather geometry + rig-prior labels, resolve category, run
        // predict — the same pipeline the CLI uses (offline when noModel).
        std::vector<float> verts;
        std::vector<uint32_t> indices;
        if (!AutoRig::gatherGeometry(entity, verts, indices) || verts.empty()) {
            mError = QStringLiteral("no readable geometry");
            return;
        }
        const int vertexCount = static_cast<int>(verts.size() / 3);
        int rigResolved = 0;
        std::vector<int> rigLabels =
            AutoRig::rigPriorPartLabels(entity, vertexCount, &rigResolved);

        MeshSegmenter::Options opts;
        opts.upAxis = mUpAxis;
        opts.forceFallback = mNoModel;
        bool ok = false;
        opts.category = MeshSegmenter::categoryFromName(mCategory, &ok);
        if (!ok)
            opts.category = MeshSegmenter::Category::Auto;
        if (!mNoModel)
            opts.category = MeshSegmenter::resolveCategoryBlocking(verts.data(), vertexCount, opts);
        else if (opts.category == MeshSegmenter::Category::Auto)
            opts.category = MeshSegmenter::Category::Body;

        QString modelPath;
        if (!mNoModel)
            modelPath = MeshSegmenter::ensureModelBlocking(opts.category);

        const MeshSegmenter::Result r = MeshSegmenter::predict(
            verts.data(), vertexCount, indices.data(), static_cast<int>(indices.size()),
            modelPath, opts, rigLabels.empty() ? nullptr : rigLabels.data());
        if (!r.ok) {
            mError = r.error.isEmpty() ? QStringLiteral("segmentation failed") : r.error;
            return;
        }

        SubMeshOps::SplitOptions sopts;
        if (!mNamePrefix.isEmpty())
            sopts.namePrefix = mNamePrefix;
        auto groups = SubMeshOps::groupFacesByLabel(r.faceLabels);
        PartOpsMesh::SplitOutcome so = PartOpsMesh::splitEntity(
            entity, r.faceLabels, groups, sopts,
            mEntityName + std::string("_parts"));
        if (!so.ok) {
            mError = so.error.isEmpty() ? QStringLiteral("split failed") : so.error;
            return;
        }
        mSplitMesh = so.mesh;
        mCreatedSubMeshes = so.createdSubMeshes;
        mPartNames = so.partNames;
    }

    if (!mSplitMesh) {
        mOk = false;
        return; // build failed on first redo; mError already set.
    }
    Ogre::Entity* ne = swapEntityMesh(mSplitMesh);
    mOk = (ne != nullptr);
    if (mOk)
        SentryReporter::addBreadcrumb(QStringLiteral("mesh.parts.split_segments"),
                                      QStringLiteral("parts=%1").arg(mCreatedSubMeshes));
    else if (mError.isEmpty())
        mError = QStringLiteral("failed to swap in split mesh");
}

void SplitMeshCommand::undo()
{
    if (!mOriginalMesh)
        return;
    swapEntityMesh(mOriginalMesh);
}
