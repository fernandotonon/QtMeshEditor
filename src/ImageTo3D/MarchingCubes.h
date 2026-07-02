#ifndef MARCHING_CUBES_H
#define MARCHING_CUBES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Host-side iso-surface extraction (marching cubes) — epic #764, slice A (#765).
//
// The image-to-3D epic reconstructs a NeRF-style *density grid* from a single
// image (TripoSR: image → triplane transformer → per-point density), and the
// surface has to be turned into a triangle mesh on the host. TripoSR does this
// with its own `MarchingCubeHelper` (torchmcubes, GPU); the QtMeshEditor
// codebase had no iso-surface code at all before this slice.
//
// This is a native, from-scratch port of the classic Lorensen–Cline marching
// cubes (SIGGRAPH 1987) — the 256-entry edge mask + triangle tables are a
// well-known public-domain data set (we author our own copy in the .cpp; nothing
// is vendored). Zero new dependencies, matching the project's native-heuristic
// stance for SkinWeights (#402, avoided GPL TetGen) and QuadRetopo (#401, avoided
// heavy GPL deps).
//
// Pure-data: no Ogre, no Qt-singleton — same shape as PbrMapSynth / SkinWeights so
// it unit-tests without a GL context. Slice C (#767) turns the returned float
// arrays into an Ogre::Mesh (normals, bounds, export); Slice B (#766) feeds this
// the density grid from the ONNX decoder.
namespace MarchingCubes {

// Extracted surface. `positions` is tightly packed xyz (3 floats/vertex);
// `indices` is triangle vertex indices (3/triangle), CCW when viewed from the
// side the field increases toward (outside, for an outward-positive field).
struct Mesh {
    std::vector<float>    positions;   // Nx3, world space (see gridMin/gridMax)
    std::vector<uint32_t> indices;     // Mx3
    int vertexCount   = 0;
    int triangleCount = 0;
};

// Extract the `isoLevel` iso-surface from a scalar field sampled on a regular
// nx*ny*nz grid.
//
//   field[z*ny*nx + y*nx + x]  — row-major (x fastest, z slowest)
//
// The grid's sample corners map linearly from index space onto the world-space
// box [gridMin, gridMax]: corner (x,y,z) sits at
//   gridMin + (i / (n-1)) * (gridMax - gridMin)   per axis,
// so emitted vertices are already in world space. A degenerate axis (n < 2) or a
// null field yields an empty mesh (never throws). The caller must supply
// nx*ny*nz samples; pass `fieldLength` (the actual buffer length) to make that
// enforceable — a non-zero `fieldLength` shorter than nx*ny*nz also yields an
// empty mesh instead of reading past the buffer (0 = caller guarantees the size).
//
// Convention for the density fields this epic feeds it: the surface is where the
// field crosses `isoLevel`, with the field taken to be POSITIVE INSIDE the
// object. TripoSR emits density and thresholds at 25.0 with an inside-positive
// sign; the caller passes `field = density` and `isoLevel = threshold` (or,
// equivalently, `field = density - threshold` and `isoLevel = 0`). The sign
// bookkeeping lives at the call site, not here.
//
// Vertices are welded across shared cube edges via an edge-key hash so the output
// is a connected indexed mesh (adjacent triangles share vertices), which keeps
// vertex counts sane and lets downstream normal computation accumulate face
// normals correctly.
Mesh extract(const float* field, int nx, int ny, int nz,
             float isoLevel,
             const std::array<float, 3>& gridMin,
             const std::array<float, 3>& gridMax,
             std::size_t fieldLength = 0);

} // namespace MarchingCubes

#endif // MARCHING_CUBES_H
