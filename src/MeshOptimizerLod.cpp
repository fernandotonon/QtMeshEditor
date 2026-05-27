#include "MeshOptimizerLod.h"

#include <meshoptimizer.h>

#include <OgreHardwareBufferManager.h>
#include <OgreHardwareIndexBuffer.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreLogManager.h>
#include <OgreVertexIndexData.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace MeshOptimizerLod {

namespace {

// Extract a tight float-XYZ position array (one vec3 per vertex) for
// a submesh. meshoptimizer's simplify path wants positions as a flat
// `const float*` with a stride — we de-interleave to keep the call
// site small and avoid stride bookkeeping.
//
// Returns empty on any unsupported vertex layout (positions missing,
// shared vertex buffers we'd need to dedupe across submeshes for —
// for now we go through `vertexData->vertexBufferBinding` directly
// and accept the case where the submesh has its own vertex data).
std::vector<float> extractPositions(Ogre::VertexData* vertexData)
{
    if (!vertexData) return {};
    const auto* posElem = vertexData->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (!posElem) return {};

    auto vbuf = vertexData->vertexBufferBinding->getBuffer(posElem->getSource());
    const size_t vertCount = vertexData->vertexCount;
    if (vertCount == 0 || !vbuf) return {};

    std::vector<float> positions(vertCount * 3);
    auto* src = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    const size_t stride = vbuf->getVertexSize();

    for (size_t i = 0; i < vertCount; ++i) {
        float* dst = nullptr;
        posElem->baseVertexPointerToElement(src + i * stride, &dst);
        positions[i * 3 + 0] = dst[0];
        positions[i * 3 + 1] = dst[1];
        positions[i * 3 + 2] = dst[2];
    }
    vbuf->unlock();
    return positions;
}

// Extract UV0 as a tight float-UV array for attribute-aware simplify.
// Returns empty if there's no TEXCOORD_0 — caller falls back to the
// no-attribute meshopt_simplify path. We always pull only UV0: it's
// what binds the dominant diffuse texture in every Mixamo / glTF /
// Maya-export FBX we see, and adding more channels grows attribute
// memory by ~1.5x per stream with diminishing return on visual
// quality for typical LODs.
std::vector<float> extractUV0(Ogre::VertexData* vertexData)
{
    if (!vertexData) return {};
    const auto* uvElem = vertexData->vertexDeclaration->findElementBySemantic(
        Ogre::VES_TEXTURE_COORDINATES, 0);
    if (!uvElem) return {};

    auto vbuf = vertexData->vertexBufferBinding->getBuffer(uvElem->getSource());
    const size_t vertCount = vertexData->vertexCount;
    if (vertCount == 0 || !vbuf) return {};

    std::vector<float> uvs(vertCount * 2);
    auto* src = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    const size_t stride = vbuf->getVertexSize();

    for (size_t i = 0; i < vertCount; ++i) {
        float* dst = nullptr;
        uvElem->baseVertexPointerToElement(src + i * stride, &dst);
        uvs[i * 2 + 0] = dst[0];
        uvs[i * 2 + 1] = dst[1];
    }
    vbuf->unlock();
    return uvs;
}

// Pull the submesh's triangle list as a flat uint32 array. Handles
// 16-bit and 32-bit Ogre index buffers transparently.
std::vector<uint32_t> extractIndices(Ogre::IndexData* indexData)
{
    if (!indexData || !indexData->indexBuffer || indexData->indexCount == 0)
        return {};

    auto ibuf = indexData->indexBuffer;
    const size_t indexCount = indexData->indexCount;
    std::vector<uint32_t> out(indexCount);

    auto* src = static_cast<unsigned char*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    src += indexData->indexStart * ibuf->getIndexSize();

    if (ibuf->getType() == Ogre::HardwareIndexBuffer::IT_16BIT) {
        const auto* p = reinterpret_cast<const uint16_t*>(src);
        for (size_t i = 0; i < indexCount; ++i) out[i] = p[i];
    } else {
        std::memcpy(out.data(), src, indexCount * sizeof(uint32_t));
    }
    ibuf->unlock();
    return out;
}

// Wrap a uint32 triangle list as an Ogre IndexData* that the caller
// can stuff into `SubMesh::mLodFaceList`. Picks 16-bit storage when
// the max index fits (saves half the GPU bandwidth) and 32-bit
// otherwise.
Ogre::IndexData* buildIndexData(const std::vector<uint32_t>& indices)
{
    auto* indexData = OGRE_NEW Ogre::IndexData();
    indexData->indexCount = indices.size();
    indexData->indexStart = 0;

    if (indices.empty()) return indexData;

    uint32_t maxIdx = 0;
    for (uint32_t i : indices) if (i > maxIdx) maxIdx = i;
    const bool use32Bit = (maxIdx > std::numeric_limits<uint16_t>::max());

    const auto itype = use32Bit
        ? Ogre::HardwareIndexBuffer::IT_32BIT
        : Ogre::HardwareIndexBuffer::IT_16BIT;

    indexData->indexBuffer = Ogre::HardwareBufferManager::getSingleton()
        .createIndexBuffer(itype, indices.size(), Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);

    auto* dst = indexData->indexBuffer->lock(Ogre::HardwareBuffer::HBL_DISCARD);
    if (use32Bit) {
        std::memcpy(dst, indices.data(), indices.size() * sizeof(uint32_t));
    } else {
        auto* p = reinterpret_cast<uint16_t*>(dst);
        for (size_t i = 0; i < indices.size(); ++i)
            p[i] = static_cast<uint16_t>(indices[i]);
    }
    indexData->indexBuffer->unlock();
    return indexData;
}

// Compute the diagonal of the mesh's bounding box; used to scale
// meshoptimizer's `target_error` from a relative ratio (0.01 =
// "1% of the mesh's size") into the absolute units meshoptimizer
// actually expects.
float meshDiagonal(Ogre::Mesh* mesh)
{
    const auto& aab = mesh->getBounds();
    if (aab.isNull() || aab.isInfinite()) return 1.0f;
    const auto& mn = aab.getMinimum();
    const auto& mx = aab.getMaximum();
    const float dx = mx.x - mn.x;
    const float dy = mx.y - mn.y;
    const float dz = mx.z - mn.z;
    const float d2 = dx * dx + dy * dy + dz * dz;
    return d2 > 0.0f ? std::sqrt(d2) : 1.0f;
}

} // namespace

void destroyLevel(LodLevel& level)
{
    for (auto* idx : level.indices) {
        if (idx) {
            // Drop the underlying buffer first — IndexData's dtor
            // doesn't release the SharedPtr it holds, it relies on
            // the buffer being explicitly cleared or the IndexData
            // going out of scope. Setting indexBuffer to null
            // decrements the ref count cleanly.
            idx->indexBuffer.reset();
            OGRE_DELETE idx;
        }
    }
    level.indices.clear();
    level.actualReductions.clear();
}

std::vector<LodLevel> generateLods(Ogre::Mesh* mesh,
                                    const std::vector<float>& reductions,
                                    float errorBudget)
{
    std::vector<LodLevel> result;
    if (!mesh || reductions.empty()) return result;

    auto* logger = Ogre::LogManager::getSingletonPtr();
    const unsigned int numSubs = mesh->getNumSubMeshes();
    if (numSubs == 0) {
        if (logger) logger->logMessage("[MeshOptimizerLod] mesh has no submeshes");
        return result;
    }

    // Per-submesh source data — extracted once and reused across
    // every LOD level. simplify() reads positions + the source index
    // list and writes a fresh index list. `uvs` (UV0, 2 floats per
    // vertex) is optional: when present we route through
    // `simplifyWithAttributes` so collapses across UV seams get
    // penalized and Mixamo-style charts stay intact.
    struct SubMeshSource {
        std::vector<float>    positions;     // tight xyz per vertex
        std::vector<float>    uvs;           // tight uv per vertex (empty = no UV0)
        std::vector<uint32_t> indices;       // triangle list
        size_t                vertexCount = 0;
    };
    std::vector<SubMeshSource> sources(numSubs);

    Ogre::VertexData* sharedData = mesh->sharedVertexData;
    for (unsigned int s = 0; s < numSubs; ++s) {
        Ogre::SubMesh* sub = mesh->getSubMesh(s);
        Ogre::VertexData* vd = sub->useSharedVertices ? sharedData : sub->vertexData;
        sources[s].positions  = extractPositions(vd);
        sources[s].uvs        = extractUV0(vd);
        sources[s].indices    = extractIndices(sub->indexData);
        sources[s].vertexCount = vd ? vd->vertexCount : 0;
    }

    const float scale = meshDiagonal(mesh);
    const float targetError = errorBudget * scale;

    for (float reduction : reductions) {
        // Negative / zero ratios are nonsense; reject explicitly so we
        // don't waste a `meshopt_simplify` call. A reduction of 1.0
        // (collapse to a single triangle) is occasionally requested
        // — `count=4` with default fallbacks produces exactly that —
        // so clamp it just below 1.0 instead of dropping the level.
        if (reduction <= 0.0f) {
            if (logger) logger->logMessage(
                "[MeshOptimizerLod] skipping non-positive reduction " +
                std::to_string(reduction));
            continue;
        }
        if (reduction >= 1.0f) reduction = 0.99f;

        LodLevel level;
        level.indices.reserve(numSubs);
        level.actualReductions.reserve(numSubs);

        for (unsigned int s = 0; s < numSubs; ++s) {
            const auto& src = sources[s];
            const size_t targetIdxCount = static_cast<size_t>(
                src.indices.size() * (1.0f - reduction));
            // meshopt_simplify wants the target index count rounded
            // to a multiple of 3 (triangles).
            const size_t targetRounded = (targetIdxCount / 3) * 3;

            std::vector<uint32_t> simplified(src.indices.size());
            float resultError = 0.0f;
            size_t newIdxCount = 0;
            if (!src.uvs.empty() && src.uvs.size() == src.vertexCount * 2) {
                // UV-aware path: treat UV0 (s,t) as a 2-float attribute
                // stream with equal weights. Tuned at 0.5 each — high
                // enough to keep visible seams stable on character
                // textures, low enough not to dominate position error.
                const float attrWeights[2] = { 0.5f, 0.5f };
                newIdxCount = meshopt_simplifyWithAttributes(
                    simplified.data(),
                    src.indices.data(), src.indices.size(),
                    src.positions.data(), src.vertexCount, sizeof(float) * 3,
                    src.uvs.data(), sizeof(float) * 2,
                    attrWeights, /*attribute_count=*/2,
                    /*vertex_lock=*/nullptr,
                    targetRounded,
                    targetError,
                    /*options=*/0,
                    &resultError);
            } else {
                newIdxCount = meshopt_simplify(
                    simplified.data(),
                    src.indices.data(), src.indices.size(),
                    src.positions.data(), src.vertexCount, sizeof(float) * 3,
                    targetRounded,
                    targetError,
                    /*options=*/0,
                    &resultError);
            }
            simplified.resize(newIdxCount);

            // Run vertex-cache optimization on the reduced index
            // list — improves post-T&L cache hit rate without
            // changing geometry. Cheap (< 1ms on Mixamo-scale
            // meshes), no downside.
            meshopt_optimizeVertexCache(
                simplified.data(),
                simplified.data(), simplified.size(),
                src.vertexCount);

            const float actualReduction = src.indices.empty()
                ? 0.0f
                : 1.0f - static_cast<float>(newIdxCount) / static_cast<float>(src.indices.size());

            level.indices.push_back(buildIndexData(simplified));
            level.actualReductions.push_back(actualReduction);
        }
        result.push_back(std::move(level));
    }

    return result;
}

} // namespace MeshOptimizerLod
