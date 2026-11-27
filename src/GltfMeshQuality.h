#pragma once
// Mesh-buffer quality of glTF / GLB files, read and fixed at the accessor
// level (issue #678).
//
// Why not through the editor's importer: MeshImporterExporter runs Assimp
// with JoinIdenticalVertices + ImproveCacheLocality, so the Ogre buffers the
// rest of the scan walks are already welded and cache-ordered — every
// ordering metric measured there describes OUR import, not the file. And a
// re-export through Assimp re-welds and reorders vertices on write. glTF is
// the format whose index/vertex buffers an engine uploads as-is, so this
// module measures the buffers exactly as stored and rewrites them in place,
// leaving materials, animations, skins, extensions and every other byte of
// the document untouched.
//
// Qt + meshoptimizer only (no Ogre), unit-testable on synthetic files.

#include "MeshQuality.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace GltfMeshQuality {

// .gltf / .glb (by extension).
bool handles(const QString& path);

struct PrimitiveReport {
    int mesh = 0;
    int primitive = 0;
    QString meshName;
    MeshQuality::Metrics metrics;
    bool measured = false;      // false = skipped (see note)
    bool indexFixable = false;  // its index accessor can be rewritten
    bool vertexFixable = false; // its vertex accessors can be rewritten (weld / fetch order)
    QString note;               // why it was skipped or only partly fixable
};

struct AnalyzeResult {
    bool ok = false;
    QString error;
    QList<PrimitiveReport> primitives;
};

AnalyzeResult analyzeFile(const QString& path, const MeshQuality::Limits& limits = {});

struct FixReport {
    bool ok = false;
    QString error;
    int primitivesChanged = 0;
    size_t degeneratesRemoved = 0;
    size_t verticesWelded = 0;
    size_t verticesDropped = 0;
    size_t trianglesBefore = 0;
    size_t trianglesAfter = 0;
    int primitivesSimplified = 0;
    qint64 bytesBefore = 0;
    qint64 bytesAfter = 0;
    QStringList notes;          // primitives left alone, and why
};

// Apply `opt` to every primitive that allows it and write the file back
// (atomically; external .bin buffers are rewritten too). With `dryRun` the
// report is computed and nothing is written.
FixReport fixFile(const QString& path, const MeshQuality::FixOptions& opt, bool dryRun = false,
                  const MeshQuality::Limits& limits = {});

} // namespace GltfMeshQuality
