#include "SkeletonDebug.h"

#include <QTimer>

#include "Manager.h"
// TODO Remove defenitively this cam (for overlay purpose, but this has to be driven app level)
SkeletonDebug::SkeletonDebug(Ogre::Entity* entity, Ogre::SceneManager *man, /*Ogre::Camera *cam,*/ float boneSize, float scaleAxes)
{
    mEntity = entity;
    mSceneMan = man;
    //mCamera = cam;

    mScaleAxes = scaleAxes;

    mBoneSize = boneSize;

    createAxesMaterial();
    createBoneMaterial();
    createAxesMesh();
    createBoneMesh();

    mShowAxes = true;
    mShowBones = true;
    mShowNames = true;

    std::map<std::string, Ogre::Entity*> mapEntities;

    int numBones = mEntity->getSkeleton()->getNumBones();

    for(unsigned short int iBone = 0; iBone < numBones; ++iBone)
    {
        Ogre::Bone* pBone = mEntity->getSkeleton()->getBone(iBone);
        if ( !pBone )
        {
            assert(false);
            continue;
        }

        Ogre::Entity *ent;
        Ogre::TagPoint *tp;

        // Absolutely HAVE to create bone representations first. Otherwise we would get the wrong child count
        // because an attached object counts as a child
        // Would be nice to have a function that only gets the children that are bones...
        unsigned short numChildren = pBone->numChildren();
        if(numChildren == 0)
        {
            // There are no children, but we should still represent the bone
            // Creates a bone of length 1 for leaf bones (bones without children)
            ent = mSceneMan->createEntity("SkeletonDebug/BoneMesh");
            tp = mEntity->attachObjectToBone(pBone->getName(), (Ogre::MovableObject*)ent);
            mBoneEntities.push_back(ent);

            Ogre::Vector3 v = pBone->getPosition();
            // If the length is zero, no point in creating the bone representation
            float length = v.length();
            if(length < 0.00001f)
                continue;
            tp->setScale(length, length, length);
        }
        else
        {
            for(int i = 0; i < numChildren; ++i)
            {
                Ogre::Vector3 v = pBone->getChild(i)->getPosition();
                // If the length is zero, no point in creating the bone representation
                float length = v.length();
                if(length < 0.00001f)
                    continue;

                ent = mSceneMan->createEntity("SkeletonDebug/BoneMesh");
                tp = mEntity->attachObjectToBone(pBone->getName(), (Ogre::MovableObject*)ent);
                mBoneEntities.push_back(ent);

                tp->setScale(length, length, length);
            }
        }

        mapEntities[pBone->getName().data()] = ent;

        ent = mSceneMan->createEntity("SkeletonDebug/AxesMesh");
        tp = mEntity->attachObjectToBone(pBone->getName(), (Ogre::MovableObject*)ent);
        // Make sure we don't wind up with tiny/giant axes and that one axis doesnt get squashed
        tp->setScale((mScaleAxes/mEntity->getParentSceneNode()->getScale().x), (mScaleAxes/mEntity->getParentSceneNode()->getScale().y), (mScaleAxes/mEntity->getParentSceneNode()->getScale().z));
        mAxisEntities.push_back(ent);

       /* Ogre::String name = mEntity->getName() + "SkeletonDebug/BoneText/Bone_";
        name += iBone;
        ObjectTextDisplay *overlay = new ObjectTextDisplay(name, pBone, mCamera, mEntity);
        overlay->enable(true);
        overlay->setText(pBone->getName());
        mTextOverlays.push_back(overlay);*/
    }

    showAxes(false);
    showBones(false);
    showNames(false);

    mTimer = new QTimer();
    connect(mTimer, &QTimer::timeout, this, [=](){
        // Set Selected Bone to Red from user data info
        for(auto ent: mBoneEntities){
            ent->setMaterial(mBoneMatPtr);
            ent->setVisible(mShowBones);
        }
        for(auto bone : mEntity->getSkeleton()->getBones())
        {
            if(!bone->getUserObjectBindings().getUserAny("selected").has_value())
                continue;

            if(!Ogre::any_cast<bool>(bone->getUserObjectBindings().getUserAny("selected")))
                continue;

            if(mapEntities.find(bone->getName()) == mapEntities.end())
                continue;

            Ogre::Entity* ent = mapEntities.find(bone->getName())->second;
            ent->setMaterial(mBoneMatSelectedPtr);
            ent->setVisible(mShowBones);
        }

    });
    mTimer->start(0);
}

SkeletonDebug::~SkeletonDebug()
{
    auto attachedObjects = mEntity->getAttachedObjects();
    for(auto object : attachedObjects)
    {
        Ogre::Entity* e = (Ogre::Entity*)object;
        if(e->getMesh().get()->getName().compare("SkeletonDebug/AxesMesh") ||
                e->getMesh().get()->getName().compare("SkeletonDebug/BoneMesh"))
        {
            Manager::getSingleton()->getSceneMgr()->destroyEntity(e);
        }
    }
}

void SkeletonDebug::update()
{
    /*std::vector<ObjectTextDisplay*>::iterator it;
    for(it = mTextOverlays.begin(); it < mTextOverlays.end(); it++)
    {
        ((ObjectTextDisplay*)*it)->update();
    }*/
}

void SkeletonDebug::showAxes(bool show)
{
    // Don't change anything if we are already in the proper state
    if(mShowAxes == show)
        return;

    mShowAxes = show;

    std::vector<Ogre::Entity*>::iterator it;
    for(it = mAxisEntities.begin(); it < mAxisEntities.end(); ++it)
    {
        ((Ogre::Entity*)*it)->setVisible(show);
    }
}

void SkeletonDebug::showBones(bool show)
{
    // Don't change anything if we are already in the proper state
    if(mShowBones == show)
        return;

    mShowBones = show;

    std::vector<Ogre::Entity*>::iterator it;
    for(it = mBoneEntities.begin(); it < mBoneEntities.end(); ++it)
    {
        ((Ogre::Entity*)*it)->setVisible(show);
    }

}

void SkeletonDebug::showNames(bool show)
{
    // Don't change anything if we are already in the proper state
    if(mShowNames == show)
        return;

    mShowNames = show;

    /*std::vector<ObjectTextDisplay*>::iterator it;
    for(it = mTextOverlays.begin(); it < mTextOverlays.end(); it++)
    {
        ((ObjectTextDisplay*)*it)->enable(show);
    }*/
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

        Ogre::Vector3 basepos[6] =
        {
            Ogre::Vector3(0,0,0),
            Ogre::Vector3(mBoneSize, mBoneSize*2, mBoneSize),
            Ogre::Vector3(-mBoneSize, mBoneSize*2, mBoneSize),
            Ogre::Vector3(-mBoneSize, mBoneSize*2, -mBoneSize),
            Ogre::Vector3(mBoneSize, mBoneSize*2, -mBoneSize),
            Ogre::Vector3(0, mBoneSize*2, 0),
        };

        // Two colours so that we can distinguish the sides of the bones (we don't use any lighting on the material)
        Ogre::ColourValue col = Ogre::ColourValue(0.5f, 0.5f, 0.5f, 1.0f);
        Ogre::ColourValue col1 = Ogre::ColourValue(0.6f, 0.6f, 0.6f, 1.0f);

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
        Ogre::Quaternion quat[6];
        Ogre::ColourValue col[3];

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

        Ogre::Vector3 basepos[7] =
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


        // vertices
        // 6 arrows
        for (size_t i = 0; i < 6; ++i)
        {
            // 7 points
            for (size_t p = 0; p < 7; ++p)
            {
                Ogre::Vector3 pos = quat[i] * basepos[p];
                mo.position(pos);
                mo.colour(col[i / 2]);
            }
        }

        // indices
        // 6 arrows
        for (size_t i = 0; i < 6; ++i)
        {
            size_t base = i * 7;
            mo.triangle(base + 0, base + 1, base + 2);
            mo.triangle(base + 0, base + 2, base + 3);
            mo.triangle(base + 4, base + 5, base + 6);
        }

        mo.end();

        mAxesMeshPtr = mo.convertToMesh(meshName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
    }
}
