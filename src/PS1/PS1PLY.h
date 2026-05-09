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

/// Export an Ogre entity as Psy-Q PLY. If outFaceColors is provided and vertex colours exist,
/// one averaged RGB entry per written face is appended (for writing a MAT sidecar).
bool exportPsyqPlyFromEntity(const Ogre::Entity* entity,
                             const QString& plyPath,
                             QVector<QColor>* outFaceColors = nullptr,
                             QString* outError = nullptr);

} // namespace PS1PLY

#endif
