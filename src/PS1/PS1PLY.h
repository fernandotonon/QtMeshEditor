/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef PS1PLY_H
#define PS1PLY_H

#include <OgreMesh.h>
#include <OgreEntity.h>

namespace Ogre {
class Pass;
}
#include <QColor>
#include <QString>
#include <QVector>

#include <array>

/**
 * Sony Psy-Q "PLY" polygon mesh (ASCII) — not Stanford PLY.
 *
 * Matches the text layout produced by common RSD toolchains and the
 * PlayStation-RSD-Blender exporter: @PLY header, vertex/normal counts,
 * vertices, normals (per-vertex then per-face), face lines (0=triangle,
 * 1=quad) with separate normal indices.
 *
 * Rendering: Ogre stores triangle index buffers, so quads from a PLY are expanded to
 * two triangles at import. Polygon topology (tri/quad and higher) is written with
 * the same `qtme.faces.<i>` n-gon binding as FBX (see EditableMesh / HalfEdgeMesh).
 */
namespace PS1PLY {

/// Uniform scale for RSD sidecar Psy-Q PLY geometry (kept at 1× so ring-style assets stay editor-sized).
constexpr float kPsyqPlyEditorUniformScale = 1.0f;

bool isPsyqPlyFile(const QString& filePath);

/** FFP pass for Psy-Q PLY / RSD materials (`matUnlit` = Blender MAT flag LSB). */
void configurePsyqRsdMaterialPass(Ogre::Pass* pass, bool hasVertexColour, bool matUnlit);

Ogre::MeshPtr importPsyqPly(const QString& filePath, const std::string& meshName);

/** Import with optional per-face colors (size must match face count). */
Ogre::MeshPtr importPsyqPlyWithFaceColors(const QString& filePath,
                                         const std::string& meshName,
                                         const QVector<QColor>& faceColors);

/** Per-face material binding for the textured import path. */
struct FaceMaterial {
    bool textured = false;        ///< true when the face references a texture slot.
    int textureIndex = -1;        ///< RSD TEX[] index (only used when `textured` is true).
    std::array<float, 4> u{};     ///< per-corner U (normalised 0..1, in PLY corner order v0..vN).
    std::array<float, 4> v{};     ///< per-corner V (normalised 0..1, top-origin).
    QColor color;                  ///< per-face flat colour fallback (used when no vertex colours are supplied).
    QVector<QColor> vertColors;   ///< 0, 3 or 4 per-corner colours (in PLY corner order); empty when N/A.
    bool unlit = false;            ///< Blender MAT flag LSB: no scene lighting (full-bright / PS1 no-light).
};

/**
 * Import with per-face material binding (UVs + texture index + colours).
 *
 * The mesh is split into one submesh per distinct (`textureIndex`, `unlit`) group.
 * Material names use suffix `_nl` when the MAT no-light (Blender "Unlit") bit is set.
 * The caller binds RSD textures to `_texN` / `_texN_nl` submeshes after import; this
 * routine only stores UVs on textured submesh vertices.
 *
 * `faceMaterials` length must match the PLY face count.
 */
Ogre::MeshPtr importPsyqPlyWithFaceMaterials(const QString& filePath,
                                             const std::string& meshName,
                                             const QVector<FaceMaterial>& faceMaterials);

/// Per-output-face UV + texture-slot info gathered alongside the PLY export.
struct ExportFaceTexture {
    bool textured = false;          ///< Source submesh has UVs + a bound texture.
    int  submeshIndex = -1;          ///< Submesh that produced this face (for RSD slot lookup).
    int  cornerCount = 3;            ///< 3 or 4 — matches the written PLY face shape.
    std::array<float, 4> u{};       ///< Per-corner U (0..1), zero-padded for tris.
    std::array<float, 4> v{};       ///< Per-corner V (0..1), zero-padded for tris.
    /// Per-corner colours (matches PLY corner order). Populated when the source submesh has
    /// a VES_DIFFUSE stream. Slots beyond `cornerCount` are default-constructed. Lets the
    /// caller emit smooth-shaded MAT entries (Psy-Q `G` / `H`) instead of averaging the
    /// corners into a single flat colour — preserves baked AO / vertex shading on round-trip.
    bool hasCornerColors = false;
    std::array<QColor, 4> cornerColors{};
};

/// Export an Ogre entity as Psy-Q PLY. Writes separate vertex and normal tables (counts
/// `nV` and `nN` may differ): positions and normals are welded independently by quantized
/// float, so shared 3D points can reuse one vertex index with distinct per-corner normals.
/// For a single submesh, if `readNgonFacesFromMesh` finds `qtme.faces.0`, Psy-Q face lines
/// follow those polygons
/// (tri / quad / n-gon fanned to tris); otherwise coplanar triangle pairs are merged heuristically.
/// If outFaceColors is provided and vertex colours exist on all submeshes, one RGB per
/// written face is filled (for a MAT sidecar). If outFaceTextures is provided, per-face
/// UV + submesh metadata is filled (for the textured MAT/RSD export path).
bool exportPsyqPlyFromEntity(const Ogre::Entity* entity,
                             const QString& plyPath,
                             QVector<QColor>* outFaceColors = nullptr,
                             QVector<ExportFaceTexture>* outFaceTextures = nullptr,
                             QString* outError = nullptr);

} // namespace PS1PLY

#endif
