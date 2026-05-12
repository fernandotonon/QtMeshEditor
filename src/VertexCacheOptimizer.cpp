#include "VertexCacheOptimizer.h"

#include <Ogre.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>
#include <OgreHardwareBufferManager.h>
#include <OgreHardwareIndexBuffer.h>

#include <QJsonArray>
#include <QLocale>
#include <algorithm>
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
    float cache[kMaxCachePos + 1] = {};
    ScoreTable() {
        // Slots 0..2 are the most-recently-emitted three vertices of the
        // current triangle — they share the same flat score (Forsyth).
        cache[0] = cache[1] = cache[2] = kLastTriScore;
        for (int i = 3; i <= kMaxCachePos; ++i) {
            const float p = (kMaxCachePos - i) / static_cast<float>(kMaxCachePos - 3);
            cache[i] = std::pow(p, kCacheDecayPow);
        }
    }
};
static const ScoreTable g_scoreTable;

float vertexScore(int cachePosition, int remainingValence)
{
    if (remainingValence <= 0) return -1.0f;  // already fully used
    float s = (cachePosition < 0) ? 0.0f
              : (cachePosition < kMaxCachePos ? g_scoreTable.cache[cachePosition] : 0.0f);
    s += kValenceBoostScl
       * std::pow(static_cast<float>(remainingValence), -kValenceBoostPow);
    return s;
}

} // namespace

bool VertexCacheOptimizer::forsyth(std::vector<uint32_t>& indices, uint32_t vertexCount,
                                   int cacheSize)
{
    if (indices.empty() || indices.size() % 3 != 0 || vertexCount == 0) return false;
    if (cacheSize > kMaxCachePos) cacheSize = kMaxCachePos;

    const size_t triangleCount = indices.size() / 3;

    // Per-vertex remaining-valence count (number of triangles still pending).
    std::vector<int> valence(vertexCount, 0);
    for (uint32_t v : indices) {
        if (v >= vertexCount) return false;
        ++valence[v];
    }

    // Per-vertex active-triangle list, packed CSR-style.
    std::vector<int> triOffset(vertexCount + 1, 0);
    for (uint32_t v = 0; v < vertexCount; ++v)
        triOffset[v + 1] = triOffset[v] + valence[v];

    std::vector<int> triList(triOffset.back(), 0);
    {
        std::vector<int> cursor(vertexCount, 0);
        for (size_t t = 0; t < triangleCount; ++t) {
            for (size_t j = 0; j < 3; ++j) {
                const uint32_t v = indices[t * 3 + j];
                triList[triOffset[v] + cursor[v]++] = static_cast<int>(t);
            }
        }
    }

    // Per-vertex cache position (-1 = not in cache).
    std::vector<int> cachePos(vertexCount, -1);
    // Per-triangle "already emitted" flag.
    std::vector<uint8_t> emitted(triangleCount, 0);
    // Per-vertex current score (lazy update — only re-computed when valence
    // or cache state changes).
    std::vector<float> vScore(vertexCount, 0.0f);
    for (uint32_t v = 0; v < vertexCount; ++v)
        vScore[v] = vertexScore(-1, valence[v]);
    // Per-triangle score (sum of its three vertex scores).
    std::vector<float> tScore(triangleCount, 0.0f);
    for (size_t t = 0; t < triangleCount; ++t) {
        tScore[t] = vScore[indices[t * 3]]
                  + vScore[indices[t * 3 + 1]]
                  + vScore[indices[t * 3 + 2]];
    }

    // LRU cache.
    std::vector<int32_t> cache;
    cache.reserve(cacheSize + 3);

    std::vector<uint32_t> output;
    output.reserve(indices.size());

    int bestTri = -1;
    float bestScore = -1.0f;
    // First seed: scan the whole triangle list once.
    for (size_t t = 0; t < triangleCount; ++t) {
        if (tScore[t] > bestScore) { bestScore = tScore[t]; bestTri = static_cast<int>(t); }
    }

    while (bestTri >= 0) {
        // Emit the triangle.
        emitted[bestTri] = 1;
        for (size_t j = 0; j < 3; ++j) {
            const uint32_t v = indices[bestTri * 3 + j];
            output.push_back(v);
            --valence[v];
        }

        // Move the triangle's three vertices to cache front.
        for (size_t j = 0; j < 3; ++j) {
            const int32_t v = static_cast<int32_t>(indices[bestTri * 3 + j]);
            auto it = std::find(cache.begin(), cache.end(), v);
            if (it != cache.end()) cache.erase(it);
            cache.insert(cache.begin(), v);
        }
        // Evict overflow and clear their cachePos.
        if (static_cast<int>(cache.size()) > cacheSize) {
            for (size_t i = cacheSize; i < cache.size(); ++i)
                cachePos[cache[i]] = -1;
            cache.resize(cacheSize);
        }
        // Refresh cache positions.
        for (size_t i = 0; i < cache.size(); ++i)
            cachePos[cache[i]] = static_cast<int>(i);

        // Re-score every vertex still in the cache.
        for (int32_t v : cache) vScore[v] = vertexScore(cachePos[v], valence[v]);

        // Re-score every triangle still pending that references any cache vert.
        bestScore = -1.0f;
        int nextBest = -1;
        for (int32_t v : cache) {
            const int begin = triOffset[v];
            const int end = triOffset[v + 1];
            for (int k = begin; k < end; ++k) {
                const int t = triList[k];
                if (emitted[t]) continue;
                tScore[t] = vScore[indices[t * 3]]
                          + vScore[indices[t * 3 + 1]]
                          + vScore[indices[t * 3 + 2]];
                if (tScore[t] > bestScore) { bestScore = tScore[t]; nextBest = t; }
            }
        }
        // Fallback: nothing in cache references a pending triangle — scan all.
        // Rare but correct (small disjoint islands).
        if (nextBest < 0) {
            for (size_t t = 0; t < triangleCount; ++t) {
                if (emitted[t]) continue;
                if (tScore[t] > bestScore) { bestScore = tScore[t]; nextBest = static_cast<int>(t); }
            }
        }
        bestTri = nextBest;
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

    for (uint32_t v : indices) {
        auto it = std::find(cache.begin(), cache.end(), static_cast<int32_t>(v));
        if (it == cache.end()) {
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
VertexCacheReport VertexCacheOptimizer::analyzeEntity(Ogre::Entity* entity, bool rewrite)
{
    VertexCacheReport report;
    if (!entity) return report;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return report;

    const QString meshName = QString::fromStdString(mesh->getName());

    for (unsigned si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub) continue;
        Ogre::IndexData* id = sub->indexData;
        if (!id || !id->indexBuffer || id->indexCount < 3 || id->indexCount % 3 != 0) continue;

        const bool use16 = (id->indexBuffer->getType() == Ogre::HardwareIndexBuffer::IT_16BIT);

        // Copy the index buffer into a uint32 vector so the optimizer can work
        // in a single uniform format.
        std::vector<uint32_t> indices(id->indexCount);
        {
            const void* src = id->indexBuffer->lock(Ogre::HardwareBuffer::HBL_READ_ONLY);
            if (use16) {
                const auto* in = static_cast<const uint16_t*>(src);
                for (size_t i = 0; i < id->indexCount; ++i) indices[i] = in[id->indexStart + i];
            } else {
                const auto* in = static_cast<const uint32_t*>(src);
                for (size_t i = 0; i < id->indexCount; ++i) indices[i] = in[id->indexStart + i];
            }
            id->indexBuffer->unlock();
        }

        Ogre::VertexData* vd = sub->useSharedVertices ? mesh->sharedVertexData : sub->vertexData;
        const uint32_t vertexCount = vd ? static_cast<uint32_t>(vd->vertexCount) : 0;

        SubMeshCacheReport sr;
        sr.meshName = meshName;
        sr.submeshIndex = static_cast<int>(si);
        sr.triangleCount = static_cast<int>(indices.size() / 3);
        sr.acmrBefore = computeAcmr(indices);

        if (rewrite && vertexCount > 0) {
            std::vector<uint32_t> reordered = indices; // forsyth() mutates in place
            if (forsyth(reordered, vertexCount)) {
                sr.acmrAfter = computeAcmr(reordered);
                // Only write back if it actually improved — never regress.
                if (sr.acmrAfter < sr.acmrBefore) {
                    void* dst = id->indexBuffer->lock(Ogre::HardwareBuffer::HBL_NORMAL);
                    if (use16) {
                        auto* out = static_cast<uint16_t*>(dst);
                        for (size_t i = 0; i < reordered.size(); ++i)
                            out[id->indexStart + i] = static_cast<uint16_t>(reordered[i]);
                    } else {
                        auto* out = static_cast<uint32_t*>(dst);
                        for (size_t i = 0; i < reordered.size(); ++i)
                            out[id->indexStart + i] = reordered[i];
                    }
                    id->indexBuffer->unlock();
                    sr.reordered = true;
                    ++report.totalReordered;
                } else {
                    sr.acmrAfter = sr.acmrBefore;
                }
            } else {
                sr.acmrAfter = sr.acmrBefore;
            }
        } else {
            sr.acmrAfter = sr.acmrBefore;
        }

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
