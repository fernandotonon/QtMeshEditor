#ifndef MESH_GEN_BUILDER_H
#define MESH_GEN_BUILDER_H

#include "MeshGenPredictor.h"

#include <QString>

namespace Ogre {
    class SceneNode;
    class Mesh;
}

// Ogre-side construction for image-to-3D (epic #764, slice C #767). Turns a
// MeshGenPredictor::Result (raw float position/index/color arrays from marching
// cubes) into a loaded Ogre::Mesh with computed per-vertex normals + correct
// bounds, attaches it to the scene via Manager (so MeshImporterExporter::exporter
// can write it), and returns the parent SceneNode.
//
// Kept SEPARATE from MeshGenPredictor (which is deliberately Ogre-free +
// unit-testable) — this file is the only piece of the feature that touches Ogre.
namespace MeshGenBuilder {

// Build an Ogre::Mesh named `meshName` from the predictor result: POSITION +
// NORMAL (accumulated from face normals — marching-cubes output has none) +
// optional TEXCOORD0 (when `result.uvs` is populated — the baked-texture path)
// or DIFFUSE (VET_COLOUR) when `result.colors` is populated. Uses a 16-bit
// index buffer when vertexCount <= 65536, else 32-bit. Sets bounds + sphere
// radius and calls load(). Returns null on empty/degenerate input.
//
// Baked-texture path: when `result.texture` is non-null and `texturePngPath`
// points at the PNG it was saved to (the caller writes it — see buildSceneNode),
// a lit material binding that texture as diffuse is created and assigned.
Ogre::Mesh* buildMesh(const MeshGenPredictor::Result& result, const QString& meshName,
                      const QString& texturePngPath = {});

// Options for buildSceneNode's texture/material stage.
struct BuildOptions {
    // Where the baked PNG (and any synthesized PBR maps) land. Empty →
    // AppData/generated_textures/. The CLI passes the export target's
    // directory so the sidecars land next to the mesh.
    QString textureDir;
    // Chain #404 PBR map synthesis onto the baked diffuse: synthesize
    // normal + roughness PNGs next to it and bind them into the material's
    // canonical slots (normal_map / roughness) with the RTSS normal-map
    // sub-render-state applied — the same treatment as the Material Editor's
    // "Generate PBR maps from diffuse" button, which is what turns the flat
    // diffuse-only result into a polished, surface-detailed one. Ignored
    // when there is no baked texture; fails soft (diffuse-only material)
    // when the models are unavailable.
    bool generatePbrMaps = false;
};

// buildMesh + create a child SceneNode under the scene root with an entity on it
// (via Manager::createEntity), ready to hand to MeshImporterExporter::exporter.
// Returns the SceneNode (owns the entity) or null on failure.
//
// When the result carries a baked texture, it is saved as `<unique>_diffuse.png`
// into `opts.textureDir` (default: AppData/generated_textures/) and that
// directory is registered as an Ogre resource location so both the viewport
// material and the exporters (FBX Video.Content embed / glTF reference) can
// resolve it — plus the optional PBR stage above.
Ogre::SceneNode* buildSceneNode(const MeshGenPredictor::Result& result,
                                const QString& baseName,
                                const BuildOptions& opts = {});

} // namespace MeshGenBuilder

#endif // MESH_GEN_BUILDER_H
