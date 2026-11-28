#pragma once
// Mesh-buffer quality analysis and lossless fixes (issue #678).
//
// Pure data: plain arrays in, plain arrays out, no Ogre. The scan adapter
// (ScanEngine) reads each submesh's buffers into a `Geometry`, calls
// `analyze` for the scan rules and `fix` for `qtmesh scan --fix-mesh`.
//
// The three ordering metrics (vertex cache, overdraw, vertex fetch) depend
// on the mesh's shape as much as on its order: a mesh with real depth
// complexity can never reach an overdraw of 1.0. So the scan rules do not
// compare them to an absolute ceiling; they compare the current value to the
// value meshoptimizer reaches on the same mesh and warn when the GAIN is
// large (see `Metrics::*Gain`). The absolute ACMR ceiling stays available
// as the older `max_acmr` rule.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace MeshQuality {

struct Geometry {
    // xyz per vertex (vertexCount * 3 floats).
    std::vector<float> positions;
    std::vector<uint32_t> indices;   // triangle list
    size_t vertexCount = 0;
    // Bytes the GPU fetches per vertex (sum of every stream's stride); used
    // by the vertex-fetch metric only. 0 = 12 (position only).
    size_t vertexBytes = 0;
    // Optional per-vertex key: every attribute that must match for two
    // vertices to be interchangeable (all stream bytes + bone weights).
    // Empty = vertices are compared by position only.
    std::vector<unsigned char> keys;
    size_t keyStride = 0;
};

// Thresholds and caps shared by analyze() and fix().
struct Limits {
    // A triangle is degenerate when two corners share an index or its
    // height over the longest edge is below this fraction of that edge
    // (scale-invariant; the same rule as MeshGenCleanup).
    double degenerateRelativeHeight = 1e-5;
    // meshopt_analyzeOverdraw rasterises every triangle; above this many
    // triangles the overdraw metric is skipped (reported as -1).
    size_t maxOverdrawTriangles = 1000000;
    // ACMR is measured with this post-transform cache size (32 = the size
    // `qtmesh vertex-cache` and ExportOptimizer report with).
    unsigned int cacheSize = 32;
    float overdrawThreshold = 1.05f;   // meshopt_optimizeOverdraw threshold
};

struct Metrics {
    size_t triangles = 0;
    size_t vertices = 0;
    // Below the relative-height threshold (or a repeated corner): what the
    // warning counts.
    size_t degenerateTriangles = 0;
    // What the lossless fix may remove: exactly flat triangles, or (on
    // skinned/morphed geometry, where a flat base-pose triangle can open up)
    // only those with a repeated corner.
    size_t zeroAreaTriangles = 0;
    size_t repeatedCornerTriangles = 0;
    size_t redundantVertices = 0;     // exact duplicates of another referenced vertex
    size_t unreferencedVertices = 0;

    double acmr = 0.0;                // current
    // After optimizeVertexCache. The overdraw fix deliberately gives back up
    // to ~5 % of this (meshoptimizer threshold 1.05), so a file fixed for
    // overdraw can still show a small ACMR gain.
    double acmrOptimized = 0.0;
    double overdraw = -1.0;           // shaded / covered pixels; -1 = skipped
    double overdrawOptimized = -1.0;
    // Fetched bytes / referenced vertex bytes, best 1.0. 0 = not measured
    // (meshoptimizer only analyses vertices up to 256 bytes).
    double overfetch = 0.0;
    double overfetchOptimized = 0.0;
    size_t vertexBytes = 0;           // bytes per vertex the fetch metric used (0 = not measured)

    size_t referencedVertices() const
    {
        return vertices > unreferencedVertices ? vertices - unreferencedVertices : 0;
    }

    // Relative improvement the matching fix would bring, in percent
    // (0 when already optimal or not measured).
    double acmrGainPct() const { return gainPct(acmr, acmrOptimized); }
    double overdrawGainPct() const { return gainPct(overdraw, overdrawOptimized); }
    double overfetchGainPct() const { return gainPct(overfetch, overfetchOptimized); }
    double redundantVertexPct() const
    {
        return vertices ? 100.0 * double(redundantVertices) / double(vertices) : 0.0;
    }
    double degenerateTrianglePct() const
    {
        return triangles ? 100.0 * double(degenerateTriangles) / double(triangles) : 0.0;
    }

    static double gainPct(double before, double after)
    {
        if (!(before > 0.0) || !(after >= 0.0) || after >= before)
            return 0.0;
        return 100.0 * (before - after) / before;
    }
};

// Measure one submesh. Never modifies the input.
Metrics analyze(const Geometry& g, const Limits& limits = {});

// Which fixes to run. Every one except `simplifyTargetTriangles` is
// lossless: the rendered surface is unchanged.
struct FixOptions {
    // Drop exactly flat triangles (repeated corner or zero cross product).
    // Slivers that only fall below Limits::degenerateRelativeHeight still
    // cover pixels and are kept.
    bool removeDegenerates = false;
    // Only drop triangles with a repeated corner index. For skinned or
    // morphed geometry a zero-area triangle in the base pose can open up
    // once deformed, so only the index test is lossless there.
    bool degeneratesByIndexOnly = false;
    bool weld = false;           // merge exact duplicate vertices
    bool vertexCache = false;
    bool overdraw = false;
    bool vertexFetch = false;    // reorder the vertex buffer by first use (drops unreferenced vertices)
    // > 0: simplify submeshes above this many triangles down to it
    // (meshopt_simplify; destructive, opt-in only).
    size_t simplifyTargetTriangles = 0;
    float simplifyMaxError = 0.02f;   // relative to the mesh extent

    bool any() const
    {
        return removeDegenerates || weld || vertexCache || overdraw || vertexFetch
            || simplifyTargetTriangles > 0;
    }
    // True when the vertex buffer must be rewritten (not just the indices).
    bool remapsVertices() const { return weld || vertexFetch; }
};

struct FixResult {
    std::vector<uint32_t> indices;    // new triangle list, in the new vertex numbering
    // Old vertex -> new vertex, ~0u for a vertex the new buffer drops.
    // Empty when the vertex buffer is unchanged (index-only fixes).
    std::vector<uint32_t> remap;
    size_t newVertexCount = 0;

    size_t degeneratesRemoved = 0;
    size_t verticesWelded = 0;
    size_t verticesDropped = 0;       // unreferenced vertices removed by the fetch remap
    size_t trianglesBefore = 0;
    size_t trianglesAfter = 0;
    bool simplified = false;
    bool changed = false;             // anything differs from the input
};

FixResult fix(const Geometry& g, const FixOptions& opt, const Limits& limits = {});

// Rewrite a vertex stream with a FixResult::remap (stride bytes per vertex).
// Vertices whose remap is ~0u are dropped; when two old vertices map to one
// new vertex the first one wins (welded vertices are byte-identical anyway).
std::vector<unsigned char> remapStream(const unsigned char* data, size_t vertexCount,
                                       size_t stride, const std::vector<uint32_t>& remap,
                                       size_t newVertexCount);

// Is triangle (a, b, c) degenerate under `limits`? Exposed for tests.
bool isDegenerate(const Geometry& g, uint32_t a, uint32_t b, uint32_t c, const Limits& limits = {});

} // namespace MeshQuality
