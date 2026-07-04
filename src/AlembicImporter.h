/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef ALEMBICIMPORTER_H
#define ALEMBICIMPORTER_H

#include <QString>

#include "VertexAnimationManager.h"

namespace Ogre { class SceneNode; }

/**
 * @brief Alembic (.abc) vertex-animation reader — Anim Slice B, sub-slice B2.
 *
 * Decodes a baked per-vertex cache (the first animated IPolyMesh in the archive)
 * into a source-agnostic VertexAnimationManager::FrameSet, then B1's
 * buildClipFromFrames turns it into an Ogre VAT_POSE clip. Cloth / sims / fluid
 * bakes / Houdini + Blender exports.
 *
 * All Alembic/Imath usage lives in AlembicImporter.cpp behind
 * `#ifdef ENABLE_ALEMBIC`. When the build lacks Alembic, `available()` is false
 * and the read/import calls fail with a clear "rebuild with -DENABLE_ALEMBIC"
 * message — nothing crashes, and the rest of the app is unaffected.
 *
 * The decode step (readFrameSet) is pure data — no Ogre, no GL — so it's
 * unit-testable against a small synthetic .abc fixture under headless CI.
 */
namespace AlembicImporter {

/// True only when built with ENABLE_ALEMBIC.
bool available();

struct ReadResult {
    bool ok = false;
    QString error;
    VertexAnimationManager::FrameSet frames;  ///< decoded cache (empty on !ok)
    QString meshName;                          ///< source IPolyMesh name (for the clip)
};

/// Decode `path`'s first animated polymesh into a FrameSet. Pure data. The
/// topology (index buffer) is taken from the first sample; positions are read
/// per time-sample. A mesh whose topology changes between frames (variable
/// vertex count) is rejected — VAT_POSE needs a fixed base. `maxFrames` caps
/// how many samples are decoded (0 = all); the streaming path (B3) reads on
/// demand instead.
ReadResult readFrameSet(const QString& path, int maxFrames = 0);

/// Import `path` into the scene: build a base Ogre::Mesh from the first sample's
/// topology, attach a VAT_POSE clip from the decoded frames, create an entity,
/// and return its SceneNode (nullptr + `error` on failure). Requires an active
/// Ogre scene (GL) — this is the GUI/CLI-viewport entry point.
Ogre::SceneNode* importToScene(const QString& path, QString* error = nullptr);

}  // namespace AlembicImporter

#endif  // ALEMBICIMPORTER_H
