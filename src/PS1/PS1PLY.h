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
#include <QColor>
#include <QString>

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

Ogre::MeshPtr importPsyqPly(const QString& filePath, const std::string& meshName);

/** Import with optional per-face colors (size must match face count). */
Ogre::MeshPtr importPsyqPlyWithFaceColors(const QString& filePath,
                                         const std::string& meshName,
                                         const QVector<QColor>& faceColors);

/// Export an Ogre entity as Psy-Q PLY. Writes separate vertex and normal tables (counts
/// `nV` and `nN` may differ): positions and normals are welded independently by quantized
/// float, so shared 3D points can reuse one vertex index with distinct per-corner normals.
/// For a single submesh, if `readNgonFacesFromMesh` finds `qtme.faces.0`, Psy-Q face lines
/// follow those polygons
/// (tri / quad / n-gon fanned to tris); otherwise coplanar triangle pairs are merged heuristically.
/// If outFaceColors is provided and vertex colours exist on all submeshes, one RGB per
/// written face is filled (for a MAT sidecar).
bool exportPsyqPlyFromEntity(const Ogre::Entity* entity,
                             const QString& plyPath,
                             QVector<QColor>* outFaceColors = nullptr,
                             QString* outError = nullptr);

} // namespace PS1PLY

#endif
