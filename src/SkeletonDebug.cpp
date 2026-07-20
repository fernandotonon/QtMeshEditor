#include "SkeletonDebug.h"
#include "GlobalDefinitions.h"

#include <algorithm>
#include <array>
#include <cassert>

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

    for (auto* ent : mBoneEntities) {
        ent->setMaterial(mBoneMatPtr);
        ent->setVisible(mShowBones);
    }
    // Dim hierarchy links so joint spheres read as the bone location.
    for (auto* ent : mBoneEntities) {
        if (ent->getMesh() && ent->getMesh()->getName() == "SkeletonDebug/LinkMesh")
            ent->setMaterial(mLinkMatPtr);
    }

    for (Ogre::Bone* root : mEntity->getSkeleton()->getRootBones()) {
        auto it = mBoneVisualByName.find(root->getName());
        if (it != mBoneVisualByName.end() && it->second) {
            it->second->setMaterial(mBoneMatRootPtr);
            it->second->setVisible(mShowBones);
        }
    }
    for (auto* bone : mEntity->getSkeleton()->getBones()) {
        if (!bone->getUserObjectBindings().getUserAny("selected").has_value())
            continue;
        if (!Ogre::any_cast<bool>(bone->getUserObjectBindings().getUserAny("selected")))
            continue;

        currentSelected = bone->getHandle();
        selectedName = bone->getName();

        // Highlight the joint and its outbound links (not sibling links).
        for (auto* ent : mBoneEntities) {
            if (boneNameForMovable(ent) == selectedName) {
                ent->setMaterial(mBoneMatSelectedPtr);
                ent->setVisible(mShowBones);
            }
        }
    }

    if (currentSelected != mLastSelectedBone) {
        mLastSelectedBone = currentSelected;
        if (currentSelected >= 0)
            emit boneSelected(static_cast<unsigned short>(currentSelected));
    }
}

void SkeletonDebug::rebuildVisuals()
{
    if (!mEntity || !mEntity->hasSkeleton())
        return;

    const bool wasAxes = mShowAxes;
    const bool wasBones = mShowBones;
    const bool wasNames = mShowNames;

    mTimer.stop();
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
    showAxes(wasAxes);
    showBones(wasBones);
    showNames(wasNames);
    mTimer.start(0);
}

SkeletonDebug::~SkeletonDebug()
{
    mTimer.stop();
    mTimer.disconnect();

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

        Ogre::Entity* joint = mSceneMan->createEntity("SkeletonDebug/JointMesh");
        auto* jointTp = mEntity->attachObjectToBone(pBone->getName(), (Ogre::MovableObject*)joint);
        jointTp->setScale(jointScale, jointScale, jointScale);
        mBoneEntities.push_back(joint);
        tagBoneVisual(joint, pBone->getName(), mEntity->getName());
        mapEntities[pBone->getName().data()] = joint;

        if (numChildren > 0)
            createChildLinks(pBone);

        Ogre::Entity* axes = mSceneMan->createEntity("SkeletonDebug/AxesMesh");
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
