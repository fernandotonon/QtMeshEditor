#include "NormalVisualizer.h"
#include "Manager.h"
#include "SentryReporter.h"

NormalVisualizer::NormalVisualizer(Ogre::SceneManager* sceneMgr, QObject* parent)
    : QObject(parent)
    , mSceneMgr(sceneMgr)
{
    createMaterial();

    connect(Manager::getSingleton(), &Manager::entityCreated,
            this, &NormalVisualizer::onEntityCreated);
    connect(Manager::getSingleton(), &Manager::sceneNodeDestroyed,
            this, &NormalVisualizer::onSceneNodeDestroyed);

    connect(&mUpdateTimer, &QTimer::timeout, this, &NormalVisualizer::updateAnimatedOverlays);
}

NormalVisualizer::~NormalVisualizer()
{
    mUpdateTimer.stop();
    mUpdateTimer.disconnect();
    destroyAllOverlays();
}

void NormalVisualizer::createMaterial()
{
    Ogre::String matName = "NormalVisualizer/Material";

    mMaterial = Ogre::static_pointer_cast<Ogre::Material>(
        Ogre::MaterialManager::getSingleton().getByName(matName));

    if (!mMaterial)
    {
        mMaterial = Ogre::static_pointer_cast<Ogre::Material>(
            Ogre::MaterialManager::getSingleton().create(
                matName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME));

        Ogre::Pass* p = mMaterial->getTechnique(0)->getPass(0);
        p->setLightingEnabled(false);
        p->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        p->setCullingMode(Ogre::CULL_NONE);
        p->setDepthCheckEnabled(false);
        p->setDepthWriteEnabled(false);
    }
}

void NormalVisualizer::setVisible(bool visible)
{
    SentryReporter::addBreadcrumb("ui.action", visible ? "Show normals" : "Hide normals");

    mVisible = visible;
    if (visible)
    {
        // Iterate scene nodes manually and filter by movable type,
        // because Manager::getEntities() does unsafe static_cast on all
        // attached objects (ManualObjects would crash).
        for (Ogre::SceneNode* node : Manager::getSingleton()->getSceneNodes())
        {
            if (!node) continue;
            for (unsigned short i = 0; i < node->numAttachedObjects(); ++i)
            {
                Ogre::MovableObject* obj = node->getAttachedObject(i);
                if (obj && obj->getMovableType() == "Entity")
                    buildOverlayForEntity(static_cast<Ogre::Entity*>(obj));
            }
        }
        mUpdateTimer.start(16);
    }
    else
    {
        mUpdateTimer.stop();
        destroyAllOverlays();
    }
}

void NormalVisualizer::onEntityCreated(Ogre::Entity* const& entity)
{
    if (mVisible)
        buildOverlayForEntity(entity);
}

void NormalVisualizer::onSceneNodeDestroyed(Ogre::SceneNode* const& node)
{
    if (!mVisible)
        return;

    // Find and remove overlays for entities attached to this node
    QList<Ogre::Entity*> toRemove;
    for (auto it = mOverlays.begin(); it != mOverlays.end(); ++it)
    {
        if (it.key()->getParentSceneNode() == node)
            toRemove.append(it.key());
    }
    for (Ogre::Entity* entity : toRemove)
        destroyOverlayForEntity(entity);
}

void NormalVisualizer::buildOverlayForEntity(Ogre::Entity* entity)
{
    if (!entity || !entity->getParentSceneNode())
        return;

    // Don't duplicate if already built
    if (mOverlays.contains(entity))
        return;

    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh)
        return;

    // Clean up stale overlay if an entity with the same name was replaced
    // (e.g. PrimitiveObject::updatePrimitive destroys/recreates entities
    // without emitting sceneNodeDestroyed, leaving a dangling entry).
    Ogre::String moName = "NormalVisualizer_" + entity->getName();
    if (mSceneMgr->hasManualObject(moName))
    {
        // Find and remove the stale entry keyed by the old (dangling) pointer
        for (auto it = mOverlays.begin(); it != mOverlays.end(); ++it)
        {
            if (it.value().manualObject->getName() == moName)
            {
                OverlayData& stale = it.value();
                stale.node->detachObject(stale.manualObject);
                mSceneMgr->destroyManualObject(stale.manualObject);
                mSceneMgr->destroySceneNode(stale.node);
                Ogre::Entity* staleEntity = it.key();
                mOverlays.erase(it);
                mSoftwareAnimRequested.remove(staleEntity);
                break;
            }
        }
    }

    bool hasSkel = entity->hasSkeleton();

    // Request software animation with normals for skeletal entities
    if (hasSkel && !mSoftwareAnimRequested.contains(entity))
    {
        entity->addSoftwareAnimationRequest(true);
        mSoftwareAnimRequested.insert(entity);
    }

    Ogre::ManualObject* mo = mSceneMgr->createManualObject(moName);
    mo->setDynamic(hasSkel);

    bool addedShared = false;
    bool hasContent = false;

    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si)
    {
        Ogre::SubMesh* submesh = mesh->getSubMesh(si);

        Ogre::VertexData* vertexData = submesh->useSharedVertices
            ? mesh->sharedVertexData : submesh->vertexData;

        if (!vertexData)
            continue;

        if (submesh->useSharedVertices && addedShared)
            continue;
        if (submesh->useSharedVertices)
            addedShared = true;

        const auto* posElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        const auto* normElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL);

        if (!posElem || !normElem)
            continue;

        auto posVbuf = vertexData->vertexBufferBinding->getBuffer(posElem->getSource());
        auto* posData = static_cast<unsigned char*>(posVbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));

        // Normal may be in a different buffer
        unsigned char* normData = nullptr;
        Ogre::HardwareVertexBufferSharedPtr normVbuf;
        bool sameBuffer = (posElem->getSource() == normElem->getSource());
        if (sameBuffer)
        {
            normData = posData;
            normVbuf = posVbuf;
        }
        else
        {
            normVbuf = vertexData->vertexBufferBinding->getBuffer(normElem->getSource());
            normData = static_cast<unsigned char*>(normVbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        }

        mo->begin(mMaterial->getName(), Ogre::RenderOperation::OT_LINE_LIST);
        mo->estimateVertexCount(vertexData->vertexCount * 2);

        for (size_t j = 0; j < vertexData->vertexCount; ++j)
        {
            Ogre::Real* pPos;
            posElem->baseVertexPointerToElement(posData + j * posVbuf->getVertexSize(), &pPos);

            Ogre::Real* pNorm;
            normElem->baseVertexPointerToElement(normData + j * normVbuf->getVertexSize(), &pNorm);

            Ogre::Vector3 pos(pPos[0], pPos[1], pPos[2]);
            Ogre::Vector3 norm(pNorm[0], pNorm[1], pNorm[2]);

            Ogre::ColourValue color(
                std::abs(norm.x),
                std::abs(norm.y),
                std::abs(norm.z),
                1.0f
            );

            mo->position(pos);
            mo->colour(color);

            Ogre::Vector3 endPos = pos + norm * NORMAL_LENGTH;
            mo->position(endPos);
            mo->colour(color);
        }

        mo->end();
        hasContent = true;

        posVbuf->unlock();
        if (!sameBuffer)
            normVbuf->unlock();
    }

    if (hasContent)
    {
        // Attach to a dedicated child node so the ManualObject doesn't appear
        // as an attached object on the entity's node. Code like
        // ObjectItemModel::appendEntitiesFromNode does unsafe static_cast<Entity*>
        // on all attached objects and would crash on our ManualObject.
        Ogre::SceneNode* overlayNode = entity->getParentSceneNode()->createChildSceneNode();
        overlayNode->attachObject(mo);
        mOverlays.insert(entity, {mo, overlayNode, hasSkel});
    }
    else
    {
        mSceneMgr->destroyManualObject(mo);
    }
}

void NormalVisualizer::updateAnimatedOverlays()
{
    for (auto it = mOverlays.begin(); it != mOverlays.end(); ++it)
    {
        Ogre::Entity* entity = it.key();
        OverlayData& data = it.value();

        if (!data.hasSkeleton || !entity->hasSkeleton())
            continue;

        entity->_updateAnimation();

        Ogre::MeshPtr mesh = entity->getMesh();
        bool addedShared = false;
        unsigned short sectionIndex = 0;

        for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si)
        {
            Ogre::SubMesh* submesh = mesh->getSubMesh(si);

            Ogre::VertexData* vertexData = submesh->useSharedVertices
                ? mesh->sharedVertexData : submesh->vertexData;

            if (!vertexData)
                continue;

            if (submesh->useSharedVertices && addedShared)
                continue;
            if (submesh->useSharedVertices)
                addedShared = true;

            // Check bind-pose has normals (same check as buildOverlayForEntity)
            const auto* bindPosElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
            const auto* bindNormElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL);
            if (!bindPosElem || !bindNormElem)
                continue;

            // Get animated vertex data
            Ogre::VertexData* animData = nullptr;
            if (submesh->useSharedVertices)
                animData = entity->_getSkelAnimVertexData();
            else
                animData = entity->getSubEntity(si)->_getSkelAnimVertexData();

            if (!animData)
            {
                ++sectionIndex;
                continue;
            }

            const auto* posElem = animData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
            const auto* normElem = animData->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL);
            if (!posElem || !normElem)
            {
                ++sectionIndex;
                continue;
            }

            auto posVbuf = animData->vertexBufferBinding->getBuffer(posElem->getSource());
            auto* posBytes = static_cast<unsigned char*>(posVbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));

            unsigned char* normBytes = nullptr;
            Ogre::HardwareVertexBufferSharedPtr normVbuf;
            bool sameBuffer = (posElem->getSource() == normElem->getSource());
            if (sameBuffer)
            {
                normBytes = posBytes;
                normVbuf = posVbuf;
            }
            else
            {
                normVbuf = animData->vertexBufferBinding->getBuffer(normElem->getSource());
                normBytes = static_cast<unsigned char*>(normVbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
            }

            data.manualObject->beginUpdate(sectionIndex);

            for (size_t j = 0; j < animData->vertexCount; ++j)
            {
                Ogre::Real* pPos;
                posElem->baseVertexPointerToElement(posBytes + j * posVbuf->getVertexSize(), &pPos);

                Ogre::Real* pNorm;
                normElem->baseVertexPointerToElement(normBytes + j * normVbuf->getVertexSize(), &pNorm);

                Ogre::Vector3 pos(pPos[0], pPos[1], pPos[2]);
                Ogre::Vector3 norm(pNorm[0], pNorm[1], pNorm[2]);

                Ogre::ColourValue color(
                    std::abs(norm.x),
                    std::abs(norm.y),
                    std::abs(norm.z),
                    1.0f
                );

                data.manualObject->position(pos);
                data.manualObject->colour(color);

                Ogre::Vector3 endPos = pos + norm * NORMAL_LENGTH;
                data.manualObject->position(endPos);
                data.manualObject->colour(color);
            }

            data.manualObject->end();

            posVbuf->unlock();
            if (!sameBuffer)
                normVbuf->unlock();

            ++sectionIndex;
        }
    }
}

void NormalVisualizer::destroyOverlayForEntity(Ogre::Entity* entity)
{
    auto it = mOverlays.find(entity);
    if (it == mOverlays.end())
        return;

    OverlayData& data = it.value();
    data.node->detachObject(data.manualObject);
    mSceneMgr->destroyManualObject(data.manualObject);
    mSceneMgr->destroySceneNode(data.node);
    mOverlays.erase(it);

    if (mSoftwareAnimRequested.contains(entity))
    {
        entity->removeSoftwareAnimationRequest(true);
        mSoftwareAnimRequested.remove(entity);
    }
}

void NormalVisualizer::destroyAllOverlays()
{
    for (auto it = mOverlays.begin(); it != mOverlays.end(); ++it)
    {
        OverlayData& data = it.value();
        data.node->detachObject(data.manualObject);
        mSceneMgr->destroyManualObject(data.manualObject);
        mSceneMgr->destroySceneNode(data.node);
    }
    mOverlays.clear();

    for (Ogre::Entity* entity : mSoftwareAnimRequested)
        entity->removeSoftwareAnimationRequest(true);
    mSoftwareAnimRequested.clear();
}
