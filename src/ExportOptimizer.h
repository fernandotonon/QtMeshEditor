#ifndef EXPORT_OPTIMIZER_H
#define EXPORT_OPTIMIZER_H

#include <QString>
#include <QList>
#include <QJsonObject>
#include <cstdint>
#include <vector>

namespace Ogre {
    class Entity;
    class Mesh;
    class SceneNode;
}

// On-export mesh optimization pipeline (issue #399, epic #397).
//
// Runs the three cheap meshoptimizer post-processing passes on each
// submesh of an entity (or whole scene), in this order:
//
//   1. optimizeVertexCache    — Forsyth-style reorder for the post-T&L
//                               cache. Drops ACMR roughly 30-50%.
//   2. optimizeOverdraw       — re-order triangles so the front-to-back
//                               draw order minimises GPU overdraw, with
//                               a small threshold (1.05) that trades a
//                               tiny ACMR regression for a bigger
//                               overdraw win.
//   3. optimizeVertexFetch    — rewrite the vertex buffer (and remap the
//                               index buffer) so post-cache vertex
//                               reads are sequential, improving the
//                               GPU's pre-T&L cache hit rate.
//
// All three are O(n) and cost a few microseconds per submesh on
// Mixamo-scale assets. The default is to run all three on every
// export; the caller can disable via `OptimizeFlags::None` or
// `MeshImporterExporter::exporter(..., optimizeOnExport=false)`.
//
// Why a new module instead of extending VertexCacheOptimizer:
// VertexCacheOptimizer is the surface for the user-invoked
// `qtmesh vertex-cache` / MCP `optimize_vertex_cache` analysis tools
// and carries the legacy custom-Forsyth implementation. The on-export
// path is a separate concern — silent, fast, runs every time — so it
// lives in its own file to keep the responsibility boundary clear.

enum class OptimizeFlags : uint32_t {
    None         = 0,
    VertexCache  = 1u << 0,
    Overdraw     = 1u << 1,
    VertexFetch  = 1u << 2,
    All          = VertexCache | Overdraw | VertexFetch,
};

inline OptimizeFlags operator|(OptimizeFlags a, OptimizeFlags b)
{
    return static_cast<OptimizeFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline OptimizeFlags operator&(OptimizeFlags a, OptimizeFlags b)
{
    return static_cast<OptimizeFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline bool any(OptimizeFlags f) { return static_cast<uint32_t>(f) != 0; }

// Per-submesh before/after report.
struct ExportOptimizeSubMeshReport {
    QString meshName;
    int     submeshIndex   = 0;
    int     triangleCount  = 0;
    int     vertexCount    = 0;
    double  acmrBefore     = 0.0;
    double  acmrAfter      = 0.0;
    bool    vertexCacheRun = false;
    bool    overdrawRun    = false;
    bool    vertexFetchRun = false;
};

struct ExportOptimizeReport {
    QList<ExportOptimizeSubMeshReport> submeshes;
    double weightedAcmrBefore = 0.0;
    double weightedAcmrAfter  = 0.0;
    int    totalTriangles     = 0;
    int    submeshesOptimized = 0;

    double improvementPct() const {
        return weightedAcmrBefore > 0.0
            ? (weightedAcmrBefore - weightedAcmrAfter) / weightedAcmrBefore * 100.0
            : 0.0;
    }
    bool empty() const { return submeshes.isEmpty(); }
};

class ExportOptimizer {
public:
    // ACMR cache size used in before/after measurements. 32 matches the
    // canonical Forsyth paper figure and meshoptimizer's own default,
    // so the reported numbers line up with `qtmesh vertex-cache`.
    static constexpr int kCacheSize = 32;
    // Overdraw threshold tuned per meshoptimizer's docs: 1.05 trades
    // ~5% extra ACMR for a meaningful overdraw reduction; higher
    // values regress ACMR too much.
    static constexpr float kOverdrawThreshold = 1.05f;

    // Run the configured optimizers on every submesh of the entity.
    // The mesh buffers are rewritten in place; the caller is
    // responsible for the export step that flushes them to disk.
    // Returns a report aggregating before/after ACMR per submesh.
    static ExportOptimizeReport optimizeEntity(Ogre::Entity* entity,
                                               OptimizeFlags flags = OptimizeFlags::All);

    // Walks every Entity attached to `node` (recursively) and applies
    // optimizeEntity to each. Used by the scene-export path. The
    // returned report aggregates across all entities.
    static ExportOptimizeReport optimizeSceneNode(const Ogre::SceneNode* node,
                                                  OptimizeFlags flags = OptimizeFlags::All);

    // Pure-data ACMR helper that doesn't touch Ogre. Mirrors
    // `VertexCacheOptimizer::computeAcmr` (uses meshoptimizer's
    // analyseVertexCache so the numbers match across tools).
    static double computeAcmr(const std::vector<uint32_t>& indices,
                              uint32_t vertexCount);

    // JSON / text serializers (CLI + MCP consumers).
    static QJsonObject toJson(const ExportOptimizeReport& report);
    static QString     toText(const ExportOptimizeReport& report);
};

#endif // EXPORT_OPTIMIZER_H
