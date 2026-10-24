#ifndef TRELLIS2_BAKE_H
#define TRELLIS2_BAKE_H

#include <QImage>
#include <QString>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <limits>
#include <vector>

// TRELLIS.2 game-ready processing + multi-channel PBR texture baking — the
// QtMeshEditor-native replacement for the functionality the upstream
// reference implementation delegates to NVIDIA nvdiffrast/nvdiffrec (both
// under the NVIDIA Source Code License, research/evaluation only — excluded
// from this project; see docs/trellis2-dependencies.md).
//
// This is ORIGINAL QtMeshEditor code built from standard, publicly documented
// graphics algorithms and the project's existing permissive dependencies:
//   * welding / simplification         — meshoptimizer (MIT, already used)
//   * tiny-component removal           — MeshSegmenter::connectedComponents
//   * UV unwrap                        — xatlas (MIT, already used)
//   * UV-space triangle rasterization  — scanline barycentric coverage (the
//     MeshGenBaker #764 approach, extended to multi-channel)
//   * source-surface correspondence    — closest point on triangle (Ericson,
//     "Real-Time Collision Detection") over a sparse uniform grid
//   * attribute lookup                 — trilinear interpolation over the
//     sparse voxel attribute volume TRELLIS.2 generates
//   * normal map                       — source smooth normal expressed in
//     the target's Lengyel tangent frame (OpenGL +Y-up convention, matching
//     NormalMapGenerator's default)
// No NVIDIA source was read, copied, translated or linked for any of it.
//
// Pure data (Qt-only, no Ogre/GL/ONNX) — unit-tested headlessly in
// Trellis2Bake_test.cpp.
namespace Trellis2Bake {

// ---- Sparse PBR attribute volume ------------------------------------------
// TRELLIS.2 emits per-occupied-voxel attributes: base_color.rgb, metallic,
// roughness, alpha (6 × u8). Sampling is trilinear over the occupied
// neighbours (weights renormalized over present voxels — the volume only
// exists in a shell around the surface), with a nearest-occupied fallback.
class SparseVolumeSampler {
public:
    // coords: Lx3 integer voxel coordinates; attrs: Lx6 (order above).
    // Voxel centre of (i,j,k) sits at origin + (ijk + 0.5) * voxelSize.
    void build(const uint32_t* coords, const uint8_t* attrs, int count,
               float voxelSize, const float origin[3]);
    bool valid() const { return m_count > 0; }
    // Sample at a world-space (TRELLIS-space) point. out[6] in 0..1.
    // Returns false (and writes neutral defaults) when nothing occupied is
    // anywhere near the point.
    bool sample(const float p[3], float out[6]) const;

private:
    std::unordered_map<uint64_t, int32_t> m_map;
    const uint8_t* m_attrs = nullptr;
    int m_count = 0;
    float m_voxelSize = 1.0f;
    float m_origin[3] = {-0.5f, -0.5f, -0.5f};
};

// ---- Closest point on triangle (exposed for unit tests) --------------------
// Standard Ericson closest-point-on-triangle. Writes the closest point and
// its barycentric coordinates (w.r.t. a,b,c).
void closestPointOnTriangle(const float a[3], const float b[3],
                            const float c[3], const float p[3],
                            float outClosest[3], float outBary[3]);

// ---- Game-ready processing (Phase 8) ---------------------------------------
struct GameReadyOptions {
    // Near-duplicate weld tolerance as a fraction of the bbox diagonal
    // (0 disables the epsilon and welds bit-identical positions only).
    float weldEpsilonFraction = 1e-5f;
    // Absolute weld tolerance — overrides the fraction when > 0. The TRELLIS
    // dual-grid callers set voxelSize/64 (see Trellis2Predictor). Keep
    // it SMALL: the raw output is already closed, and a coarse (voxelSize/8)
    // weld creates non-manifold edges and floating fragments.
    float weldEpsilonAbsolute = 0.0f;
    // Disconnected components smaller than BOTH thresholds are dropped
    // (floating debris from the sparse-voxel extraction). The largest
    // component is always kept.
    int   minComponentTriangles = 16;
    float minComponentFraction  = 0.002f;   // × total triangle count
    // 0 = keep original density. Otherwise meshopt_simplify toward this
    // triangle count (no exact-count promise — border locking can stop
    // earlier; the achieved count is in the result).
    int   targetTriangles = 0;
    float simplifyTargetError = 0.01f;      // relative to bbox extent
    // targetTriangles is a HARD CEILING (a platform upload limit — Roblox
    // refuses a MeshPart past 20k / an accessory past 4k) rather than a
    // budget: when the error-capped and uncapped QEM passes both leave the
    // count above the target, finish with the topology-free sloppy
    // simplifier, which always reaches it. Off = the ordinary pass, which
    // accepts landing up to 2x above the target (see makeGameReady).
    bool  strictTriangleBudget = false;
    // Taubin λ|μ smoothing passes applied to the welded mesh before
    // simplification (0 = off). The TRELLIS dual-grid callers enable this:
    // fuzzy subjects (fur/hair) decode with sub-voxel micro-pits that render
    // as dark speckle and derail QEM on thin double-walled features.
    int   taubinIterations = 0;
    // > 0: rebuild the welded mesh as a CLOSED MANIFOLD surface before
    // simplification — voxelize at this resolution (cells along the longest
    // bbox axis), flood-fill the exterior, blur the solid mask and run
    // marching cubes. Removes every non-manifold edge (thin double walls fuse
    // into one slab) at the cost of detail below ~1/remeshResolution of the
    // model size; the dense source still feeds the detail-normal bake.
    // 0 = off. Falls back to the unmodified mesh when the remesh is empty or
    // not manifold.
    int   remeshResolution = 0;
    // Also return the welded, debris-culled, winding-UNIFIED source (before any
    // remesh/simplify) in GameReadyResult::weldedPositions/Indices. Feed THAT
    // to bake() as the dense source: the raw dual-grid mesh has large flipped
    // patches, and both the source normals the detail map is built from and
    // the orientation-aware closest-triangle search read the winding.
    bool  exportWeldedSource = false;
};

struct GameReadyResult {
    bool ok = false;
    QString error;
    std::vector<float>    positions;   // compacted
    std::vector<uint32_t> indices;
    int inputTriangles = 0;
    int outputTriangles = 0;
    int weldedVertices = 0;       // vertices merged by the weld
    std::vector<float>    weldedPositions;   // see exportWeldedSource
    std::vector<uint32_t> weldedIndices;
    int removedComponents = 0;    // debris islands dropped
    int removedTriangles = 0;     // triangles dropped with them (+degenerates)
    bool remeshed = false;        // voxel remesh replaced the mesh
    int remeshedFromTriangles = 0;
    int splitVertices = 0;        // vertices duplicated to separate pinched sheets
    float simplifyError = 0.0f;   // meshopt result_error (0 when not simplified)
};

GameReadyResult makeGameReady(const std::vector<float>& positions,
                              const std::vector<uint32_t>& indices,
                              const GameReadyOptions& opts);

// Edge-topology census of an indexed triangle mesh (positions are not read —
// connectivity is by index, so weld first). Used to tell WHERE holes come
// from: a closed surface has boundaryEdges == 0 and nonManifoldEdges == 0.
struct TopologyStats {
    int triangles = 0;
    int boundaryEdges = 0;      // edges used by exactly one face (hole rims)
    int nonManifoldEdges = 0;   // edges used by 3+ faces
    int flippedEdges = 0;       // 2-face edges whose faces disagree on winding
    int components = 0;         // connected components (vertex-connected)
    int smallComponents = 0;    // components under 1% of the triangles
};
TopologyStats topologyStats(size_t vertexCount,
                            const std::vector<uint32_t>& indices);

// Voxel remesh (see GameReadyOptions::remeshResolution). Exposed for tests.
// Returns false (outputs untouched) when the input is degenerate or the
// result is empty.
bool remeshManifold(const std::vector<float>& positions,
                    const std::vector<uint32_t>& indices, int resolution,
                    std::vector<float>& outPositions,
                    std::vector<uint32_t>& outIndices);

// Quadric edge-collapse simplification that enforces the link condition, so a
// closed manifold mesh stays manifold. Collapses until `targetTriangles`
// remain or no legal collapse is left. Returns true (outputs always valid).
// `maxCost` stops the pass once the CHEAPEST remaining collapse costs more than
// this quadric error: when the cheap legal collapses run out (thin tubes and
// sheets lock under the link condition) the queue otherwise keeps taking ever
// more destructive ones to reach the count — measured on a real generation the
// cost rises ~1000x over the last 2k triangles and the surface shreds. The
// result then stays above the target; the caller decides what to do about it.
bool simplifyManifold(std::vector<float>& positions,
                      std::vector<uint32_t>& indices, size_t targetTriangles,
                      double maxCost = std::numeric_limits<double>::infinity());

// Make a mesh edge-manifold WITHOUT moving geometry or opening holes: every
// edge shared by 4+ faces (two sheets pinched together by simplification) is
// resolved by duplicating the pinched vertices so each sheet owns its own.
// Faces around an edge are ordered by angle and the two bounding each SOLID
// wedge are paired, so parts that merely touch end up as separate bodies.
// Edges that cannot be paired (odd face count, inconsistent winding) are left
// as they are. Returns the number of vertices added.
int splitNonManifold(std::vector<float>& positions,
                     std::vector<uint32_t>& indices);

// Smooth shading normals for the (xatlas-split) output mesh. `xref[v]` names the
// original target vertex output vertex v came from. Smoothing runs across UV
// chart seams as usual, but only over original-mesh faces within ~100 degrees of
// the faces the output vertex itself uses: an original vertex shared by
// opposite-facing sheets (the rim of a thin slab, two sheets a cell apart)
// would otherwise average them into a sideways or cancelled normal.
std::vector<float> creaseAwareVertexNormals(
    const std::vector<float>& targetPositions,
    const std::vector<uint32_t>& targetIndices,
    const std::vector<uint32_t>& xref,
    const std::vector<uint32_t>& outIndices, int vertexCount);

// Unify triangle winding (exposed for unit tests; makeGameReady runs it).
// Raw TRELLIS dual-grid output arrives with large patches of flipped
// triangles (~17% conflicting directed edges measured on a real generation)
// — under backface culling the flipped patches simply vanish, and they
// corrupt area-weighted smooth normals. BFS over the 2-manifold edge graph
// flips faces to a consistent orientation; each connected component is then
// oriented OUTWARD by its signed volume. Non-manifold edges (3+ faces) are
// not used for propagation. Returns the number of faces flipped.
int unifyWinding(const std::vector<float>& positions,
                 std::vector<uint32_t>& indices);

// ---- Multi-channel bake (Phase 7) ------------------------------------------
struct BakeOptions {
    int  textureSize  = 2048;   // clamped to [64, 8192]
    int  dilatePx     = 4;      // chart-border dilation passes
    int  supersample  = 1;      // 1 or 2 (2 = 2×2 subsamples per texel)
    bool bakeNormalMap = true;  // bake source detail normals (for simplified targets)
    // Laplacian smoothing iterations applied to the SOURCE normal field
    // before it feeds the detail-normal bake (0 = off, the default). Raw
    // dual-grid dumps carried voxel-scale normal noise that baked into a
    // glittery normal map; the runtime now receives the REMESHED shell, so
    // by default the field is used as-is — smoothing a clean source would
    // instead bake curvature disagreement (rounded source vs one-ring
    // target normals) into the map. Raise this only when baking from a raw
    // (un-remeshed) dual-grid source.
    int  sourceNormalSmoothIterations = 0;
    // done/total covered texels; return false to cancel.
    std::function<bool(int done, int total)> progress;
};

struct BakeResult {
    bool ok = false;
    bool cancelled = false;
    QString error;
    // Target mesh re-indexed by xatlas (chart seams split), with UV0.
    std::vector<float>    positions;
    std::vector<uint32_t> indices;
    std::vector<float>    uvs;         // [0,1], V not flipped (MeshGenBaker convention)
    // Position-welded smooth shading normals for the re-indexed target (the
    // same ones the bake's tangent frames used). Exporting these instead of
    // recomputing from the seam-split mesh keeps chart seams smooth.
    std::vector<float>    normals;     // Nx3
    int vertexCount = 0;
    int triangleCount = 0;
    QImage baseColor;   // RGBA8888 (alpha channel from the volume)
    QImage roughness;   // Grayscale8
    QImage metallic;    // Grayscale8
    QImage normalMap;   // RGB888 tangent-space, OpenGL +Y up (null if disabled)
};

// Bake the SOURCE surface's TRELLIS.2 attributes onto the (possibly
// simplified) TARGET mesh: unwrap target with xatlas, rasterize each chart in
// UV space, project every covered texel to the closest point on the source
// surface, and trilinearly sample the sparse attribute volume there. Standard
// offline texture baking — no differentiable rendering involved.
BakeResult bake(const std::vector<float>& targetPositions,
                const std::vector<uint32_t>& targetIndices,
                const std::vector<float>& sourcePositions,
                const std::vector<uint32_t>& sourceIndices,
                const SparseVolumeSampler& volume,
                const BakeOptions& opts);

// ---- Detail-normal bake onto EXISTING UVs (TripoSR/TripoSG game-ready) -----
// The high-poly → low-poly workflow for the local ONNX backends: the target
// was already unwrapped + diffuse-baked (MeshGenBaker), so this bakes ONLY a
// tangent-space detail normal map into that SAME atlas — sampling smooth
// normals from the dense pre-simplification source mesh. Seam-split vertices
// are position-welded for the target's shading normals so chart seams don't
// read as hard edges. Same rasterizer/dilation as bake().
struct NormalBakeResult {
    bool ok = false;
    bool cancelled = false;
    QString error;
    QImage normalMap;   // RGB888 tangent-space, OpenGL +Y up (W×H as given)
};

NormalBakeResult bakeDetailNormal(const std::vector<float>& targetPositions,
                                  const std::vector<uint32_t>& targetIndices,
                                  const std::vector<float>& targetUvs, // Nx2 [0,1]
                                  int width, int height,
                                  const std::vector<float>& sourcePositions,
                                  const std::vector<uint32_t>& sourceIndices,
                                  const BakeOptions& opts);

} // namespace Trellis2Bake

#endif // TRELLIS2_BAKE_H
