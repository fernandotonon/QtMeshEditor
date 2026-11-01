/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef ALEMBICEXPORTER_H
#define ALEMBICEXPORTER_H

#include <QList>
#include <QStringList>
#include <QString>

#include <array>
#include <cstdint>
#include <vector>

namespace Ogre { class Entity; }

/**
 * @brief Alembic (.abc) exporter — epic #517 Slice J (#527).
 *
 * The write side of AlembicImporter. Bakes whatever animates an entity —
 * skeletal skinning, VAT_POSE vertex/morph clips, and SceneManager node
 * transform clips — into one Ogawa `.abc` cache: an `OXform` per entity
 * (world matrix per frame) with an `OPolyMesh` child (deformed positions per
 * frame, static topology + UV0). Everything shares one uniform time sampling
 * (1/fps), so the file reads back through `AlembicImporter::readFrameSet`
 * with the same fps and frame count.
 *
 * Two layers, mirroring the importer:
 *  - `write()` — pure data in, file out. No Ogre, no GL, unit-testable.
 *  - `exportEntities()` — the Ogre adapter that samples the live scene into a
 *    `Scene` and calls `write()`.
 *
 * All Alembic/Imath usage lives in AlembicExporter.cpp behind
 * `#ifdef ENABLE_ALEMBIC`; without it `available()` is false and both entry
 * points fail with a "rebuild with -DENABLE_ALEMBIC" message.
 *
 * Conventions: Ogre space is written as-is (Y-up, right-handed, no unit or
 * axis conversion) and triangles keep their Ogre winding — the same
 * interpretation `AlembicImporter` applies on read, so a round trip is
 * lossless. Normals are not written (consumers recompute them).
 */
namespace AlembicExporter {

/// True only when built with ENABLE_ALEMBIC.
bool available();

/// One exported mesh object. `framePositions` holds the deformed positions
/// (3 floats per vertex) per frame; a single entry means a static mesh.
struct Shape {
    QString xformName;                       ///< Parent OXform name.
    QString meshName;                        ///< OPolyMesh name.
    std::vector<int32_t> faceCounts;         ///< Vertices per face.
    std::vector<int32_t> faceIndices;        ///< Concatenated face corner indices.
    std::vector<float>   uvs;                ///< UV0, 2 floats per vertex (optional).
    std::vector<std::vector<float>> framePositions;
    /// World matrix per frame in Imath layout (row-major, row-vector
    /// convention: translation in elements 12..14). One entry = static. Empty
    /// = identity.
    std::vector<std::array<double, 16>> frameXforms;
};

struct Scene {
    double fps = 30.0;
    std::vector<Shape> shapes;
};

struct WriteResult {
    bool ok = false;
    QString error;
    int frameCount = 0;   ///< Time samples written (max across shapes).
    int shapeCount = 0;
};

/// Write `scene` to `path` (Ogawa). Validates topology and per-frame sizes
/// first and writes through a temp file so a failure never leaves a truncated
/// `.abc` behind.
WriteResult write(const QString& path, const Scene& scene);

struct Options {
    double  fps = 30.0;
    double  startTime = -1.0;   ///< < 0 → 0.
    double  endTime = -1.0;     ///< < 0 → the longest driven clip.
    QString animationName;      ///< Entity clip to bake; empty → every enabled clip.
    bool    nodeAnimations = true;  ///< Also bake SceneManager node transform clips.
};

struct Result {
    bool ok = false;
    QString error;
    int frameCount = 0;
    int shapeCount = 0;
    int vertexCount = 0;     ///< Across all shapes.
    double durationSec = 0.0;
    bool animated = false;   ///< False → a single static frame was written.
    QStringList animationsUsed;  ///< Clips that drove the bake.
};

/// Sample `entities` (positions, UV0, parent node world transform) over the
/// requested range and write the cache. The scene's animation state and node
/// transforms are restored afterwards. Needs a live Ogre scene.
Result exportEntities(const QList<Ogre::Entity*>& entities, const QString& path,
                      const Options& opts = Options());

}  // namespace AlembicExporter

#endif  // ALEMBICEXPORTER_H
