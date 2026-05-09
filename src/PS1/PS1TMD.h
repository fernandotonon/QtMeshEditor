/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef PS1TMD_H
#define PS1TMD_H

#include <OgreEntity.h>
#include <OgreMesh.h>
#include <QString>

/**
 * Sony PlayStation TMD (Timed / 3D Model Data) import and export.
 *
 * Layout follows the Net Yaroze / libgs documentation: 12-byte file header,
 * 28-byte object headers, 8-byte vertices/normals (int16 x,y,z + int16 pad),
 * primitive packets (olen, ilen, flag, mode + ilen*4 payload).
 *
 * Supported modes include lit polygons (flag 0) and “no light” textured
 * triangles (mode 0x25 / 0x35, flag 1 per Net Yaroze). Texture UVs refer to
 * PSX VRAM layout (cba/tsb select CLUT and texture page); bitmaps live in .tim files.
 * UV import maps 8-bit page texels with a texel-center bias (no V flip; PSX and Ogre both treat
 * increasing V as downward in image space). Full VRAM page offsets are not baked in — match your texture
 * to the page.
 *
 * Coordinates are converted using a fixed-point scale (default 1/4096 world units
 * per TMD integer step). On import, an extra editor transform is applied: uniform
 * scale by kTmdEditorUniformScale and a 180° rotation about Z (x,y → −x, −y); normals use the same
 * rotation. Triangle vertex order is swapped (v1 ↔ v2) on import so CCW front faces match Ogre;
 * export swaps back for on-disk PSX order. See issue #357.
 */
namespace PS1TMD {

/// Default: Ogre world units per one TMD fixed-point step (PSX-style 12.4).
constexpr float kDefaultOgreUnitsPerTmdStep = 1.0f / 4096.0f;

/// Extra scale applied when importing (removed on export) to better match typical Ogre world units.
constexpr float kTmdEditorUniformScale = 10.0f;

/** Import a .tmd file into a new manual Ogre::Mesh in group "General". */
Ogre::MeshPtr importTmd(const QString& filePath, const std::string& meshName,
                          float ogreUnitsPerTmdStep = kDefaultOgreUnitsPerTmdStep);

/** Export one entity (all submeshes → one TMD object each). Static mesh only. */
bool exportEntity(const Ogre::Entity* entity, const QString& filePath,
                  float ogreUnitsPerTmdStep = kDefaultOgreUnitsPerTmdStep);

} // namespace PS1TMD

#endif
