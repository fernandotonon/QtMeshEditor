#ifndef MESH_GEN_BAKER_H
#define MESH_GEN_BAKER_H

#include <QImage>
#include <QString>

#include <cstdint>
#include <functional>
#include <vector>

// Texture baking for image-to-3D (epic #764, quality pass).
//
// TripoSR's colour lives in the decoder as a per-POINT radiance query. Slice C
// surfaced it as per-VERTEX colour (VET_COLOUR), which reads blurry: colour
// resolution is capped by the marching-cubes vertex density, and every engine
// interpolates it linearly across triangles. Commercial services bake a real
// diffuse TEXTURE instead — colour resolution becomes the texture resolution,
// independent of the geometry. This module is that bake:
//
//   1. xatlas auto-unwrap of the generated mesh (unique, non-overlapping UVs —
//      the same MIT library the #400 UvUnwrap feature already vendors);
//   2. rasterize every chart triangle in UV space; for each covered texel,
//      barycentrically interpolate the 3D surface position;
//   3. batch-sample the colour at those positions through `ColorSampler`
//      (MeshGenPredictor wires this to the TripoSR decoder's color output —
//      the same chunked query loop the vertex-colour pass uses);
//   4. dilate the chart borders a few texels so bilinear filtering / MIPs
//      don't bleed background into the seams.
//
// Pure-data + callback: no Ogre, no ONNX — unit-testable with an analytic
// sampler. MeshGenBuilder turns the result into an Ogre mesh with UV0 + a
// textured material; the CLI/GUI write the QImage next to the exported mesh.
namespace MeshGenBaker {

// Batched colour query: fill `outRgb` (count*3 floats, [0,1]) for `count`
// tightly-packed xyz points. Return false to abort the bake (cancel/error).
using ColorSampler =
    std::function<bool(const float* points, size_t count, float* outRgb)>;

struct Result {
    bool ok = false;
    bool cancelled = false;   // sampler aborted (typed — don't string-match error)
    QString error;
    // Re-indexed mesh (xatlas splits vertices along chart seams): same layout
    // as MeshGenPredictor::Result, plus a UV channel.
    std::vector<float>    positions;   // Nx3
    std::vector<uint32_t> indices;     // Mx3
    std::vector<float>    uvs;         // Nx2 in [0,1] (V *not* flipped)
    int vertexCount   = 0;
    int triangleCount = 0;
    QImage texture;                    // baked diffuse (textureSize², RGB)
};

struct Options {
    int textureSize = 1024;   // square bake target
    int dilatePx    = 4;      // chart-border dilation (texels)
    // Points per sampler call (bounds the query buffer; same idea as
    // MeshGenPredictor::Options::chunkPoints).
    int chunkPoints = 262144;
    // Optional per-chunk progress: (texelsDone, texelsTotal); return false to
    // CANCEL (bake returns cancelled, no partial data). The baker knows the
    // true texel total, which the sampler owner doesn't.
    std::function<bool(int done, int total)> progress;
};

// Unwrap + bake. `positions`/`indices` describe the input triangle mesh
// (typically a MeshGenPredictor::Result). Fails (ok=false, no partial data)
// on degenerate input, xatlas failure, or a sampler abort.
Result bake(const std::vector<float>& positions,
            const std::vector<uint32_t>& indices,
            const ColorSampler& sampler,
            const Options& opts = {});

} // namespace MeshGenBaker

#endif // MESH_GEN_BAKER_H
