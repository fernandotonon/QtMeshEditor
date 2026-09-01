#ifndef TRELLIS2_BAKE_H
#define TRELLIS2_BAKE_H

#include <QImage>
#include <QString>

#include <cstdint>
#include <functional>
#include <unordered_map>
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
    // dual-grid callers set voxelSize/8 (the upstream reference weld): raw
    // dual-grid output is non-manifold seam soup at sub-voxel scale, and
    // without a voxel-scale weld the topology-preserving simplifier can get
    // stuck millions of triangles above the requested budget.
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
    // Taubin λ|μ smoothing passes applied to the welded mesh before
    // simplification (0 = off). The TRELLIS dual-grid callers enable this:
    // fuzzy subjects (fur/hair) decode with sub-voxel micro-pits that render
    // as dark speckle and derail QEM on thin double-walled features.
    int   taubinIterations = 0;
};

struct GameReadyResult {
    bool ok = false;
    QString error;
    std::vector<float>    positions;   // compacted
    std::vector<uint32_t> indices;
    int inputTriangles = 0;
    int outputTriangles = 0;
    int weldedVertices = 0;       // vertices merged by the weld
    int removedComponents = 0;    // debris islands dropped
    int removedTriangles = 0;     // triangles dropped with them (+degenerates)
    float simplifyError = 0.0f;   // meshopt result_error (0 when not simplified)
};

GameReadyResult makeGameReady(const std::vector<float>& positions,
                              const std::vector<uint32_t>& indices,
                              const GameReadyOptions& opts);

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
