#ifndef VERTEXCACHEOPTIMIZER_H
#define VERTEXCACHEOPTIMIZER_H

#include <QString>
#include <QList>
#include <QJsonObject>
#include <cstdint>
#include <vector>

namespace Ogre {
    class Entity;
}

// Per-submesh ACMR report. The ACMR (Average Cache Miss Ratio) measures the
// number of cache misses divided by the number of triangles in the index
// buffer — a perfect order is ~0.5 for a 32-entry post-T&L cache, while
// unordered indices typically score 1.5-3.0.
struct SubMeshCacheReport {
    QString meshName;
    int submeshIndex = 0;
    int triangleCount = 0;
    double acmrBefore = 0.0;
    double acmrAfter = 0.0;
    bool reordered = false; // true when the index buffer was actually rewritten
};

struct VertexCacheReport {
    QList<SubMeshCacheReport> submeshes;
    double weightedAcmrBefore = 0.0; // tri-weighted average across all submeshes
    double weightedAcmrAfter = 0.0;
    int totalTriangles = 0;
    int totalReordered = 0; // number of submeshes whose indices were rewritten

    double improvement() const {
        return weightedAcmrBefore > 0
                   ? (weightedAcmrBefore - weightedAcmrAfter) / weightedAcmrBefore * 100.0
                   : 0.0;
    }
};

// Pure-data optimizer. All methods are static and side-effect free over plain
// integer vectors. Ogre-backed wrappers below do the SubMesh I/O.
class VertexCacheOptimizer {
public:
    // Default post-T&L cache size used by Forsyth's heuristic. 32 is the
    // canonical value from Hugues Hoppe / Forsyth's paper and approximates
    // modern NVIDIA/AMD post-T&L caches well enough for ACMR comparison.
    static constexpr int kDefaultCacheSize = 32;

    // Reorder `indices` (a flat list of triangle vertex indices, 3 per tri)
    // in place using Forsyth's linear-time vertex-cache optimization.
    // `vertexCount` must be at least the max value in `indices` + 1.
    // Returns true when the function ran (false on empty / malformed input).
    static bool forsyth(std::vector<uint32_t>& indices, uint32_t vertexCount,
                        int cacheSize = kDefaultCacheSize);

    // Compute ACMR for an index buffer given a cache size. Returns 0.0 for
    // empty input. Uses an LRU eviction model — same convention Forsyth's
    // paper and meshoptimizer both use, so the numbers are comparable.
    static double computeAcmr(const std::vector<uint32_t>& indices,
                              int cacheSize = kDefaultCacheSize);

    // Ogre-backed convenience: analyze a single Entity's submeshes and
    // return a report with before/after ACMR. When `rewrite` is true, the
    // optimized index buffer is written back to Ogre (and `reordered` is
    // set on each submesh report). When false, the function only reads
    // and reports — pure analysis.
    static VertexCacheReport analyzeEntity(Ogre::Entity* entity, bool rewrite);

    // Merge `partial` into `aggregate` and recompute the running tri-
    // weighted ACMR totals. Used by every caller that walks multiple
    // entities (CLI cmdVertexCache + MCP toolOptimizeVertexCache).
    static void mergeReport(VertexCacheReport& aggregate,
                            const VertexCacheReport& partial);

    // Final post-merge step: divide weighted-ACMR sums by total triangles.
    // Idempotent on empty reports.
    static void finalize(VertexCacheReport& report);

    // Serialize a VertexCacheReport as JSON (CLI / MCP).
    static QJsonObject toJson(const VertexCacheReport& report);

    // Serialize as human-readable text (CLI default).
    static QString toText(const VertexCacheReport& report);
};

#endif // VERTEXCACHEOPTIMIZER_H
