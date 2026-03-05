#include "NormalVisualizer.h"
#include "Manager.h"

NormalVisualizer::NormalVisualizer(Ogre::SceneManager* sceneMgr, QObject* parent)
    : QObject(parent)
    , mSceneMgr(sceneMgr)
{
    createMaterial();

    connect(Manager::getSingleton(), &Manager::entityCreated,
            this, &NormalVisualizer::onEntityCreated);
    connect(Manager::getSingleton(), &Manager::sceneNodeDestroyed,
            this, &NormalVisualizer::onSceneNodeDestroyed);
}

NormalVisualizer::~NormalVisualizer()
{
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
    }
    else
    {
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

    Ogre::String moName = "NormalVisualizer_" + entity->getName();
    Ogre::ManualObject* mo = mSceneMgr->createManualObject(moName);

    const float normalLength = 0.1f;
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

            // Color = abs(normal) mapped to RGB (like a normal map)
            Ogre::ColourValue color(
                std::abs(norm.x),
                std::abs(norm.y),
                std::abs(norm.z),
                1.0f
            );

            // Start point of the line
            mo->position(pos);
            mo->colour(color);

            // End point of the line
            Ogre::Vector3 endPos = pos + norm * normalLength;
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
        mOverlays.insert(entity, {mo, overlayNode});
    }
    else
    {
        mSceneMgr->destroyManualObject(mo);
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
}
