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
#include <QSet>
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
    // Its triangles may be REORDERED (vertex cache / overdraw). False for
    // alpha-blended materials, where draw order changes the blended result.
    bool orderFixable = false;
    // Skinned or morphed: a zero-area base triangle may open up when
    // deformed, so the degenerate fix only drops repeated-corner triangles.
    bool deformable = false;
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

// Apply `opt` to every primitive that allows it and write the file back.
// External .bin buffers are rewritten too: every file is staged first, the
// buffers are swapped in before the main document, and a failure rolls the
// already-replaced files back. With `dryRun` the report is computed and
// nothing is written.
//
// `writeRoot` bounds where external buffers may be written (canonical
// paths, so `..` and symlinks cannot leave it); empty = the asset's own
// directory. A buffer outside it refuses the whole fix.
//
// `sharedBuffers` holds canonical paths of external buffers that another
// glTF document also reads (see sharedExternalBuffers). Rewriting one would
// move bytes under the other document's accessors, so a document using any
// of them is refused as a whole and the shared file is left untouched.
FixReport fixFile(const QString& path, const MeshQuality::FixOptions& opt, bool dryRun = false,
                  const MeshQuality::Limits& limits = {}, const QString& writeRoot = QString(),
                  const QSet<QString>& sharedBuffers = {});

// Canonical paths of the external (file) buffers a glTF/GLB references;
// data URIs and the GLB BIN chunk are not included. Empty when the file
// cannot be parsed.
QStringList externalBufferPaths(const QString& path);

// External buffers referenced by two or more of `documents` (canonical
// paths). Non-glTF paths in the list are ignored.
QSet<QString> sharedExternalBuffers(const QStringList& documents);

} // namespace GltfMeshQuality
