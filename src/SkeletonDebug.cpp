#include "SkeletonDebug.h"
#include "GlobalDefinitions.h"
#include "BoneDisplay.h"
#include "BoneDisplayController.h"
#include "Manager.h"

#include <OgreRTShaderSystem.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace {
constexpr const char* kBoneNameTag = "skeletonDebugBoneName";
constexpr const char* kEntityNameTag = "skeletonDebugEntityName";

void tagBoneVisual(Ogre::MovableObject* obj,
                   const Ogre::String& boneName,
                   const Ogre::String& entityName)
{
    if (!obj) return;
    obj->getUserObjectBindings().setUserAny(kBoneNameTag, Ogre::Any(boneName));
    obj->getUserObjectBindings().setUserAny(kEntityNameTag, Ogre::Any(entityName));
    obj->setQueryFlags(BONE_QUERY_FLAGS);
}
}

Ogre::String SkeletonDebug::boneNameForMovable(const Ogre::MovableObject* obj)
{
    if (!obj) return {};
    const auto& any = obj->getUserObjectBindings().getUserAny(kBoneNameTag);
    if (!any.has_value()) return {};
    try {
        return Ogre::any_cast<Ogre::String>(any);
    } catch (const std::exception&) {
        return {};
    }
}

Ogre::String SkeletonDebug::entityNameForMovable(const Ogre::MovableObject* obj)
{
    if (!obj) return {};
    const auto& any = obj->getUserObjectBindings().getUserAny(kEntityNameTag);
    if (!any.has_value()) return {};
    try {
        return Ogre::any_cast<Ogre::String>(any);
    } catch (const std::exception&) {
        return {};
    }
}

SkeletonDebug::SkeletonDebug(Ogre::Entity* entity, Ogre::SceneManager *man, float boneSize, float scaleAxes)
    : mBoneSize(boneSize)
    , mEntity(entity)
    , mSceneMan(man)
    , mScaleAxes(scaleAxes)
{
    createAxesMaterial();
    createBoneMaterial();
    createAxesMesh();
    createBoneMesh();
    createJointMesh();
    createLinkMesh();
    createShapeMeshes();

    mBoneVisualByName = createBoneVisuals();

    showAxes(false);
    showBones(false);
    showNames(false);

    connect(&mTimer, &QTimer::timeout, this, &SkeletonDebug::onTimerTick);
    mTimer.start(0);
}

void SkeletonDebug::onTimerTick()
{
    if (!mEntity || !mEntity->hasSkeleton())
        return;

    short currentSelected = -1;
    std::string selectedName;

    const BoneDisplay::Document display = BoneDisplayController::instance()->documentFor(mEntity);
    std::unordered_set<std::string> rootNames;
    for (Ogre::Bone* root : mEntity->getSkeleton()->getRootBones()) {
        if (root) rootNames.insert(root->getName());
    }
    for (auto* bone : mEntity->getSkeleton()->getBones()) {
        if (!bone->getUserObjectBindings().getUserAny("selected").has_value())
            continue;
        if (!Ogre::any_cast<bool>(bone->getUserObjectBindings().getUserAny("selected")))
            continue;
        currentSelected = bone->getHandle();
        selectedName = bone->getName();
        break;
    }

    for (auto* ent : mBoneEntities) {
        const std::string name = boneNameForMovable(ent);
        const bool shown = mShowBones && BoneDisplay::isVisible(display, QString::fromStdString(name));
        ent->setVisible(shown);
        ent->setQueryFlags(shown ? BONE_QUERY_FLAGS : 0);
        if (!shown) continue;

        const bool link = ent->getMesh() && ent->getMesh()->getName() == "SkeletonDebug/LinkMesh";
        const bool selected = !selectedName.empty() && name == selectedName;
        bool hasColor = false;
        const BoneDisplay::Color color = BoneDisplay::effectiveColor(
            display, QString::fromStdString(name), &hasColor);
        Ogre::MaterialPtr mat;
        if (hasColor) {
            // Keep the chosen hue on the selected bone. The red selection
            // material used to replace it, so editing the swatch looked like
            // it did nothing.
            mat = materialForColor(color.r, color.g, color.b, selected ? std::max(color.a, 0.95f) : color.a);
        } else if (selected) {
            mat = mBoneMatSelectedPtr;
        } else if (link) {
            mat = mLinkMatPtr;
        } else if (rootNames.count(name) != 0) {
            mat = mBoneMatRootPtr;
        } else {
            mat = mBoneMatPtr;
        }
        ent->setMaterial(mat);
        for (unsigned i = 0; i < ent->getNumSubEntities(); ++i)
            ent->getSubEntity(i)->setMaterial(mat);
    }
    for (auto* axes : mAxisEntities) {
        const bool shown = BoneDisplay::isVisible(
            display, QString::fromStdString(boneNameForMovable(axes)));
        axes->setVisible(mShowAxes && shown);
        if (!shown)
            axes->setQueryFlags(0);
        else
            axes->setQueryFlags(BONE_QUERY_FLAGS);
    }

    if (currentSelected != mLastSelectedBone) {
        mLastSelectedBone = currentSelected;
        if (currentSelected >= 0)
            emit boneSelected(static_cast<unsigned short>(currentSelected));
    }

    if (mShowRestGhost)
        updateGhostVisuals();
}

void SkeletonDebug::rebuildVisuals()
{
    if (!mEntity || !mEntity->hasSkeleton())
        return;

    const bool wasAxes = mShowAxes;
    const bool wasBones = mShowBones;
    const bool wasNames = mShowNames;
    const bool wasGhost = mShowRestGhost;

    mTimer.stop();
    destroyGhostVisuals();
    mEntity->detachAllObjectsFromBone();

    for (auto* ent : mBoneEntities)
        mSceneMan->destroyEntity(ent);
    mBoneEntities.clear();

    for (auto* ent : mAxisEntities)
        mSceneMan->destroyEntity(ent);
    mAxisEntities.clear();

    mBoneVisualByName.clear();
    mBoneVisualByName = createBoneVisuals();

    // Force visibility onto freshly created entities — showBones/showAxes early-
    // return when the flag is already true, which would leave new meshes hidden.
    mShowAxes = false;
    mShowBones = false;
    mShowNames = false;
    mShowRestGhost = false;
    showAxes(wasAxes);
    showBones(wasBones);
    showNames(wasNames);
    showRestGhost(wasGhost);
    mTimer.start(0);
}

SkeletonDebug::~SkeletonDebug()
{
    mTimer.stop();
    mTimer.disconnect();

    destroyGhostVisuals();

    // Detach all bone-attached objects at once — this clears mChildObjectList
    // and frees TagPoints without leaving stale parent references.
    mEntity->detachAllObjectsFromBone();

    for(auto* ent : mBoneEntities)
        mSceneMan->destroyEntity(ent);
    mBoneEntities.clear();

    for(auto* ent : mAxisEntities)
        mSceneMan->destroyEntity(ent);
    mAxisEntities.clear();
}

void SkeletonDebug::createChildLinks(const Ogre::Bone* pBone)
{
    // Thin rods parent→child. These are hierarchy edges, not extra bones —
    // the joint sphere at the parent is the actual bone location.
    for (unsigned short i = 0; i < pBone->numChildren(); ++i)
    {
        Ogre::Node* childNode = pBone->getChild(i);
        if (!childNode)
            continue;

        const Ogre::Vector3 childPos = childNode->getInitialPosition();
        const float length = childPos.length();
        if (length < 0.00001f)
            continue;

        Ogre::Entity* link = mSceneMan->createEntity("SkeletonDebug/LinkMesh");
        // Skel Slice D (#558): the skeleton draws LAST so it stays visible on
        // top of both weight overlays. Order is mesh (MAIN) -> heat map (MAIN+1,
        // alpha 0.7, would otherwise wash the bones out) -> vertex dots (MAIN+2)
        // -> skeleton (MAIN+3).
        link->setRenderQueueGroup(Ogre::RENDER_QUEUE_MAIN + 3);
        auto* tp = mEntity->attachObjectToBone(pBone->getName(), (Ogre::MovableObject*)link);
        mBoneEntities.push_back(link);

        const Ogre::Vector3 dir = childPos / length;
        tp->setOrientation(Ogre::Vector3::UNIT_Y.getRotationTo(dir));
        // Link mesh is unit length along +Y; keep thickness constant so
        // multi-child fans don't look like a bundle of full bones.
        const float thick = std::max(mBoneSize * 0.35f, 0.008f);
        tp->setScale(thick, length, thick);

        tagBoneVisual(link, pBone->getName(), mEntity->getName());
    }
}

std::map<std::string, Ogre::Entity*, std::less<>> SkeletonDebug::createBoneVisuals()
{
    std::map<std::string, Ogre::Entity*, std::less<>> mapEntities;
    const BoneDisplay::Document display = BoneDisplayController::instance()->documentFor(mEntity);
    int numBones = mEntity->getSkeleton()->getNumBones();

    Ogre::Vector3 entityScale = Ogre::Vector3::UNIT_SCALE;
    if (mEntity->getParentSceneNode())
        entityScale = mEntity->getParentSceneNode()->getScale();
    const Ogre::Vector3 scaleMagnitude(
        std::max(std::abs(entityScale.x), 1e-4f),
        std::max(std::abs(entityScale.y), 1e-4f),
        std::max(std::abs(entityScale.z), 1e-4f));
    const float invEntScale = 1.f
        / std::max({scaleMagnitude.x, scaleMagnitude.y, scaleMagnitude.z});

    for (unsigned short int iBone = 0; iBone < numBones; ++iBone)
    {
        const Ogre::Bone* pBone = mEntity->getSkeleton()->getBone(iBone);
        if (!pBone)
        {
            assert(false);
            continue;
        }

        // Same sizing rule as the old octahedron bones: scale from the
        // bone's length (avg distance to children, or inbound offset for leaves).
        float length = 0.f;
        const unsigned short numChildren = pBone->numChildren();
        if (numChildren > 0) {
            for (unsigned short i = 0; i < numChildren; ++i) {
                if (auto* child = pBone->getChild(i))
                    length += child->getInitialPosition().length();
            }
            length /= static_cast<float>(numChildren);
        } else {
            length = pBone->getInitialPosition().length();
        }
        if (length < 1e-5f)
            length = mBoneSize;

        // Unit joint mesh → world radius ≈ length * mBoneSize (matches the
        // old setScale(length) on a mesh built in mBoneSize units). Clamp so
        // tiny bones stay pickable and long ones don't swallow neighbours.
        const float jointScale = std::clamp(length * mBoneSize, mBoneSize * 0.2f, mBoneSize * 0.75f)
            * invEntScale;

        const QString boneName = QString::fromStdString(pBone->getName());
        const BoneDisplay::Bone displayBone = BoneDisplay::boneOrDefault(display, boneName);
        Ogre::MeshPtr customMesh;
        if (displayBone.shape == BoneDisplay::Shape::CustomMesh && !displayBone.customMesh.isEmpty()) {
            if (auto* mgr = Manager::getSingletonPtr()) {
                for (Ogre::Entity* candidate : mgr->getEntities()) {
                    if (!candidate || candidate == mEntity || !candidate->getMesh()) continue;
                    if (QString::fromStdString(candidate->getName()) == displayBone.customMesh) {
                        customMesh = candidate->getMesh();
                        break;
                    }
                }
            }
        }

        const char* shapeMesh = "SkeletonDebug/JointMesh";
        switch (displayBone.shape) {
        case BoneDisplay::Shape::Sphere: shapeMesh = "SkeletonDebug/SphereMesh"; break;
        case BoneDisplay::Shape::Cube: shapeMesh = "SkeletonDebug/CubeMesh"; break;
        case BoneDisplay::Shape::Arrow: shapeMesh = "SkeletonDebug/ArrowMesh"; break;
        case BoneDisplay::Shape::Line: shapeMesh = "SkeletonDebug/LineMesh"; break;
        case BoneDisplay::Shape::Circle: shapeMesh = "SkeletonDebug/CircleMesh"; break;
        case BoneDisplay::Shape::Ring: shapeMesh = "SkeletonDebug/RingMesh"; break;
        case BoneDisplay::Shape::CustomMesh:
        case BoneDisplay::Shape::Octahedron: shapeMesh = "SkeletonDebug/JointMesh"; break;
        }

        Ogre::Entity* joint = nullptr;
        if (customMesh) {
            try {
                joint = mSceneMan->createEntity(customMesh);
            } catch (const Ogre::Exception&) {
                joint = nullptr;
            }
        }
        if (!joint)
            joint = mSceneMan->createEntity(shapeMesh);
        joint->setRenderQueueGroup(Ogre::RENDER_QUEUE_MAIN + 3);
        auto* jointTp = mEntity->attachObjectToBone(pBone->getName(), (Ogre::MovableObject*)joint);

        const bool arrowLike = displayBone.shape == BoneDisplay::Shape::Arrow
            || displayBone.shape == BoneDisplay::Shape::Line;
        Ogre::Quaternion align = Ogre::Quaternion::IDENTITY;
        if (arrowLike && numChildren > 0) {
            if (auto* child = pBone->getChild(0)) {
                const Ogre::Vector3 dir = child->getInitialPosition();
                if (dir.squaredLength() > 1e-10f)
                    align = Ogre::Vector3::UNIT_Y.getRotationTo(dir.normalisedCopy());
            }
        }
        const Ogre::Quaternion user =
            Ogre::Quaternion(Ogre::Degree(displayBone.rotationDeg.x), Ogre::Vector3::UNIT_X)
            * Ogre::Quaternion(Ogre::Degree(displayBone.rotationDeg.y), Ogre::Vector3::UNIT_Y)
            * Ogre::Quaternion(Ogre::Degree(displayBone.rotationDeg.z), Ogre::Vector3::UNIT_Z);
        jointTp->setOrientation(align * user);
        const Ogre::Vector3 localOffset(displayBone.offset.x, displayBone.offset.y, displayBone.offset.z);
        const float sx = displayBone.scale.x;
        const float sy = displayBone.scale.y;
        const float sz = displayBone.scale.z;
        if (customMesh) {
            jointTp->setScale(sx, sy, sz);
            jointTp->setPosition(align * localOffset);
        } else if (arrowLike) {
            jointTp->setScale(jointScale * sx, std::max(length, mBoneSize) * sy, jointScale * sz);
            jointTp->setPosition(align * localOffset * jointScale);
        } else {
            jointTp->setScale(jointScale * sx, jointScale * sy, jointScale * sz);
            jointTp->setPosition(align * localOffset * jointScale);
        }
        mBoneEntities.push_back(joint);
        tagBoneVisual(joint, pBone->getName(), mEntity->getName());
        mapEntities[pBone->getName().data()] = joint;

        if (numChildren > 0)
            createChildLinks(pBone);

        Ogre::Entity* axes = mSceneMan->createEntity("SkeletonDebug/AxesMesh");
        axes->setRenderQueueGroup(Ogre::RENDER_QUEUE_MAIN + 3);
        auto* axesTp = mEntity->attachObjectToBone(pBone->getName(), (Ogre::MovableObject*)axes);
        axesTp->setScale(mScaleAxes / scaleMagnitude.x,
                         mScaleAxes / scaleMagnitude.y,
                         mScaleAxes / scaleMagnitude.z);
        mAxisEntities.push_back(axes);
        tagBoneVisual(axes, pBone->getName(), mEntity->getName());
    }

    return mapEntities;
}

void SkeletonDebug::update() const
{
    // Intentionally empty — bone updates are handled by the QTimer callback
}

void SkeletonDebug::showAxes(bool show)
{
    // Don't change anything if we are already in the proper state
    if(mShowAxes == show)
        return;

    mShowAxes = show;

    for(auto* ent : mAxisEntities)
    {
        ent->setVisible(show);
    }
}

void SkeletonDebug::showBones(bool show)
{
    // Don't change anything if we are already in the proper state
    if(mShowBones == show)
        return;

    mShowBones = show;

    for(auto* ent : mBoneEntities)
    {
        ent->setVisible(show);
    }
}

void SkeletonDebug::showNames(bool show)
{
    // Don't change anything if we are already in the proper state
    if(mShowNames == show)
        return;

    mShowNames = show;
}

void SkeletonDebug::showRestGhost(bool show)
{
    if (mShowRestGhost == show)
        return;
    mShowRestGhost = show;
    if (show) {
        ensureGhostVisuals();
        updateGhostVisuals();
    } else {
        destroyGhostVisuals();
    }
}

void SkeletonDebug::destroyGhostVisuals()
{
    if (mGhostMeshEntity) {
        if (mGhostMeshEntity->getParentSceneNode())
            mGhostMeshEntity->detachFromParent();
        mSceneMan->destroyEntity(mGhostMeshEntity);
        mGhostMeshEntity = nullptr;
    }
    // Drop the private mesh clone so the next ghost enable re-snapshots rest.
    if (mGhostMeshPtr) {
        const Ogre::String name = mGhostMeshPtr->getName();
        mGhostMeshPtr.reset();
        if (Ogre::MeshManager::getSingleton().resourceExists(name))
            Ogre::MeshManager::getSingleton().remove(name);
    }
    for (auto* ent : mGhostEntities) {
        if (ent && ent->getParentSceneNode())
            ent->detachFromParent();
        if (ent)
            mSceneMan->destroyEntity(ent);
    }
    mGhostEntities.clear();
    mGhostNodes.clear();
    mGhostBoneNames.clear();
    if (mGhostRoot) {
        mGhostRoot->removeAndDestroyAllChildren();
        mSceneMan->destroySceneNode(mGhostRoot);
        mGhostRoot = nullptr;
    }
}

void SkeletonDebug::ensureGhostVisuals()
{
    if (!mEntity || !mEntity->hasSkeleton() || !mSceneMan)
        return;
    if (mGhostRoot)
        return;

    createBoneMaterial();

    Ogre::SceneNode* entNode = mEntity->getParentSceneNode();
    if (!entNode)
        return;

    mGhostRoot = entNode->createChildSceneNode(
        mEntity->getName() + "_restGhostRoot");

    // Translucent blue MESH frozen at the rest pose when the ghost was enabled.
    // Must NOT share the live mesh resource — rest-pose bake writes into that
    // mesh and would make the ghost follow the new pose.
    // Live skeleton joints keep their normal materials (never this blue).
    try {
        const Ogre::String ghostEntName = mEntity->getName() + "_restGhostMesh";
        const Ogre::String ghostMeshRes = mEntity->getName() + "_restGhostMeshRes";
        if (mSceneMan->hasEntity(ghostEntName))
            mSceneMan->destroyEntity(ghostEntName);
        if (Ogre::MeshManager::getSingleton().resourceExists(ghostMeshRes))
            Ogre::MeshManager::getSingleton().remove(ghostMeshRes);

        mGhostMeshPtr = mEntity->getMesh()->clone(ghostMeshRes);
        mGhostMeshEntity = mSceneMan->createEntity(ghostEntName, ghostMeshRes);
        mGhostMeshEntity->setQueryFlags(0);
        mGhostMeshEntity->setCastShadows(false);
        mGhostMeshEntity->setMaterial(mMeshGhostMatPtr);
        for (unsigned short i = 0; i < mGhostMeshEntity->getNumSubEntities(); ++i)
            mGhostMeshEntity->getSubEntity(i)->setMaterial(mMeshGhostMatPtr);
        mGhostRoot->attachObject(mGhostMeshEntity);
        // Keep ghost at its cloned bind: disable clips and reset to bind.
        if (Ogre::AnimationStateSet* states = mGhostMeshEntity->getAllAnimationStates()) {
            for (const auto& pair : states->getAnimationStates())
                pair.second->setEnabled(false);
        }
        if (Ogre::SkeletonInstance* gSkel = mGhostMeshEntity->getSkeleton())
            gSkel->reset(true);
    } catch (const Ogre::Exception&) {
        mGhostMeshEntity = nullptr;
        if (mGhostMeshPtr) {
            const Ogre::String name = mGhostMeshPtr->getName();
            mGhostMeshPtr.reset();
            if (Ogre::MeshManager::getSingleton().resourceExists(name))
                Ogre::MeshManager::getSingleton().remove(name);
        }
    }
}

void SkeletonDebug::updateGhostVisuals()
{
    if (!mShowRestGhost || !mEntity || !mGhostRoot)
        return;

    // Force the ghost mesh to the bind pose every tick so it never
    // picks up the live animated skeleton.
    if (mGhostMeshEntity) {
        if (Ogre::SkeletonInstance* gSkel = mGhostMeshEntity->getSkeleton())
            gSkel->reset(true);
        mGhostMeshEntity->setVisible(true);
        if (mMeshGhostMatPtr) {
            for (unsigned short i = 0; i < mGhostMeshEntity->getNumSubEntities(); ++i)
                mGhostMeshEntity->getSubEntity(i)->setMaterial(mMeshGhostMatPtr);
        }
    }
}

void SkeletonDebug::createAxesMaterial()
{
    Ogre::String matName = "SkeletonDebug/AxesMat";

    mAxisMatPtr = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().getByName(matName));
    if (!mAxisMatPtr)
    {
        mAxisMatPtr = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().create(matName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME));

        // First pass for axes that are partially within the model (shows transparency)
        Ogre::Pass* p = mAxisMatPtr->getTechnique(0)->getPass(0);
        p->setLightingEnabled(false);
        p->setPolygonModeOverrideable(false);
        p->setVertexColourTracking(Ogre::TVC_AMBIENT);
        p->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        p->setCullingMode(Ogre::CULL_NONE);
        p->setDepthWriteEnabled(false);
        p->setDepthCheckEnabled(false);

        // Second pass for the portion of the axis that is outside the model (solid colour)
        Ogre::Pass* p2 = mAxisMatPtr->getTechnique(0)->createPass();
        p2->setLightingEnabled(false);
        p2->setPolygonModeOverrideable(false);
        p2->setVertexColourTracking(Ogre::TVC_AMBIENT);
        p2->setCullingMode(Ogre::CULL_NONE);
        p2->setDepthWriteEnabled(false);
    }
}

void SkeletonDebug::createBoneMaterial()
{
    Ogre::String matName = "SkeletonDebug/BoneMat";

    mBoneMatPtr = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().getByName(matName));

    if (!mBoneMatPtr)
    {
        mBoneMatPtr = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().create(matName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME));

        Ogre::Pass* p = mBoneMatPtr->getTechnique(0)->getPass(0);
        p->setLightingEnabled(false);
        p->setPolygonModeOverrideable(false);
        p->setVertexColourTracking(Ogre::TVC_AMBIENT);
        p->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        p->setCullingMode(Ogre::CULL_ANTICLOCKWISE);
        p->setDepthWriteEnabled(false);
        p->setDepthCheckEnabled(false);
    }

    // Create a red bone material for selected bones
    Ogre::String matName2 = "SkeletonDebug/BoneMatSelected";

    mBoneMatSelectedPtr = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().getByName(matName2));

    if (!mBoneMatSelectedPtr)
    {
        SkeletonDebug::mBoneMatSelectedPtr = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().create(matName2, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME));

        Ogre::Pass* p = mBoneMatSelectedPtr->getTechnique(0)->getPass(0);
        p->setLightingEnabled(true);
        p->setDepthWriteEnabled(false);
        p->setDepthCheckEnabled(false);
        p->setVertexColourTracking(Ogre::TVC_AMBIENT);
        p->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        p->setCullingMode(Ogre::CULL_ANTICLOCKWISE);
        p->setDiffuse(1,0,0,1);
        p->setAmbient(1,0,0);
        p->setEmissive(1,0,0);
    }

    // Yellow material for root bones — visual hint that translation is
    // unrestricted on these bones (the rest are rotation-only when rigged).
    Ogre::String matName3 = "SkeletonDebug/BoneMatRoot";
    mBoneMatRootPtr = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().getByName(matName3));
    if (!mBoneMatRootPtr) {
        mBoneMatRootPtr = Ogre::static_pointer_cast<Ogre::Material>(Ogre::MaterialManager::getSingleton().create(matName3, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME));
        Ogre::Pass* p = mBoneMatRootPtr->getTechnique(0)->getPass(0);
        p->setLightingEnabled(true);
        p->setDepthWriteEnabled(false);
        p->setDepthCheckEnabled(false);
        p->setVertexColourTracking(Ogre::TVC_AMBIENT);
        p->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        p->setCullingMode(Ogre::CULL_ANTICLOCKWISE);
        p->setDiffuse(1, 0.85f, 0, 1);
        p->setAmbient(1, 0.85f, 0);
        p->setEmissive(1, 0.85f, 0);
    }

    // Dimmer material for hierarchy link rods (edges, not joints).
    Ogre::String linkMatName = "SkeletonDebug/LinkMat";
    mLinkMatPtr = Ogre::static_pointer_cast<Ogre::Material>(
        Ogre::MaterialManager::getSingleton().getByName(linkMatName));
    if (!mLinkMatPtr) {
        mLinkMatPtr = Ogre::static_pointer_cast<Ogre::Material>(
            Ogre::MaterialManager::getSingleton().create(
                linkMatName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME));
        Ogre::Pass* p = mLinkMatPtr->getTechnique(0)->getPass(0);
        p->setLightingEnabled(false);
        p->setPolygonModeOverrideable(false);
        p->setVertexColourTracking(Ogre::TVC_AMBIENT);
        p->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        p->setCullingMode(Ogre::CULL_NONE);
        p->setDepthWriteEnabled(false);
        p->setDepthCheckEnabled(false);
    }

    // Translucent blue for rest-pose MESH ghost (slice C #557).
    // Live skeleton joints never use this — only the ghost mesh entity.
    Ogre::String meshGhostMatName = "SkeletonDebug/MeshGhostMat";
    mMeshGhostMatPtr = Ogre::static_pointer_cast<Ogre::Material>(
        Ogre::MaterialManager::getSingleton().getByName(meshGhostMatName));
    if (!mMeshGhostMatPtr) {
        mMeshGhostMatPtr = Ogre::static_pointer_cast<Ogre::Material>(
            Ogre::MaterialManager::getSingleton().create(
                meshGhostMatName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME));
        Ogre::Pass* p = mMeshGhostMatPtr->getTechnique(0)->getPass(0);
        p->setLightingEnabled(true);
        p->setDepthWriteEnabled(false);
        p->setDepthCheckEnabled(true);
        p->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        p->setCullingMode(Ogre::CULL_NONE);
        p->setDiffuse(0.25f, 0.55f, 1.0f, 0.35f);
        p->setAmbient(0.1f, 0.25f, 0.5f);
        p->setEmissive(0.05f, 0.15f, 0.35f);
    }

    // Legacy joint-ghost material kept for any residual joint markers.
    Ogre::String ghostMatName = "SkeletonDebug/BoneMatGhost";
    mBoneMatGhostPtr = Ogre::static_pointer_cast<Ogre::Material>(
        Ogre::MaterialManager::getSingleton().getByName(ghostMatName));
    if (!mBoneMatGhostPtr) {
        mBoneMatGhostPtr = Ogre::static_pointer_cast<Ogre::Material>(
            Ogre::MaterialManager::getSingleton().create(
                ghostMatName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME));
        Ogre::Pass* p = mBoneMatGhostPtr->getTechnique(0)->getPass(0);
        p->setLightingEnabled(true);
        p->setDepthWriteEnabled(false);
        p->setDepthCheckEnabled(false);
        p->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        p->setCullingMode(Ogre::CULL_ANTICLOCKWISE);
        p->setDiffuse(0.25f, 0.55f, 1.0f, 0.35f);
        p->setAmbient(0.1f, 0.25f, 0.5f);
        p->setEmissive(0.05f, 0.15f, 0.35f);
    }
}

void SkeletonDebug::createBoneMesh()
{
    Ogre::String meshName = "SkeletonDebug/BoneMesh";

    mBoneMeshPtr = Ogre::static_pointer_cast<Ogre::Mesh>(Ogre::MeshManager::getSingleton().getByName(meshName));
    if(!mBoneMeshPtr)
    {
        Ogre::ManualObject mo("tmp");
        mo.begin(mBoneMatPtr->getName());

        std::array<Ogre::Vector3, 6> basepos =
        {
            Ogre::Vector3(0,0,0),
            Ogre::Vector3(mBoneSize, mBoneSize*2, mBoneSize),
            Ogre::Vector3(-mBoneSize, mBoneSize*2, mBoneSize),
            Ogre::Vector3(-mBoneSize, mBoneSize*2, -mBoneSize),
            Ogre::Vector3(mBoneSize, mBoneSize*2, -mBoneSize),
            Ogre::Vector3(0, mBoneSize*2, 0),
        };

        // Two colours so that we can distinguish the sides of the bones (we don't use any lighting on the material)
        auto col = Ogre::ColourValue(0.5f, 0.5f, 0.5f, 1.0f);
        auto col1 = Ogre::ColourValue(0.6f, 0.6f, 0.6f, 1.0f);

        mo.position(basepos[0]);
        mo.colour(col);
        mo.position(basepos[2]);
        mo.colour(col);
        mo.position(basepos[1]);
        mo.colour(col);

        mo.position(basepos[0]);
        mo.colour(col1);
        mo.position(basepos[3]);
        mo.colour(col1);
        mo.position(basepos[2]);
        mo.colour(col1);

        mo.position(basepos[0]);
        mo.colour(col);
        mo.position(basepos[4]);
        mo.colour(col);
        mo.position(basepos[3]);
        mo.colour(col);

        mo.position(basepos[0]);
        mo.colour(col1);
        mo.position(basepos[1]);
        mo.colour(col1);
        mo.position(basepos[4]);
        mo.colour(col1);

        mo.position(basepos[1]);
        mo.colour(col1);
        mo.position(basepos[2]);
        mo.colour(col1);
        mo.position(basepos[5]);
        mo.colour(col1);

        mo.position(basepos[2]);
        mo.colour(col);
        mo.position(basepos[3]);
        mo.colour(col);
        mo.position(basepos[5]);
        mo.colour(col);

        mo.position(basepos[3]);
        mo.colour(col1);
        mo.position(basepos[4]);
        mo.colour(col1);
        mo.position(basepos[5]);
        mo.colour(col1);

        mo.position(basepos[4]);
        mo.colour(col);
        mo.position(basepos[1]);
        mo.colour(col);
        mo.position(basepos[5]);
        mo.colour(col);

        // indices
        mo.triangle(0, 1, 2);
        mo.triangle(3, 4, 5);
        mo.triangle(6, 7, 8);
        mo.triangle(9, 10, 11);
        mo.triangle(12, 13, 14);
        mo.triangle(15, 16, 17);
        mo.triangle(18, 19, 20);
        mo.triangle(21, 22, 23);

        mo.end();

        mBoneMeshPtr = mo.convertToMesh(meshName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
    }
}

void SkeletonDebug::createJointMesh()
{
    // Unit octahedron centered at origin — scaled at attach time.
    const Ogre::String meshName = "SkeletonDebug/JointMesh";
    mJointMeshPtr = Ogre::static_pointer_cast<Ogre::Mesh>(
        Ogre::MeshManager::getSingleton().getByName(meshName));
    if (mJointMeshPtr)
        return;

    Ogre::ManualObject mo("tmpJoint");
    mo.begin(mBoneMatPtr->getName());
    const Ogre::ColourValue col(0.85f, 0.85f, 0.9f, 1.0f);
    const std::array<Ogre::Vector3, 6> v = {
        Ogre::Vector3( 1, 0, 0),
        Ogre::Vector3(-1, 0, 0),
        Ogre::Vector3( 0, 1, 0),
        Ogre::Vector3( 0,-1, 0),
        Ogre::Vector3( 0, 0, 1),
        Ogre::Vector3( 0, 0,-1),
    };
    auto tri = [&](int a, int b, int c) {
        mo.position(v[a]); mo.colour(col);
        mo.position(v[b]); mo.colour(col);
        mo.position(v[c]); mo.colour(col);
    };
    tri(0, 2, 4); tri(0, 4, 3); tri(0, 3, 5); tri(0, 5, 2);
    tri(1, 4, 2); tri(1, 3, 4); tri(1, 5, 3); tri(1, 2, 5);
    for (unsigned short i = 0; i < 8; ++i)
        mo.triangle(i * 3, i * 3 + 1, i * 3 + 2);
    mo.end();
    mJointMeshPtr = mo.convertToMesh(meshName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
}

void SkeletonDebug::createLinkMesh()
{
    // Unit-length thin octahedron along +Y [0..1]. Scaled: (thick, length, thick).
    const Ogre::String meshName = "SkeletonDebug/LinkMesh";
    mLinkMeshPtr = Ogre::static_pointer_cast<Ogre::Mesh>(
        Ogre::MeshManager::getSingleton().getByName(meshName));
    if (mLinkMeshPtr)
        return;

    Ogre::ManualObject mo("tmpLink");
    mo.begin(mLinkMatPtr ? mLinkMatPtr->getName() : mBoneMatPtr->getName());
    const Ogre::ColourValue col(0.45f, 0.45f, 0.5f, 0.85f);
    const Ogre::ColourValue col1(0.55f, 0.55f, 0.6f, 0.85f);
    const float r = 0.5f;
    const std::array<Ogre::Vector3, 6> basepos = {
        Ogre::Vector3(0, 0, 0),
        Ogre::Vector3( r, 0.5f,  r),
        Ogre::Vector3(-r, 0.5f,  r),
        Ogre::Vector3(-r, 0.5f, -r),
        Ogre::Vector3( r, 0.5f, -r),
        Ogre::Vector3(0, 1, 0),
    };
    auto tri = [&](int a, int b, int c, const Ogre::ColourValue& cval) {
        mo.position(basepos[a]); mo.colour(cval);
        mo.position(basepos[b]); mo.colour(cval);
        mo.position(basepos[c]); mo.colour(cval);
    };
    tri(0, 2, 1, col);  tri(0, 3, 2, col1);
    tri(0, 4, 3, col);  tri(0, 1, 4, col1);
    tri(1, 2, 5, col1); tri(2, 3, 5, col);
    tri(3, 4, 5, col1); tri(4, 1, 5, col);
    for (unsigned short i = 0; i < 8; ++i)
        mo.triangle(i * 3, i * 3 + 1, i * 3 + 2);
    mo.end();
    mLinkMeshPtr = mo.convertToMesh(meshName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
}

void SkeletonDebug::createAxesMesh()
{
    Ogre::String meshName = "SkeletonDebug/AxesMesh";

    mAxesMeshPtr = Ogre::static_pointer_cast<Ogre::Mesh>(Ogre::MeshManager::getSingleton().getByName(meshName));
    if (!mAxesMeshPtr)
    {
        Ogre::ManualObject mo("tmp");
        mo.begin(mAxisMatPtr->getName());
        /* 3 axes, each made up of 2 of these (base plane = XY)
        *   .------------|\
        *   '------------|/
        */
        mo.estimateVertexCount(7 * 2 * 3);
        mo.estimateIndexCount(3 * 2 * 3);
        std::array<Ogre::Quaternion, 6> quat;
        std::array<Ogre::ColourValue, 3> col;

        // x-axis
        quat[0] = Ogre::Quaternion::IDENTITY;
        quat[1].FromAxes(Ogre::Vector3::UNIT_X, Ogre::Vector3::NEGATIVE_UNIT_Z, Ogre::Vector3::UNIT_Y);
        col[0] = Ogre::ColourValue::Red;
        col[0].a = 0.3f;
        // y-axis
        quat[2].FromAxes(Ogre::Vector3::UNIT_Y, Ogre::Vector3::NEGATIVE_UNIT_X, Ogre::Vector3::UNIT_Z);
        quat[3].FromAxes(Ogre::Vector3::UNIT_Y, Ogre::Vector3::UNIT_Z, Ogre::Vector3::UNIT_X);
        col[1] = Ogre::ColourValue::Green;
        col[1].a = 0.3f;
        // z-axis
        quat[4].FromAxes(Ogre::Vector3::UNIT_Z, Ogre::Vector3::UNIT_Y, Ogre::Vector3::NEGATIVE_UNIT_X);
        quat[5].FromAxes(Ogre::Vector3::UNIT_Z, Ogre::Vector3::UNIT_X, Ogre::Vector3::UNIT_Y);
        col[2] = Ogre::ColourValue::Blue;
        col[2].a = 0.3f;

        std::array<Ogre::Vector3, 7> basepos =
        {
            // stalk
            Ogre::Vector3(0.0f, 0.05f, 0.0f),
            Ogre::Vector3(0.0f, -0.05f, 0.0f),
            Ogre::Vector3(0.7f, -0.05f, 0.0f),
            Ogre::Vector3(0.7f, 0.05f, 0.0f),
            // head
            Ogre::Vector3(0.7f, -0.15f, 0.0f),
            Ogre::Vector3(1.0f, 0.0f, 0.0f),
            Ogre::Vector3(0.7f, 0.15f, 0.0f)
        };

        // vertices — 6 arrows, 7 points each
        for (size_t i = 0; i < 6; ++i)
        {
            for (const auto& bp : basepos)
            {
                mo.position(quat[i] * bp);
                mo.colour(col[i / 2]);
            }
        }

        // indices — 6 arrows
        for (unsigned int i = 0; i < 6; ++i)
        {
            unsigned int base = i * 7;
            mo.triangle(base + 0, base + 1, base + 2);
            mo.triangle(base + 0, base + 2, base + 3);
            mo.triangle(base + 4, base + 5, base + 6);
        }

        mo.end();

        mAxesMeshPtr = mo.convertToMesh(meshName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
    }
}

void SkeletonDebug::createShapeMeshes()
{
    const Ogre::ColourValue white(1.f, 1.f, 1.f, 1.f);
    auto finish = [&](Ogre::ManualObject& mo, int triangles) {
        for (int i = 0; i < triangles; ++i)
            mo.triangle(static_cast<uint32_t>(i * 3),
                        static_cast<uint32_t>(i * 3 + 1),
                        static_cast<uint32_t>(i * 3 + 2));
        mo.end();
    };
    auto ensure = [&](const char* meshName, const char* tmpName, const auto& build) {
        if (Ogre::MeshManager::getSingleton().resourceExists(meshName))
            return;
        Ogre::ManualObject mo(tmpName);
        mo.begin(mBoneMatPtr->getName());
        const int triangles = build(mo);
        finish(mo, triangles);
        mo.convertToMesh(meshName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
    };

    ensure("SkeletonDebug/SphereMesh", "tmpBoneSphere", [&](Ogre::ManualObject& mo) {
        const int stacks = 6;
        const int slices = 10;
        int triangles = 0;
        auto pos = [](float phi, float theta) {
            return Ogre::Vector3(std::sin(phi) * std::cos(theta),
                                 std::cos(phi),
                                 std::sin(phi) * std::sin(theta));
        };
        for (int s = 0; s < stacks; ++s) {
            const float phi0 = Ogre::Math::PI * static_cast<float>(s) / stacks;
            const float phi1 = Ogre::Math::PI * static_cast<float>(s + 1) / stacks;
            for (int i = 0; i < slices; ++i) {
                const float th0 = Ogre::Math::TWO_PI * static_cast<float>(i) / slices;
                const float th1 = Ogre::Math::TWO_PI * static_cast<float>(i + 1) / slices;
                const Ogre::Vector3 p00 = pos(phi0, th0);
                const Ogre::Vector3 p10 = pos(phi0, th1);
                const Ogre::Vector3 p01 = pos(phi1, th0);
                const Ogre::Vector3 p11 = pos(phi1, th1);
                if (s == 0) {
                    mo.position(p00); mo.colour(white);
                    mo.position(p01); mo.colour(white);
                    mo.position(p11); mo.colour(white);
                    ++triangles;
                } else if (s == stacks - 1) {
                    mo.position(p00); mo.colour(white);
                    mo.position(p11); mo.colour(white);
                    mo.position(p10); mo.colour(white);
                    ++triangles;
                } else {
                    mo.position(p00); mo.colour(white);
                    mo.position(p01); mo.colour(white);
                    mo.position(p11); mo.colour(white);
                    mo.position(p00); mo.colour(white);
                    mo.position(p11); mo.colour(white);
                    mo.position(p10); mo.colour(white);
                    triangles += 2;
                }
            }
        }
        return triangles;
    });

    ensure("SkeletonDebug/CubeMesh", "tmpBoneCube", [&](Ogre::ManualObject& mo) {
        const Ogre::Vector3 v[8] = {
            {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
            {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1},
        };
        const int faces[12][3] = {
            {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
            {0, 1, 5}, {0, 5, 4}, {3, 6, 2}, {3, 7, 6},
            {0, 4, 7}, {0, 7, 3}, {1, 2, 6}, {1, 6, 5},
        };
        for (const auto& f : faces) {
            mo.position(v[f[0]]); mo.colour(white);
            mo.position(v[f[1]]); mo.colour(white);
            mo.position(v[f[2]]); mo.colour(white);
        }
        return 12;
    });

    auto addBox = [&](Ogre::ManualObject& mo, float y0, float y1, float rx, float rz) {
        const Ogre::Vector3 v[8] = {
            {-rx, y0, -rz}, {rx, y0, -rz}, {rx, y1, -rz}, {-rx, y1, -rz},
            {-rx, y0, rz}, {rx, y0, rz}, {rx, y1, rz}, {-rx, y1, rz},
        };
        const int faces[12][3] = {
            {0, 1, 2}, {0, 2, 3}, {4, 6, 5}, {4, 7, 6},
            {0, 4, 5}, {0, 5, 1}, {3, 2, 6}, {3, 6, 7},
            {0, 3, 7}, {0, 7, 4}, {1, 5, 6}, {1, 6, 2},
        };
        for (const auto& f : faces) {
            mo.position(v[f[0]]); mo.colour(white);
            mo.position(v[f[1]]); mo.colour(white);
            mo.position(v[f[2]]); mo.colour(white);
        }
    };
    ensure("SkeletonDebug/LineMesh", "tmpBoneLine", [&](Ogre::ManualObject& mo) {
        addBox(mo, 0.f, 1.f, 0.08f, 0.08f);
        return 12;
    });
    ensure("SkeletonDebug/ArrowMesh", "tmpBoneArrow", [&](Ogre::ManualObject& mo) {
        addBox(mo, 0.f, 0.68f, 0.1f, 0.1f);
        const int slices = 10;
        int triangles = 12;
        const Ogre::Vector3 tip(0.f, 1.f, 0.f);
        const Ogre::Vector3 baseCenter(0.f, 0.62f, 0.f);
        for (int i = 0; i < slices; ++i) {
            const float a0 = Ogre::Math::TWO_PI * static_cast<float>(i) / slices;
            const float a1 = Ogre::Math::TWO_PI * static_cast<float>(i + 1) / slices;
            const Ogre::Vector3 p0(std::cos(a0) * 0.32f, 0.62f, std::sin(a0) * 0.32f);
            const Ogre::Vector3 p1(std::cos(a1) * 0.32f, 0.62f, std::sin(a1) * 0.32f);
            mo.position(tip); mo.colour(white);
            mo.position(p0); mo.colour(white);
            mo.position(p1); mo.colour(white);
            mo.position(baseCenter); mo.colour(white);
            mo.position(p1); mo.colour(white);
            mo.position(p0); mo.colour(white);
            triangles += 2;
        }
        return triangles;
    });

    ensure("SkeletonDebug/CircleMesh", "tmpBoneCircle", [&](Ogre::ManualObject& mo) {
        const int slices = 16;
        const Ogre::Vector3 center(0.f, 0.f, 0.f);
        for (int i = 0; i < slices; ++i) {
            const float a0 = Ogre::Math::TWO_PI * static_cast<float>(i) / slices;
            const float a1 = Ogre::Math::TWO_PI * static_cast<float>(i + 1) / slices;
            mo.position(center); mo.colour(white);
            mo.position(std::cos(a0), 0.f, std::sin(a0)); mo.colour(white);
            mo.position(std::cos(a1), 0.f, std::sin(a1)); mo.colour(white);
            mo.position(center); mo.colour(white);
            mo.position(std::cos(a1), 0.f, std::sin(a1)); mo.colour(white);
            mo.position(std::cos(a0), 0.f, std::sin(a0)); mo.colour(white);
        }
        return slices * 2;
    });
    ensure("SkeletonDebug/RingMesh", "tmpBoneRing", [&](Ogre::ManualObject& mo) {
        const int slices = 16;
        const float inner = 0.55f;
        for (int i = 0; i < slices; ++i) {
            const float a0 = Ogre::Math::TWO_PI * static_cast<float>(i) / slices;
            const float a1 = Ogre::Math::TWO_PI * static_cast<float>(i + 1) / slices;
            const Ogre::Vector3 o0(std::cos(a0), 0.f, std::sin(a0));
            const Ogre::Vector3 o1(std::cos(a1), 0.f, std::sin(a1));
            const Ogre::Vector3 i0 = o0 * inner;
            const Ogre::Vector3 i1 = o1 * inner;
            mo.position(o0); mo.colour(white);
            mo.position(i0); mo.colour(white);
            mo.position(i1); mo.colour(white);
            mo.position(o0); mo.colour(white);
            mo.position(i1); mo.colour(white);
            mo.position(o1); mo.colour(white);
            mo.position(o0); mo.colour(white);
            mo.position(i1); mo.colour(white);
            mo.position(i0); mo.colour(white);
            mo.position(o1); mo.colour(white);
            mo.position(o0); mo.colour(white);
            mo.position(i1); mo.colour(white);
        }
        return slices * 4;
    });
}

Ogre::MaterialPtr SkeletonDebug::materialForColor(float r, float g, float b, float a)
{
    const auto quant = [](float v) {
        return static_cast<int>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f));
    };
    const std::string key = std::to_string(quant(r)) + "_" + std::to_string(quant(g)) + "_"
        + std::to_string(quant(b)) + "_" + std::to_string(quant(a));
    const auto found = mColorMaterials.find(key);
    if (found != mColorMaterials.end())
        return found->second;

    const Ogre::String matName = "SkeletonDebug/BoneColor/" + key;
    Ogre::MaterialPtr mat = Ogre::static_pointer_cast<Ogre::Material>(
        Ogre::MaterialManager::getSingleton().getByName(matName));
    if (!mat) {
        mat = Ogre::static_pointer_cast<Ogre::Material>(
            Ogre::MaterialManager::getSingleton().create(
                matName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME));
        Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
        // Lighting stays on with tracking off so the baked vertex colours in
        // the bone meshes do not replace this swatch. Emissive carries the
        // colour; diffuse stays zero so scene lights do not wash it out.
        pass->setLightingEnabled(true);
        pass->setDepthWriteEnabled(false);
        pass->setDepthCheckEnabled(false);
        pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        pass->setCullingMode(Ogre::CULL_NONE);
        pass->setVertexColourTracking(Ogre::TVC_NONE);
        pass->setAmbient(0.f, 0.f, 0.f);
        pass->setDiffuse(0.f, 0.f, 0.f, a);
        pass->setEmissive(r, g, b);
        pass->setSelfIllumination(r, g, b);
        if (auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr()) {
            const Ogre::String scheme = Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME;
            shaderGen->createShaderBasedTechnique(
                *mat, Ogre::MaterialManager::DEFAULT_SCHEME_NAME, scheme);
            shaderGen->validateMaterial(scheme, *mat);
        }
    }
    mColorMaterials.emplace(key, mat);
    return mat;
}
