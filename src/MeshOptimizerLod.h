#ifndef MESH_OPTIMIZER_LOD_H
#define MESH_OPTIMIZER_LOD_H

#include <cstdint>
#include <string>
#include <vector>
#include <OgreMesh.h>
#include <OgreSubMesh.h>

// Thin C++ facade around meshoptimizer for LOD generation. Lives
// next to Ogre's MeshLodGenerator-based path so callers (CLIPipeline,
// MeshLodController, MCPServer) can pick either backend at runtime
// via an `algo` parameter.
//
// Why this exists: Ogre's stock `MeshLodGenerator` produces visible
// seam artifacts on UV-mapped meshes and discards skinning weights
// when it collapses verts. meshoptimizer's `simplifyWithAttributes`
// takes UVs + bone weights as attribute streams so collapses preserve
// both. It's the same code path glTF tooling, Godot, and Unity use
// for asset-import simplification.
//
// First consumer: `qtmesh lod --algo meshopt` (#398). Future
// consumers (the mesh-quality epic #678 — vertex-cache reorder,
// overdraw optimization, vertex-fetch optimization, weld-dedupe)
// will land alongside this file as additional free functions.

namespace MeshOptimizerLod {

// Per-LOD result returned by generateLods. mLodFaceList in the
// caller's SubMesh is the canonical Ogre storage — this struct is
// the intermediate form we hand back so MeshLodController /
// CLIPipeline can decide whether to commit, export, or discard.
struct LodLevel
{
    // One IndexData per submesh, in submesh index order. The caller
    // owns these — they're allocated with `OGRE_NEW`. Pass directly
    // to `SubMesh::mLodFaceList[lod-1] = result.indices[s];` to wire
    // them into the Ogre mesh, or call destroyLevel() to drop them.
    std::vector<Ogre::IndexData*> indices;

    // The actual reduction ratio achieved per submesh (input_tris ->
    // output_tris / input_tris). meshoptimizer's simplify can stop
    // short of the requested target when topology + the configured
    // error budget make further reduction impossible — we report
    // what we actually got, not what was asked.
    std::vector<float> actualReductions;
};

// Free this LodLevel's IndexData* entries via OGRE_DELETE. Use after
// the caller has either committed the levels to mLodFaceList (in
// which case Ogre owns them) or decided to discard them.
void destroyLevel(LodLevel& level);

// Generate `reductions.size()` LOD levels for `mesh`. Each reduction
// is in [0.0, 1.0]: 0.5 means "keep 50% of the triangles". Per
// submesh, calls `meshopt_simplify` (preserves UV seams + skin
// boundaries via attribute-aware simplification) followed by
// `meshopt_optimizeVertexCache` (Forsyth-style reorder for GPU
// post-T&L cache).
//
// `errorBudget` (default 0.01 ≈ 1% of the mesh's bounding-sphere
// diameter) caps how far a vertex can move during collapse. Passed
// through to `meshopt_simplify`'s `target_error` arg. Lower values
// give safer (less destructive) LODs; higher values give more
// aggressive simplification.
//
// Returns one LodLevel per reduction. Empty vector on failure
// (logged via Ogre::LogManager).
std::vector<LodLevel> generateLods(Ogre::Mesh* mesh,
                                    const std::vector<float>& reductions,
                                    float errorBudget = 0.01f);

} // namespace MeshOptimizerLod

#endif
