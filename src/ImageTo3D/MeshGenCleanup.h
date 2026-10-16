#ifndef MESH_GEN_CLEANUP_H
#define MESH_GEN_CLEANUP_H

#include <cstdint>
#include <vector>

// Final topology clean-up for image-to-3D results (every backend: TripoSR,
// TripoSG, TRELLIS.2, Pixal3D). Runs on the finished Result, AFTER the
// game-ready simplify and the xatlas bake, because both of those are where
// new degenerates appear: the sloppy (clustering) simplifier collapses
// triangles to zero area without dropping them, and marching cubes emits
// slivers on grid-aligned surfaces whenever an edge crossing lands on a
// grid corner. makeGameReady's own pass only catches repeated INDICES
// before simplification, so a collinear or post-simplify sliver reached
// the export.
//
// Pure data (no Ogre/Qt) — unit-tested in MeshGenCleanup_test.cpp.
namespace MeshGenCleanup {

struct DegenerateReport {
    int repeatedIndex = 0;   // two corners share a vertex index
    int zeroArea      = 0;   // collinear / coincident corners (any index)
    int duplicate     = 0;   // same three vertices as an earlier triangle
    int removedVertices = 0; // vertices no surviving triangle references
    int total() const { return repeatedIndex + zeroArea + duplicate; }
};

// Drop degenerate triangles in place:
//   * repeated index (a==b, b==c, a==c);
//   * zero area: the triangle's height is below `relativeHeightEps` times
//     its own longest edge (scale-invariant, so it catches collinear
//     needles on a 1-unit mesh and a 1000-unit one alike), or the corners
//     coincide exactly;
//   * duplicates: the same unordered vertex triple as an earlier triangle
//     (either winding — a back-to-back pair over one triangle is
//     z-fighting, never intended geometry).
// Then compacts away vertices nothing references, remapping every
// per-vertex stream the caller passes (`perVertex` entries with their
// component count — positions are always compacted). Streams whose size
// does not match the vertex count are left untouched (an empty colours
// array, for instance).
struct PerVertexStream {
    std::vector<float>* data = nullptr;
    int components = 0;
};
DegenerateReport removeDegenerates(std::vector<float>& positions,
                                   std::vector<uint32_t>& indices,
                                   const std::vector<PerVertexStream>& perVertex = {},
                                   float relativeHeightEps = 1e-5f);

} // namespace MeshGenCleanup

#endif // MESH_GEN_CLEANUP_H
