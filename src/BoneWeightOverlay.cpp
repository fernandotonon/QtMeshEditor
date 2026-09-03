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

    connect(&mUpdateTimer, &QTimer::timeout, this, [this]() {
        pollBoneSelection();
        updateOverlayPositions();
        updateVertexOverlay();
    });
}

BoneWeightOverlay::~BoneWeightOverlay()
{
    mUpdateTimer.stop();
    mUpdateTimer.disconnect();
    destroyOverlay();
    destroyVertexOverlay();

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
        destroyVertexOverlay();
    }
}

void BoneWeightOverlay::setShowVertices(bool show)
{
    if (mShowVertices == show)
        return;
    mShowVertices = show;
    if (show)
        updateVertexOverlay();
    else
        destroyVertexOverlay();
}

/// Small opaque dots, DEPTH-TESTED on purpose.
///
/// The heat map itself runs depth-check off so it shows through the surface,
/// which is what makes it hard to tell which side of the mesh a colour is on.
/// These dots keep depth testing, so back-facing vertices are hidden by the
/// geometry and the visible ones read as being ON the near surface — that
/// occlusion IS the depth cue the flat heat map cannot give.
void BoneWeightOverlay::createVertexMaterial()
{
    auto& matMgr = Ogre::MaterialManager::getSingleton();
    const char* kName = "BoneWeightOverlay/VertexMaterial";
    mVertexMaterial = matMgr.getByName(kName);

    // NB: no early return once the fill material resolves. All THREE materials
    // must be re-resolved on every call — a MaterialManager teardown (toggling
    // the overlay off, a mesh reload) invalidates every cached MaterialPtr, and
    // returning early here left mVertexHaloMaterial / mVertexPlainMaterial
    // dangling. begin() then dereferenced the stale pointer and crashed inside
    // MaterialManager::getByName. Reported as: disable weights, then re-enable
    // paint -> crash.
    if (!mVertexMaterial)
    {
        mVertexMaterial = matMgr.create(
            kName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* pass = mVertexMaterial->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(false);
        // Per-vertex colour: each dot carries its own heat-map colour, so the dots
        // read as part of the heat map rather than a separate white layer.
        pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
        pass->setDepthCheckEnabled(true);
        pass->setDepthWriteEnabled(false);
        pass->setPointSize(kDotFillSize);
        pass->setPointSpritesEnabled(false);
    }

    // The HALO: a larger, near-black point drawn UNDER the coloured one. A GL
    // point cannot have an outline of its own, and a heat-map-coloured dot on
    // top of the same heat-map colour is invisible by construction — so the
    // contrast has to come from a second, bigger dark point behind it. That
    // dark ring is what separates the dot from the surface underneath while
    // still letting the dot show its real weight colour.
    const char* kHaloName = "BoneWeightOverlay/VertexHaloMaterial";
    mVertexHaloMaterial = matMgr.getByName(kHaloName);
    if (!mVertexHaloMaterial)
    {
        mVertexHaloMaterial = matMgr.create(
            kHaloName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* hp = mVertexHaloMaterial->getTechnique(0)->getPass(0);
        hp->setLightingEnabled(false);
        hp->setVertexColourTracking(Ogre::TVC_NONE);
        const Ogre::ColourValue halo(0.05f, 0.05f, 0.05f, 1.0f);
        hp->setDiffuse(halo);
        hp->setAmbient(halo);
        hp->setSelfIllumination(halo);
        hp->setDepthCheckEnabled(true);
        hp->setDepthWriteEnabled(false);
        hp->setPointSize(kDotHaloSize);
        hp->setPointSpritesEnabled(false);
    }

    // Unconnected vertices: one flat mid-grey, no halo, no per-vertex colour.
    // They are positional context only, so they stay visually quiet.
    const char* kPlainName = "BoneWeightOverlay/VertexPlainMaterial";
    mVertexPlainMaterial = matMgr.getByName(kPlainName);
    if (!mVertexPlainMaterial)
    {
        mVertexPlainMaterial = matMgr.create(
            kPlainName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
        auto* pp = mVertexPlainMaterial->getTechnique(0)->getPass(0);
        pp->setLightingEnabled(false);
        pp->setVertexColourTracking(Ogre::TVC_NONE);
        const Ogre::ColourValue plain(0.55f, 0.55f, 0.58f, 1.0f);
        pp->setDiffuse(plain);
        pp->setAmbient(plain);
        pp->setSelfIllumination(plain);
        pp->setDepthCheckEnabled(true);
        pp->setDepthWriteEnabled(false);
        pp->setPointSize(kDotPlainSize);
        pp->setPointSpritesEnabled(false);
    }
}

void BoneWeightOverlay::destroyVertexOverlay()
{
    if (!mVertexOverlay)
        return;
    if (mEntity && mEntity->getParentSceneNode())
        mEntity->getParentSceneNode()->detachObject(mVertexOverlay);
    mSceneMgr->destroyManualObject(mVertexOverlay);
    mVertexOverlay = nullptr;
}

void BoneWeightOverlay::updateVertexOverlay()
{
    if (!mShowVertices || !mVisible || !mEntity || !mEntity->hasSkeleton())
    {
        destroyVertexOverlay();
        return;
    }

    Ogre::MeshPtr mesh = mEntity->getMesh();
    if (!mesh)
        return;

    createVertexMaterial();

    if (!mVertexOverlay)
    {
        mVertexOverlay = mSceneMgr->createManualObject(
            mSceneMgr->getName() + "/BoneWeightVertices/" + mEntity->getName());
        mVertexOverlay->setDynamic(true);
        // Its OWN group, drawn after the heat map. Sharing MAIN+1 with the heat
        // map left the order between them undefined, and the translucent
        // triangles frequently won — painting over 5px points and making the
        // dots effectively invisible even though they were emitted, attached
        // and inside a valid bounding box (all verified before this fix).
        mVertexOverlay->setRenderQueueGroup(Ogre::RENDER_QUEUE_MAIN + 2);
        if (mEntity->getParentSceneNode())
            mEntity->getParentSceneNode()->attachObject(mVertexOverlay);
    }

    // Rebuilt wholesale each tick: the point count is fixed but the SKINNED
    // positions move every frame, so there is nothing stable to cache.
    //
    // Vertices are split by whether they are actually weighted to the selected
    // bone. CONNECTED ones get the full treatment (dark halo + heat-map colour)
    // because those are the ones being painted and worth picking out; the rest
    // get a single flat colour and NO halo. That halves their draw cost and,
    // more importantly, stops a sea of haloed dots on unrelated parts of the
    // mesh from competing with the bone the user is working on.
    mVertexOverlay->clear();

    std::vector<Ogre::Vector3> pts;          // connected: haloed + coloured
    std::vector<Ogre::ColourValue> cols;
    std::vector<Ogre::Vector3> plainPts;     // not weighted to this bone

    bool addedShared = false;
    size_t sectionIndex = 0;
    size_t emitted = 0;

    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si)
    {
        Ogre::SubMesh* submesh = mesh->getSubMesh(si);
        Ogre::VertexData* vertexData = submesh->useSharedVertices
            ? mesh->sharedVertexData : submesh->vertexData;
        if (!vertexData)
            continue;

        // Same section walk as updateOverlayPositions/refreshColours, so the
        // dot colours line up with the heat-map colours vertex for vertex.
        if (submesh->useSharedVertices && addedShared)
            continue;
        if (submesh->useSharedVertices)
            addedShared = true;

        Ogre::VertexData* animData = submesh->useSharedVertices
            ? mEntity->_getSkelAnimVertexData()
            : mEntity->getSubEntity(si)->_getSkelAnimVertexData();
        if (!animData)
        {
            ++sectionIndex;
            continue;
        }

        const auto* posElem =
            animData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        if (!posElem)
        {
            ++sectionIndex;
            continue;
        }

        const std::vector<Ogre::ColourValue>* colours =
            sectionIndex < mSectionColours.size() ? &mSectionColours[sectionIndex]
                                                  : nullptr;

        // Connectivity comes from the ASSIGNMENT LIST, not from the cached
        // colour: a vertex weighted 0.0 to this bone and a vertex with no
        // assignment at all share the same ramp colour, so colour cannot tell
        // them apart. Only a real assignment counts as connected.
        const std::map<size_t, float> weightMap = buildWeightMap(mesh.get(), submesh);

        auto vbuf = animData->vertexBufferBinding->getBuffer(posElem->getSource());
        auto* vertex = static_cast<unsigned char*>(
            vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));

        for (size_t j = 0; j < animData->vertexCount; ++j, vertex += vbuf->getVertexSize())
        {
            Ogre::Real* pReal;
            posElem->baseVertexPointerToElement(vertex, &pReal);
            const Ogre::Vector3 p(pReal[0], pReal[1], pReal[2]);

            if (weightMap.find(j) != weightMap.end())
            {
                pts.push_back(p);
                // Opaque: the heat map itself blends at alpha 0.7, and a
                // translucent dot would mix with it and wash the reading out.
                Ogre::ColourValue c = (colours && j < colours->size())
                    ? (*colours)[j] : Ogre::ColourValue::White;
                c.a = 1.0f;
                cols.push_back(c);
            }
            else
            {
                plainPts.push_back(p);
            }
            ++emitted;
        }

        vbuf->unlock();
        ++sectionIndex;
    }

    // Section order is draw order within a ManualObject, so: unconnected dots
    // first (they must never sit on top of the bone being edited), then each
    // connected dot's halo, then its colour.
    if (!plainPts.empty())
    {
        mVertexOverlay->begin(mVertexPlainMaterial->getName(),
                              Ogre::RenderOperation::OT_POINT_LIST);
        mVertexOverlay->estimateVertexCount(plainPts.size());
        for (const auto& p : plainPts)
            mVertexOverlay->position(p);
        mVertexOverlay->end();
    }

    if (!pts.empty())
    {
        mVertexOverlay->begin(mVertexHaloMaterial->getName(),
                              Ogre::RenderOperation::OT_POINT_LIST);
        mVertexOverlay->estimateVertexCount(pts.size());
        for (const auto& p : pts)
            mVertexOverlay->position(p);
        mVertexOverlay->end();

        mVertexOverlay->begin(mVertexMaterial->getName(),
                              Ogre::RenderOperation::OT_POINT_LIST);
        mVertexOverlay->estimateVertexCount(pts.size());
        for (size_t i = 0; i < pts.size(); ++i)
        {
            mVertexOverlay->position(pts[i]);
            mVertexOverlay->colour(cols[i]);
        }
        mVertexOverlay->end();
    }

    // An empty section renders nothing but still costs a draw call; drop it.
    if (emitted == 0)
        destroyVertexOverlay();
}

/// Per-vertex weight of the selected bone for one submesh's vertex block.
///
/// A submesh that uses SHARED vertices keeps its assignments on the MESH, not on
/// itself: `SubMesh::addBoneAssignment` hard-asserts `!useSharedVertices`, so
/// exactly one of the two lists can ever be non-empty. Selecting the right one
/// therefore reads identically to the previous code, which summed both — that
/// sum was redundant rather than wrong, and no double-counting was reachable.
/// Kept as a single lookup because it states the ownership rule directly and is
/// the shared path for both buildOverlay() and refreshColours().
std::map<size_t, float> BoneWeightOverlay::buildWeightMap(const Ogre::Mesh* mesh,
                                                          const Ogre::SubMesh* submesh) const
{
    std::map<size_t, float> weightMap;
    if (!mesh || !submesh)
        return weightMap;

    const auto& assignments = submesh->useSharedVertices
        ? mesh->getBoneAssignments()
        : submesh->getBoneAssignments();

    for (const auto& [vertexIndex, assignment] : assignments)
    {
        if (assignment.boneIndex == mBoneIndex)
            weightMap[vertexIndex] += assignment.weight;
    }
    return weightMap;
}

void BoneWeightOverlay::refreshColours()
{
    // Re-read the weights and restamp the cached colours WITHOUT rebuilding the
    // ManualObject. rebuildVisuals() destroys and recreates the whole overlay,
    // which is far too heavy to run per dab while a stroke is in flight; the
    // geometry is unchanged by a weight edit, so only the colours need to move.
    if (!mVisible || !mOverlay || !mEntity)
        return;

    const Ogre::MeshPtr& mesh = mEntity->getMesh();
    if (!mesh)
        return;

    size_t sectionIndex = 0;
    bool refreshedShared = false;

    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si)
    {
        Ogre::SubMesh* submesh = mesh->getSubMesh(si);
        Ogre::VertexData* vertexData = submesh->useSharedVertices
            ? mesh->sharedVertexData : submesh->vertexData;
        if (!vertexData)
            continue;

        // Mirror buildOverlay's section walk exactly: the shared vertex block
        // is emitted once, so a second shared submesh maps to no section and
        // must not consume a section index.
        if (submesh->useSharedVertices && refreshedShared)
            continue;
        if (submesh->useSharedVertices)
            refreshedShared = true;

        if (sectionIndex >= mSectionColours.size())
            break;

        const std::map<size_t, float> weightMap = buildWeightMap(mesh.get(), submesh);
        auto& colours = mSectionColours[sectionIndex];

        for (size_t j = 0; j < colours.size(); ++j)
        {
            const auto it = weightMap.find(j);
            colours[j] = weightToColor(it != weightMap.end() ? it->second : 0.0f);
        }
        ++sectionIndex;
    }

    // updateOverlayPositions() re-feeds position+colour from the caches, so
    // restamping the cache is enough to push the new colours to the GPU.
    updateOverlayPositions();
}

void BoneWeightOverlay::rebuildVisuals()
{
    if (!mVisible)
        return;
    destroyOverlay();
    buildOverlay();
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
        p->setVertexColourTracking(Ogre::TVC_DIFFUSE);
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
    // Draw AFTER the mesh (RENDER_QUEUE_MAIN) but before the vertex dots
    // (MAIN+2) and the skeleton (MAIN+3, set in SkeletonDebug). The heat map has depth-check and
    // depth-write OFF so it can show through the surface; that also means an
    // EARLIER queue does not work — the opaque mesh at MAIN would simply paint
    // over it and the overlay disappears entirely (observed). Ordering is
    // therefore mesh (50) -> heat map (51) -> skeleton (52), which keeps the
    // skeleton visible on top without hiding the weights.
    mOverlay->setRenderQueueGroup(Ogre::RENDER_QUEUE_MAIN + 1);

    mSectionColours.clear();
    mSectionIndices.clear();
    bool addedShared = false;

    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si)
    {
        Ogre::SubMesh* submesh = mesh->getSubMesh(si);

        // Build weight lookup for the selected bone
        std::map<size_t, float> weightMap = buildWeightMap(mesh.get(), submesh);

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

void BoneWeightOverlay::pollBoneSelection()
{
    if (!mEntity || !mEntity->hasSkeleton())
        return;

    auto* skeleton = mEntity->getSkeleton();
    for (unsigned short i = 0; i < skeleton->getNumBones(); ++i)
    {
        auto& bindings = skeleton->getBone(i)->getUserObjectBindings();
        if (bindings.getUserAny("selected").has_value() &&
            Ogre::any_cast<bool>(bindings.getUserAny("selected")))
        {
            setSelectedBone(i);
            return;
        }
    }
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
