#include "SkeletonDebug.h"

#include <array>
#include <cassert>

#include "Manager.h"

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

    auto mapEntities = createBoneVisuals();

    showAxes(false);
    showBones(false);
    showNames(false);

    connect(&mTimer, &QTimer::timeout, this, [this, mapEntities](){
        short currentSelected = -1;
        for(auto* ent: mBoneEntities){
            ent->setMaterial(mBoneMatPtr);
            ent->setVisible(mShowBones);
        }
        for(auto* bone : mEntity->getSkeleton()->getBones())
        {
            if(!bone->getUserObjectBindings().getUserAny("selected").has_value())
                continue;

            if(!Ogre::any_cast<bool>(bone->getUserObjectBindings().getUserAny("selected")))
                continue;

            currentSelected = bone->getHandle();

            if(mapEntities.find(bone->getName()) == mapEntities.end())
                continue;

            Ogre::Entity* ent = mapEntities.find(bone->getName())->second;
            ent->setMaterial(mBoneMatSelectedPtr);
            ent->setVisible(mShowBones);
        }

        if (currentSelected != mLastSelectedBone)
        {
            mLastSelectedBone = currentSelected;
            if (currentSelected >= 0)
                emit boneSelected(static_cast<unsigned short>(currentSelected));
        }
    });
    mTimer.start(0);
}

SkeletonDebug::~SkeletonDebug()
{
    mTimer.stop();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();

    for(auto* ent : mBoneEntities)
    {
        mEntity->detachObjectFromBone(ent);
        sceneMgr->destroyEntity(ent);
    }
    mBoneEntities.clear();

    for(auto* ent : mAxisEntities)
    {
        mEntity->detachObjectFromBone(ent);
        sceneMgr->destroyEntity(ent);
    }
    mAxisEntities.clear();
}

void SkeletonDebug::createChildBoneRepresentations(const Ogre::Bone* pBone, Ogre::Entity*& lastEnt)
{
    for(unsigned short i = 0; i < pBone->numChildren(); ++i)
    {
        float length = pBone->getChild(i)->getPosition().length();
        if(length < 0.00001f)
            continue;

        lastEnt = mSceneMan->createEntity("SkeletonDebug/BoneMesh");
        auto* tp = mEntity->attachObjectToBone(pBone->getName(), (Ogre::MovableObject*)lastEnt);
        mBoneEntities.push_back(lastEnt);
        tp->setScale(length, length, length);
    }
}

std::map<std::string, Ogre::Entity*, std::less<>> SkeletonDebug::createBoneVisuals()
{
    std::map<std::string, Ogre::Entity*, std::less<>> mapEntities;
    int numBones = mEntity->getSkeleton()->getNumBones();

    for(unsigned short int iBone = 0; iBone < numBones; ++iBone)
    {
        const Ogre::Bone* pBone = mEntity->getSkeleton()->getBone(iBone);
        if(!pBone)
        {
            assert(false);
            continue;
        }

        Ogre::Entity *ent = nullptr;

        if(unsigned short numChildren = pBone->numChildren(); numChildren == 0)
        {
            ent = mSceneMan->createEntity("SkeletonDebug/BoneMesh");
            auto* tp = mEntity->attachObjectToBone(pBone->getName(), (Ogre::MovableObject*)ent);
            mBoneEntities.push_back(ent);

            float length = pBone->getPosition().length();
            if(length >= 0.00001f)
                tp->setScale(length, length, length);
        }
        else
        {
            createChildBoneRepresentations(pBone, ent);
        }

        mapEntities[pBone->getName().data()] = ent;

        ent = mSceneMan->createEntity("SkeletonDebug/AxesMesh");
        auto* tp = mEntity->attachObjectToBone(pBone->getName(), (Ogre::MovableObject*)ent);
        tp->setScale((mScaleAxes/mEntity->getParentSceneNode()->getScale().x), (mScaleAxes/mEntity->getParentSceneNode()->getScale().y), (mScaleAxes/mEntity->getParentSceneNode()->getScale().z));
        mAxisEntities.push_back(ent);
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
