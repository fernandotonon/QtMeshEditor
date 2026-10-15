#ifndef QUAD_REMESH_CORE_H
#define QUAD_REMESH_CORE_H

#include <QHash>
#include <QImage>
#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

// Field-aligned quad remeshing (QuadriFlow) — the pure-data half.
//
// QuadriFlow rebuilds the surface from scratch, so none of the source's
// per-vertex data survives on its own. Everything the user cares about is
// carried across by CLOSEST-POINT correspondence onto the ORIGINAL surface:
//   * skin weights   — barycentric blend of the source triangle's corner
//                       weights, top-4, renormalised (a rigged mesh stays
//                       rigged);
//   * textures       — the remeshed surface is unwrapped with xatlas using
//                       its QUAD faces (a chart seam can never cut a quad in
//                       two), then every texel samples the source texture at
//                       the closest source point's UV, per material slot;
//   * normal maps    — re-expressed from the source tangent frame into the
//                       new one (geometric detail lost to the coarser mesh is
//                       baked in too, so a mesh without a normal map gains a
//                       detail map when bakeDetailNormal is on).
//
// QuadriFlow needs a clean 2-manifold: seam-split vertices are position
// welded first, and a non-manifold input is rebuilt by a voxel pre-pass
// (narrow-band distance field + exterior flood fill + marching cubes) before
// the remesh. The remesh itself runs OUT OF PROCESS (QuadriFlowRunner) —
// QuadriFlow calls exit() on internal failures.
//
// Qt-only (QImage/QString): no Ogre, unit-tested headlessly.
namespace QuadRemesh {

struct BoneWeight {
    uint16_t bone = 0;
    float weight = 0.0f;
};

// One material of the source: its texture maps by canonical slot name
// ("diffuse_map", "albedo", "normal_map", "roughness", "metallic", "ao",
// "emissive", …). A slot named "normal_map" is treated as a tangent-space
// normal map; every other slot is resampled as colour data.
struct SourceMaterial {
    QHash<QString, QImage> maps;
};

struct SourceMesh {
    std::vector<float>    positions;   // Nx3
    std::vector<uint32_t> indices;     // Mx3
    std::vector<float>    normals;     // Nx3, optional
    std::vector<float>    uvs;         // Nx2, optional (texture transfer needs it)
    std::vector<std::vector<BoneWeight>> bones;   // N or empty
    std::vector<int>      triangleMaterial;       // M or empty (= all 0)
    std::vector<SourceMaterial> materials;
};

enum class VoxelMode { Auto, Always, Never };

struct Options {
    int  targetFaces = 5000;         // quads; QuadriFlow lands near it (see defaultTargetFaces)
    // Align to sharp creases + denser quads where curvature is high. Both ON:
    // measured on a Mixamo character at the same budget, mean deviation from
    // the source 0.24% -> 0.19% and max 3.3% -> 2.4% (Hip Hop: max 2.9 -> 1.9%).
    bool preserveSharp = true;
    bool preserveBoundary = false;
    bool adaptiveScale = true;
    int  seed = 0;
    VoxelMode voxel = VoxelMode::Auto;
    int  voxelResolution = 0;        // cells along the longest axis; 0 = auto
    bool bakeTextures = true;
    bool bakeDetailNormal = true;    // emit a normal map even without a source one
    int  textureSize = 0;            // longest side; 0 = largest source map (min 1024)
    int  dilatePx = 4;
    // Fit the remeshed vertices back onto the ORIGINAL surface: QuadriFlow
    // places them on its smoothed position field, which rounds off features.
    // Each iteration = tangential relaxation (keeps quads even) + snap to the
    // closest source point. 0 = keep QuadriFlow's positions; -1 (default) =
    // auto: 3 after a voxel rebuild (whose surface sits up to ~a voxel off
    // the source; measured mean deviation -20%), 0 otherwise (QuadriFlow's
    // vertices already lie on a clean source — fitting measured no gain).
    int  reprojectIterations = -1;
    int  timeoutMs = 5 * 60 * 1000;     // per helper attempt (a retry may follow)
    QString helperPath;              // empty = QuadriFlowRunner::defaultHelperPath()
    // (stage text, fraction 0..1 of the whole remesh) on each stage change,
    // plus ("", -1) as a ~10 Hz poll while the helper runs — a fraction < 0
    // carries no new progress. Return false to cancel. Called on the thread
    // that called remesh().
    std::function<bool(const QString& stage, double fraction)> progress;
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    QString error;

    // Output mesh (seam-split by the UV unwrap when one ran).
    std::vector<float>    positions;   // Nx3
    std::vector<float>    normals;     // Nx3 smooth, welded across UV seams
    std::vector<float>    uvs;         // Nx2 in [0,1], empty when not unwrapped
    std::vector<std::vector<uint32_t>> faces;   // quads (a few tris possible)
    std::vector<uint32_t> indices;     // fan triangulation of `faces`
    std::vector<std::vector<BoneWeight>> bones; // N or empty
    QHash<QString, QImage> maps;       // baked texture per slot

    // Report.
    int  inputTriangles = 0;
    int  weldedVertices = 0;
    int  nonManifoldEdges = 0;
    int  boundaryEdges = 0;
    bool voxelized = false;
    QString voxelReason;
    QString fallbackReason;            // set when a plain retry produced the result
    int  quads = 0;
    int  triangles = 0;
    int  vertexCount = 0;
    int  textureWidth = 0;
    int  textureHeight = 0;
    // Shape fidelity: distance from the source vertices to the remeshed
    // surface, as a fraction of the source bounding-box diagonal.
    float meanDeviation = 0.0f;
    float maxDeviation = 0.0f;
};

// Default quad budget for a mesh of `inputTriangles`: 0.6x the triangle
// count (quads cover ~2 triangles each, so this is ~1.2x the polygon
// density — enough to keep fingers/brims a half-density remesh collapsed),
// clamped to 1000..20000 (QuadriFlow stays in seconds there).
int defaultTargetFaces(int inputTriangles);

// ---- Building blocks (exposed for unit tests) ------------------------------

struct Topology {
    int boundaryEdges = 0;
    int nonManifoldEdges = 0;      // edges shared by 3+ faces
    int inconsistentEdges = 0;     // 2-face edges walked the same direction
    int nonManifoldVertices = 0;   // vertices whose fan is not one disc/strip
    bool manifold() const { return nonManifoldEdges == 0 && nonManifoldVertices == 0; }
};
Topology analyzeTopology(const std::vector<uint32_t>& indices, int vertexCount);

// Merge vertices closer than `relEps` × bbox diagonal; drops triangles that
// collapse. Returns the number of merged vertices.
int weldByPosition(std::vector<float>& positions, std::vector<uint32_t>& indices,
                   float relEps = 1e-6f);

// Rebuild a closed 2-manifold approximation of an arbitrary triangle soup.
// `resolution` = cells along the longest bbox axis (clamped 16..512).
bool voxelRemesh(const std::vector<float>& positions,
                 const std::vector<uint32_t>& indices, int resolution,
                 std::vector<float>& outPositions, std::vector<uint32_t>& outIndices,
                 QString* error = nullptr);

// Closest-point queries over a fixed triangle set.
class SurfaceLocator {
public:
    void build(const std::vector<float>& positions, const std::vector<uint32_t>& indices);
    bool valid() const { return !m_tris.empty(); }
    // Closest triangle + barycentrics to p. Returns false only when empty.
    bool closest(const float p[3], int& triangle, float bary[3], float closestPt[3]) const;

private:
    const std::vector<float>* m_pos = nullptr;
    const std::vector<uint32_t>* m_idx = nullptr;
    std::vector<int> m_tris;           // triangle ids, bucketed
    std::vector<int> m_cellStart;      // nx*ny*nz + 1
    float m_min[3] = {0, 0, 0};
    float m_cell = 1.0f;
    int m_n[3] = {1, 1, 1};
};

// Move `positions` (a polygon mesh) onto the `source` surface: `iterations`
// rounds of tangential Laplacian relaxation followed by a closest-point snap.
// A vertex whose closest source point is farther than `maxSnap` is left in
// place (it belongs to a region the remesh had to invent, e.g. a voxel-closed
// hole). Exposed for tests.
void fitToSurface(std::vector<float>& positions, const std::vector<std::vector<uint32_t>>& faces,
                  const SurfaceLocator& source, int iterations, float maxSnap);

// Mean / max distance from `samples` (xyz) to the surface (positions,
// indices), in absolute units.
void surfaceDeviation(const std::vector<float>& samples, const std::vector<float>& positions,
                      const std::vector<uint32_t>& indices, float& mean, float& max);

// Blend source corner weights at a barycentric point; top-4, renormalised.
std::vector<BoneWeight> blendBones(const std::vector<BoneWeight>* corners[3],
                                   const float bary[3]);

// ---- Pipeline ----------------------------------------------------------------

// Remesh `src` with QuadriFlow and carry its attributes across. Runs the
// helper process; blocking.
Result remesh(const SourceMesh& src, const Options& opts);

// The post-remesh half on its own (attribute transfer + unwrap + bake) for a
// quad mesh produced elsewhere — used by remesh() and directly by tests.
Result transferOnto(const SourceMesh& src,
                    const std::vector<float>& quadPositions,
                    const std::vector<std::vector<uint32_t>>& quadFaces,
                    const Options& opts);

} // namespace QuadRemesh

#endif // QUAD_REMESH_CORE_H
