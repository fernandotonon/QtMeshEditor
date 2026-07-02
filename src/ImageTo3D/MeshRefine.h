#ifndef MESH_REFINE_H
#define MESH_REFINE_H

#include <cstddef>
#include <cstdint>
#include <vector>

// Post-extraction mesh refinement for image-to-3D (epic #764, quality pass).
//
// Marching cubes on a res^3 density grid quantizes the surface to the grid:
// the output shows stair-stepping and per-cell faceting that reads as "AI
// blob" next to the smooth, crisp output of commercial services. Two cheap,
// classical fixes recover most of the visible gap:
//
//  * taubinSmooth — Taubin λ|μ smoothing (Taubin, SIGGRAPH 1995): alternating
//    positive/negative-weight Laplacian passes that remove the high-frequency
//    grid noise WITHOUT the volume shrinkage of plain Laplacian smoothing.
//    This is the same post-MC treatment TripoSR-derived pipelines apply.
//
//  * isoProjectStep — one Newton step per vertex toward the true iso-surface:
//    Δ = -f·∇f / |∇f|², clamped to `maxStep`. After smoothing has dragged
//    vertices slightly off the level set, one projection step (with the field
//    + gradient sampled from the ONNX decoder at the smoothed positions) puts
//    them back ON the network's actual surface, recovering detail that the
//    MC grid quantized away. The decoder sampling is orchestrated by the
//    caller (MeshGenPredictor) so this stays pure-data + unit-testable.
//
// Pure-data: no Ogre, no Qt, no ONNX — same shape as MarchingCubes /
// PbrMapSynth so it unit-tests without a GL context.
namespace MeshRefine {

// Taubin λ|μ smoothing, in place. `positions` is tightly packed xyz,
// `indices` triangle vertex indices (3/triangle). One iteration = one λ
// (shrink) pass followed by one μ (inflate) pass over the uniform-weight
// vertex Laplacian. Classic stable parameters: lambda=0.5, mu=-0.53.
// Degenerate input (empty mesh, out-of-range indices) is a no-op.
void taubinSmooth(std::vector<float>& positions,
                  const std::vector<uint32_t>& indices,
                  int iterations = 6,
                  float lambda = 0.5f,
                  float mu = -0.53f);

// One Newton projection step toward field == 0, in place. `f` holds the
// signed field value per vertex (inside-positive, like the thresholded
// TripoSR density), `grad` the field gradient per vertex (Nx3, same layout
// as positions). Each vertex moves by Δ = -f·g/|g|², with |Δ| clamped to
// `maxStep` (pass ~one grid cell) and vertices with a vanishing gradient
// (|g|² < 1e-12) left untouched. Sizes must agree (N, N, Nx3) — mismatched
// input is a no-op.
void isoProjectStep(std::vector<float>& positions,
                    const std::vector<float>& f,
                    const std::vector<float>& grad,
                    float maxStep);

} // namespace MeshRefine

#endif // MESH_REFINE_H
