#include "BoneWeightOverlay.h"

#include <map>

BoneWeightOverlay::BoneWeightOverlay(Ogre::Entity* entity, Ogre::SceneManager* sceneMgr)
    : mEntity(entity)
    , mSceneMgr(sceneMgr)
{
    createMaterial();

    // Request software animation so we can read skinned vertex positions
    if (mEntity->hasSkeleton())
    {
        mEntity->addSoftwareAnimationRequest(false);
        mSoftwareAnimRequested = true;
    }

    connect(&mUpdateTimer, &QTimer::timeout, this, &BoneWeightOverlay::updateOverlayPositions);
}

BoneWeightOverlay::~BoneWeightOverlay()
{
    mUpdateTimer.stop();
    destroyOverlay();

    if (mSoftwareAnimRequested && mEntity)
    {
        mEntity->removeSoftwareAnimationRequest(false);
        mSoftwareAnimRequested = false;
    }
}

void BoneWeightOverlay::setSelectedBone(unsigned short boneIndex)
{
    if (mBoneIndex == boneIndex && mOverlay)
        return;

    mBoneIndex = boneIndex;
    if (mVisible)
        buildOverlay();
}

void BoneWeightOverlay::setVisible(bool visible)
{
    mVisible = visible;
    if (visible)
    {
        buildOverlay();
        mUpdateTimer.start(0);
    }
    else
    {
        mUpdateTimer.stop();
        destroyOverlay();
    }
}

void BoneWeightOverlay::destroyOverlay()
{
    if (mOverlay)
    {
        if (mEntity->getParentSceneNode())
            mEntity->getParentSceneNode()->detachObject(mOverlay);
        mSceneMgr->destroyManualObject(mOverlay);
        mOverlay = nullptr;
    }
}

void BoneWeightOverlay::createMaterial()
{
    Ogre::String matName = "BoneWeightOverlay/Material";

    mMaterial = Ogre::static_pointer_cast<Ogre::Material>(
        Ogre::MaterialManager::getSingleton().getByName(matName));

    if (!mMaterial)
    {
        mMaterial = Ogre::static_pointer_cast<Ogre::Material>(
            Ogre::MaterialManager::getSingleton().create(
                matName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME));

        Ogre::Pass* p = mMaterial->getTechnique(0)->getPass(0);
        p->setLightingEnabled(false);
        p->setVertexColourTracking(Ogre::TVC_AMBIENT);
        p->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        p->setCullingMode(Ogre::CULL_NONE);
        p->setDepthCheckEnabled(false);
        p->setDepthWriteEnabled(false);
    }
}

Ogre::ColourValue BoneWeightOverlay::weightToColor(float weight)
{
    // Clamp to [0, 1]
    if (weight < 0.0f) weight = 0.0f;
    if (weight > 1.0f) weight = 1.0f;

    // Heat map: blue (0.0) -> cyan (0.25) -> green (0.5) -> yellow (0.75) -> red (1.0)
    float r = 0.0f, g = 0.0f, b = 0.0f;

    if (weight < 0.25f)
    {
        // Blue to Cyan
        float t = weight / 0.25f;
        r = 0.0f;
        g = t;
        b = 1.0f;
    }
    else if (weight < 0.5f)
    {
        // Cyan to Green
        float t = (weight - 0.25f) / 0.25f;
        r = 0.0f;
        g = 1.0f;
        b = 1.0f - t;
    }
    else if (weight < 0.75f)
    {
        // Green to Yellow
        float t = (weight - 0.5f) / 0.25f;
        r = t;
        g = 1.0f;
        b = 0.0f;
    }
    else
    {
        // Yellow to Red
        float t = (weight - 0.75f) / 0.25f;
        r = 1.0f;
        g = 1.0f - t;
        b = 0.0f;
    }

    return Ogre::ColourValue(r, g, b, 0.7f);
}

void BoneWeightOverlay::buildOverlay()
{
    destroyOverlay();

    if (!mEntity || !mEntity->getParentSceneNode())
        return;

    Ogre::MeshPtr mesh = mEntity->getMesh();
    if (!mesh)
        return;

    Ogre::String moName = "BoneWeightOverlay_" + mEntity->getName();
    mOverlay = mSceneMgr->createManualObject(moName);
    mOverlay->setDynamic(true);

    mSectionColours.clear();
    mSectionIndices.clear();
    bool addedShared = false;

    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si)
    {
        Ogre::SubMesh* submesh = mesh->getSubMesh(si);

        // Build weight lookup for the selected bone
        std::map<size_t, float> weightMap;
        const auto& boneAssignments = submesh->getBoneAssignments();
        for (const auto& [vertexIndex, assignment] : boneAssignments)
        {
            if (assignment.boneIndex == mBoneIndex)
                weightMap[vertexIndex] += assignment.weight;
        }

        // Also check shared vertex bone assignments from the mesh itself
        if (submesh->useSharedVertices)
        {
            const auto& sharedAssignments = mesh->getBoneAssignments();
            for (const auto& [vertexIndex, assignment] : sharedAssignments)
            {
                if (assignment.boneIndex == mBoneIndex)
                    weightMap[vertexIndex] += assignment.weight;
            }
        }

        // Get bind-pose vertex data for initial positions
        Ogre::VertexData* vertexData = submesh->useSharedVertices
            ? mesh->sharedVertexData : submesh->vertexData;

        if (!vertexData)
            continue;

        if (submesh->useSharedVertices && addedShared)
            continue;
        if (submesh->useSharedVertices)
            addedShared = true;

        const auto* posElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        if (!posElem)
            continue;

        auto vbuf = vertexData->vertexBufferBinding->getBuffer(posElem->getSource());
        auto* vertex = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));

        // Cache vertex colours for this section (needed for per-frame updates)
        std::vector<Ogre::ColourValue> colours;
        colours.reserve(vertexData->vertexCount);

        mOverlay->begin(mMaterial->getName(), Ogre::RenderOperation::OT_TRIANGLE_LIST);
        mOverlay->estimateVertexCount(vertexData->vertexCount);

        // Add vertices with weight colors
        for (size_t j = 0; j < vertexData->vertexCount; ++j, vertex += vbuf->getVertexSize())
        {
            Ogre::Real* pReal;
            posElem->baseVertexPointerToElement(vertex, &pReal);

            float weight = 0.0f;
            auto it = weightMap.find(j);
            if (it != weightMap.end())
                weight = it->second;

            auto col = weightToColor(weight);
            colours.push_back(col);

            mOverlay->position(pReal[0], pReal[1], pReal[2]);
            mOverlay->colour(col);
        }

        vbuf->unlock();
        mSectionColours.push_back(std::move(colours));

        // Add and cache indices
        std::vector<uint32_t> sectionIdx;
        Ogre::IndexData* indexData = submesh->indexData;
        if (indexData && indexData->indexCount > 0)
        {
            sectionIdx.reserve(indexData->indexCount);
            mOverlay->estimateIndexCount(indexData->indexCount);

            auto* ibuf = indexData->indexBuffer.get();
            auto* indices = static_cast<unsigned char*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
            bool use32bit = ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;

            for (size_t j = 0; j < indexData->indexCount; ++j)
            {
                uint32_t idx = use32bit
                    ? reinterpret_cast<uint32_t*>(indices)[j]
                    : reinterpret_cast<uint16_t*>(indices)[j];
                sectionIdx.push_back(idx);
                mOverlay->index(idx);
            }

            ibuf->unlock();
        }
        mSectionIndices.push_back(std::move(sectionIdx));

        mOverlay->end();
    }

    mEntity->getParentSceneNode()->attachObject(mOverlay);
}

void BoneWeightOverlay::updateOverlayPositions()
{
    if (!mOverlay || !mEntity || !mEntity->hasSkeleton())
        return;

    // Ensure animation buffers are up to date
    mEntity->_updateAnimation();

    Ogre::MeshPtr mesh = mEntity->getMesh();
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

        if (sectionIndex >= mSectionColours.size() || sectionIndex >= mSectionIndices.size())
            break;

        // Get the animated vertex data from software skinning
        Ogre::VertexData* animData = nullptr;
        if (submesh->useSharedVertices)
            animData = mEntity->_getSkelAnimVertexData();
        else
            animData = mEntity->getSubEntity(si)->_getSkelAnimVertexData();

        if (!animData)
        {
            ++sectionIndex;
            continue;
        }

        const auto* posElem = animData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        if (!posElem)
        {
            ++sectionIndex;
            continue;
        }

        auto vbuf = animData->vertexBufferBinding->getBuffer(posElem->getSource());
        auto* vertex = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));

        const auto& colours = mSectionColours[sectionIndex];
        const auto& cachedIndices = mSectionIndices[sectionIndex];
        mOverlay->beginUpdate(sectionIndex);

        for (size_t j = 0; j < animData->vertexCount; ++j, vertex += vbuf->getVertexSize())
        {
            Ogre::Real* pReal;
            posElem->baseVertexPointerToElement(vertex, &pReal);

            mOverlay->position(pReal[0], pReal[1], pReal[2]);
            if (j < colours.size())
                mOverlay->colour(colours[j]);
        }

        for (uint32_t idx : cachedIndices)
            mOverlay->index(idx);

        vbuf->unlock();
        mOverlay->end();

        ++sectionIndex;
    }
}
