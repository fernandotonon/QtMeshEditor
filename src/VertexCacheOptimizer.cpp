#include "VertexCacheOptimizer.h"

#include <Ogre.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>
#include <OgreHardwareBufferManager.h>
#include <OgreHardwareIndexBuffer.h>

#include <QJsonArray>
#include <QLocale>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// ============================================================================
//   Forsyth's "Linear-Speed Vertex Cache Optimisation"
//
//   Tom Forsyth, 2006 — http://eelpi.gotdns.org/papers/fast_vert_cache_opt.html
//
//   Heuristic: assign each vertex a score combining its current LRU-cache
//   position (recent = better) with its remaining valence (fewer triangles
//   left = better). Each triangle's score is the sum of its three vertex
//   scores. On every step pick the highest-scoring triangle, emit it, update
//   the cache, and re-score the affected vertices. Linear time in triangle
//   count because the per-vertex active-triangle list only ever shrinks.
// ============================================================================

namespace {

constexpr int    kMaxCachePos     = 32;       // Highest cache slot Forsyth's bonus indexes
constexpr float  kCacheDecayPow   = 1.5f;
constexpr float  kLastTriScore    = 0.75f;
constexpr float  kValenceBoostScl = 2.0f;
constexpr float  kValenceBoostPow = 0.5f;

// Pre-compute the per-cache-position score so the inner loop is a table lookup.
struct ScoreTable {
    std::array<float, kMaxCachePos + 1> cache{};
    ScoreTable() {
        // Slots 0..2 are the most-recently-emitted three vertices of the
        // current triangle — they share the same flat score (Forsyth).
        cache[0] = cache[1] = cache[2] = kLastTriScore;
        for (int i = 3; i <= kMaxCachePos; ++i) {
            const float p = static_cast<float>(kMaxCachePos - i)
                          / static_cast<float>(kMaxCachePos - 3);
            cache[i] = std::pow(p, kCacheDecayPow);
        }
    }
};
const ScoreTable g_scoreTable;

float cachePositionScore(int cachePosition)
{
    if (cachePosition < 0) return 0.0f;
    if (cachePosition >= kMaxCachePos) return 0.0f;
    return g_scoreTable.cache[cachePosition];
}

float vertexScore(int cachePosition, int remainingValence)
{
    if (remainingValence <= 0) return -1.0f;  // already fully used
    float s = cachePositionScore(cachePosition);
    s += kValenceBoostScl
       * std::pow(static_cast<float>(remainingValence), -kValenceBoostPow);
    return s;
}

// Build a CSR-style per-vertex active-triangle list. triOffset[v] is the start
// of v's entries in triList; triOffset[v+1] - triOffset[v] is v's valence.
void buildTriangleAdjacency(const std::vector<uint32_t>& indices,
                            uint32_t vertexCount,
                            std::vector<int>& valence,
                            std::vector<int>& triOffset,
                            std::vector<int>& triList)
{
    const size_t triangleCount = indices.size() / 3;
    valence.assign(vertexCount, 0);
    for (uint32_t v : indices) ++valence[v];

    triOffset.assign(vertexCount + 1, 0);
    for (uint32_t v = 0; v < vertexCount; ++v)
        triOffset[v + 1] = triOffset[v] + valence[v];

    triList.assign(triOffset.back(), 0);
    std::vector<int> cursor(vertexCount, 0);
    for (size_t t = 0; t < triangleCount; ++t) {
        for (size_t j = 0; j < 3; ++j) {
            const uint32_t v = indices[t * 3 + j];
            triList[triOffset[v] + cursor[v]++] = static_cast<int>(t);
        }
    }
}

// Move v to the front of the LRU cache (moving / inserting as needed).
void cachePushFront(std::vector<int32_t>& cache, int32_t v)
{
    if (const auto it = std::find(cache.begin(), cache.end(), v); it != cache.end())
        cache.erase(it);
    cache.insert(cache.begin(), v);
}

// Evict any cache entries past `cacheSize`, mark them not-in-cache, and
// refresh cachePos[] for whatever remains.
void enforceCacheCapacity(std::vector<int32_t>& cache, int cacheSize,
                          std::vector<int>& cachePos)
{
    if (static_cast<int>(cache.size()) > cacheSize) {
        for (size_t i = cacheSize; i < cache.size(); ++i)
            cachePos[cache[i]] = -1;
        cache.resize(cacheSize);
    }
    for (size_t i = 0; i < cache.size(); ++i)
        cachePos[cache[i]] = static_cast<int>(i);
}

// Sum the three vertex scores of triangle `t`.
float triangleScore(size_t t, const std::vector<uint32_t>& indices,
                    const std::vector<float>& vScore)
{
    return vScore[indices[t * 3]]
         + vScore[indices[t * 3 + 1]]
         + vScore[indices[t * 3 + 2]];
}

// Pick the highest-scoring pending triangle: first scan the triangles
// adjacent to cached vertices (the fast path Forsyth's algorithm normally
// hits), then fall back to a full scan if every cached vertex's neighbors
// are already emitted (rare — small disjoint islands).
int findNextBestTriangle(const std::vector<int32_t>& cache,
                         const std::vector<int>& triOffset,
                         const std::vector<int>& triList,
                         const std::vector<uint8_t>& emitted,
                         const std::vector<uint32_t>& indices,
                         const std::vector<float>& vScore,
                         std::vector<float>& tScore)
{
    float bestScore = -1.0f;
    int bestTri = -1;
    for (const int32_t v : cache) {
        for (int k = triOffset[v]; k < triOffset[v + 1]; ++k) {
            const int t = triList[k];
            if (emitted[t]) continue;
            tScore[t] = triangleScore(t, indices, vScore);
            if (tScore[t] > bestScore) { bestScore = tScore[t]; bestTri = t; }
        }
    }
    if (bestTri >= 0) return bestTri;

    for (size_t t = 0; t < emitted.size(); ++t) {
        if (emitted[t]) continue;
        if (tScore[t] > bestScore) { bestScore = tScore[t]; bestTri = static_cast<int>(t); }
    }
    return bestTri;
}

} // namespace

bool VertexCacheOptimizer::forsyth(std::vector<uint32_t>& indices, uint32_t vertexCount,
                                   int cacheSize)
{
    if (indices.empty() || indices.size() % 3 != 0 || vertexCount == 0) return false;
    if (cacheSize > kMaxCachePos) cacheSize = kMaxCachePos;

    // Bounds check the input before any allocation — out-of-range index is
    // a fatal input error.
    for (const uint32_t v : indices) {
        if (v >= vertexCount) return false;
    }

    const size_t triangleCount = indices.size() / 3;

    std::vector<int> valence;
    std::vector<int> triOffset;
    std::vector<int> triList;
    buildTriangleAdjacency(indices, vertexCount, valence, triOffset, triList);

    // Per-vertex state.
    std::vector<int> cachePos(vertexCount, -1);
    std::vector<float> vScore(vertexCount, 0.0f);
    for (uint32_t v = 0; v < vertexCount; ++v)
        vScore[v] = vertexScore(-1, valence[v]);

    // Per-triangle state.
    std::vector<uint8_t> emitted(triangleCount, 0);
    std::vector<float> tScore(triangleCount, 0.0f);
    for (size_t t = 0; t < triangleCount; ++t)
        tScore[t] = triangleScore(t, indices, vScore);

    // LRU cache + output buffer.
    std::vector<int32_t> cache;
    cache.reserve(static_cast<size_t>(cacheSize) + 3);
    std::vector<uint32_t> output;
    output.reserve(indices.size());

    // Seed the loop with the best-scoring triangle in the initial state.
    int bestTri = -1;
    {
        float bestScore = -1.0f;
        for (size_t t = 0; t < triangleCount; ++t) {
            if (tScore[t] > bestScore) {
                bestScore = tScore[t];
                bestTri = static_cast<int>(t);
            }
        }
    }

    while (bestTri >= 0) {
        emitted[bestTri] = 1;
        for (size_t j = 0; j < 3; ++j) {
            const uint32_t v = indices[bestTri * 3 + j];
            output.push_back(v);
            --valence[v];
            cachePushFront(cache, static_cast<int32_t>(v));
        }
        enforceCacheCapacity(cache, cacheSize, cachePos);

        // Re-score every vertex still in the cache (only their scores change).
        for (const int32_t v : cache)
            vScore[v] = vertexScore(cachePos[v], valence[v]);

        bestTri = findNextBestTriangle(cache, triOffset, triList, emitted,
                                       indices, vScore, tScore);
    }

    if (output.size() != indices.size()) return false; // sanity guard
    indices.swap(output);
    return true;
}

double VertexCacheOptimizer::computeAcmr(const std::vector<uint32_t>& indices, int cacheSize)
{
    if (indices.empty() || indices.size() % 3 != 0) return 0.0;
    if (cacheSize <= 0) return 0.0;

    std::vector<int32_t> cache;
    cache.reserve(cacheSize + 1);
    size_t misses = 0;

    for (const uint32_t v : indices) {
        if (const auto it = std::find(cache.begin(), cache.end(),
                                      static_cast<int32_t>(v));
            it == cache.end()) {
            ++misses;
            cache.insert(cache.begin(), static_cast<int32_t>(v));
            if (static_cast<int>(cache.size()) > cacheSize)
                cache.pop_back();
        } else {
            cache.erase(it);
            cache.insert(cache.begin(), static_cast<int32_t>(v));
        }
    }
    return static_cast<double>(misses) / static_cast<double>(indices.size() / 3);
}

// ----- Ogre-backed wrapper ---------------------------------------------------

// LCOV_EXCL_START — Ogre-only branch, covered indirectly by manual / CLI tests

namespace {

// Read an Ogre IndexData into a uint32 vector. Handles 16/32-bit index types
// transparently so the optimizer sees a single uniform format. The buffer is
// locked HBL_READ_ONLY, so a pointer-to-const is sufficient.
std::vector<uint32_t> readIndexBuffer(const Ogre::IndexData* id)
{
    std::vector<uint32_t> indices(id->indexCount);
    const bool use16 = (id->indexBuffer->getType() == Ogre::HardwareIndexBuffer::IT_16BIT);
    const void* src = id->indexBuffer->lock(Ogre::HardwareBuffer::HBL_READ_ONLY);
    if (use16) {
        const auto* in = static_cast<const uint16_t*>(src);
        for (size_t i = 0; i < id->indexCount; ++i) indices[i] = in[id->indexStart + i];
    } else {
        const auto* in = static_cast<const uint32_t*>(src);
        for (size_t i = 0; i < id->indexCount; ++i) indices[i] = in[id->indexStart + i];
    }
    id->indexBuffer->unlock();
    return indices;
}

// Write a uint32 vector back into an Ogre IndexData, narrowing to 16-bit
// when the underlying buffer is 16-bit (Forsyth never introduces new indices,
// so all values fit in the existing buffer's width).
void writeIndexBuffer(Ogre::IndexData* id, const std::vector<uint32_t>& indices)
{
    const bool use16 = (id->indexBuffer->getType() == Ogre::HardwareIndexBuffer::IT_16BIT);
    void* dst = id->indexBuffer->lock(Ogre::HardwareBuffer::HBL_NORMAL);
    if (use16) {
        auto* out = static_cast<uint16_t*>(dst);
        for (size_t i = 0; i < indices.size(); ++i)
            out[id->indexStart + i] = static_cast<uint16_t>(indices[i]);
    } else {
        auto* out = static_cast<uint32_t*>(dst);
        for (size_t i = 0; i < indices.size(); ++i)
            out[id->indexStart + i] = indices[i];
    }
    id->indexBuffer->unlock();
}

// Analyze a single submesh's index buffer. When `rewrite` is true AND the
// reorder strictly improves ACMR, write the reordered indices back through
// Ogre and set `sr.reordered = true`.
void analyzeSubMesh(const Ogre::MeshPtr& mesh, unsigned si, bool rewrite,
                    SubMeshCacheReport& sr)
{
    const Ogre::SubMesh* sub = mesh->getSubMesh(si);
    if (!sub) return;
    Ogre::IndexData* id = sub->indexData;
    if (!id || !id->indexBuffer || id->indexCount < 3 || id->indexCount % 3 != 0) return;

    std::vector<uint32_t> indices = readIndexBuffer(id);

    const Ogre::VertexData* vd = sub->useSharedVertices ? mesh->sharedVertexData
                                                        : sub->vertexData;
    const uint32_t vertexCount = vd ? static_cast<uint32_t>(vd->vertexCount) : 0;

    sr.submeshIndex = static_cast<int>(si);
    sr.triangleCount = static_cast<int>(indices.size() / 3);
    sr.acmrBefore = VertexCacheOptimizer::computeAcmr(indices);
    sr.acmrAfter = sr.acmrBefore;

    if (vertexCount == 0) return;

    // Always run Forsyth on a local copy so the report shows the projected
    // ACMR even in analyze-only mode. `reordered` flips to true only when
    // bytes actually changed on the underlying buffer.
    std::vector<uint32_t> reordered = indices;
    if (!VertexCacheOptimizer::forsyth(reordered, vertexCount)) return;

    sr.acmrAfter = VertexCacheOptimizer::computeAcmr(reordered);
    if (rewrite && sr.acmrAfter < sr.acmrBefore) {
        writeIndexBuffer(id, reordered);
        sr.reordered = true;
    }
}

} // namespace

void VertexCacheOptimizer::mergeReport(VertexCacheReport& aggregate,
                                       const VertexCacheReport& partial)
{
    for (const SubMeshCacheReport& sr : partial.submeshes) {
        aggregate.submeshes.append(sr);
        aggregate.totalTriangles += sr.triangleCount;
        aggregate.weightedAcmrBefore += sr.acmrBefore * sr.triangleCount;
        aggregate.weightedAcmrAfter  += sr.acmrAfter  * sr.triangleCount;
        if (sr.reordered) ++aggregate.totalReordered;
    }
}

void VertexCacheOptimizer::finalize(VertexCacheReport& report)
{
    if (report.totalTriangles > 0) {
        report.weightedAcmrBefore /= report.totalTriangles;
        report.weightedAcmrAfter  /= report.totalTriangles;
    }
}

VertexCacheReport VertexCacheOptimizer::analyzeEntity(Ogre::Entity* entity, bool rewrite)
{
    VertexCacheReport report;
    if (!entity) return report;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return report;

    const QString meshName = QString::fromStdString(mesh->getName());

    for (unsigned si = 0; si < mesh->getNumSubMeshes(); ++si) {
        SubMeshCacheReport sr;
        sr.meshName = meshName;
        analyzeSubMesh(mesh, si, rewrite, sr);
        if (sr.triangleCount == 0) continue;

        if (sr.reordered) ++report.totalReordered;
        report.submeshes.append(sr);
        report.totalTriangles += sr.triangleCount;
        report.weightedAcmrBefore += sr.acmrBefore * sr.triangleCount;
        report.weightedAcmrAfter  += sr.acmrAfter  * sr.triangleCount;
    }

    if (report.totalTriangles > 0) {
        report.weightedAcmrBefore /= report.totalTriangles;
        report.weightedAcmrAfter  /= report.totalTriangles;
    }
    return report;
}
// LCOV_EXCL_STOP

// ----- Serialisation --------------------------------------------------------

QJsonObject VertexCacheOptimizer::toJson(const VertexCacheReport& report)
{
    QJsonObject obj;

    QJsonArray submeshes;
    for (const SubMeshCacheReport& sr : report.submeshes) {
        QJsonObject so;
        so["mesh"] = sr.meshName;
        so["submeshIndex"] = sr.submeshIndex;
        so["triangleCount"] = sr.triangleCount;
        so["acmrBefore"] = sr.acmrBefore;
        so["acmrAfter"] = sr.acmrAfter;
        so["reordered"] = sr.reordered;
        submeshes.append(so);
    }
    obj["submeshes"] = submeshes;

    QJsonObject totals;
    totals["totalTriangles"] = report.totalTriangles;
    totals["acmrBefore"] = report.weightedAcmrBefore;
    totals["acmrAfter"] = report.weightedAcmrAfter;
    totals["improvementPercent"] = report.improvement();
    totals["submeshesReordered"] = report.totalReordered;
    obj["totals"] = totals;

    return obj;
}

QString VertexCacheOptimizer::toText(const VertexCacheReport& report)
{
    QString out;
    QTextStream s(&out);
    QLocale locale;

    s << "Vertex Cache Analysis\n";
    s << "=====================\n\n";

    if (report.submeshes.isEmpty()) {
        s << "(no submeshes)\n";
        return out;
    }

    for (const SubMeshCacheReport& sr : report.submeshes) {
        s << "  " << sr.meshName << " [" << sr.submeshIndex << "]"
          << "  tris=" << locale.toString(sr.triangleCount)
          << "  ACMR " << QString::number(sr.acmrBefore, 'f', 3)
          << " → "    << QString::number(sr.acmrAfter,  'f', 3)
          << (sr.reordered ? "  (reordered)" : "")
          << "\n";
    }

    s << "\n";
    s << "Total triangles: " << locale.toString(report.totalTriangles) << "\n";
    s << "Weighted ACMR:   "
      << QString::number(report.weightedAcmrBefore, 'f', 3)
      << " → "
      << QString::number(report.weightedAcmrAfter,  'f', 3);
    if (report.weightedAcmrBefore > 0) {
        s << "  (" << QString::number(report.improvement(), 'f', 1) << "% improvement)";
    }
    s << "\n";
    s << "Submeshes rewritten: " << report.totalReordered
      << " of " << report.submeshes.size() << "\n";

    return out;
}
