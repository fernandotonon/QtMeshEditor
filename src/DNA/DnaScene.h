#ifndef DNA_DNASCENE_H
#define DNA_DNASCENE_H

// MetaHuman DNA -> Ogre scene (epic #1095, Phase 1). Ogre side of the importer;
// the pure-data reader is DnaReader.h.

#include "DnaReader.h"

#include <QString>
#include <QStringList>

namespace Ogre { class SceneNode; }

namespace Dna {

struct ImportReport {
    int joints = 0;
    int meshes = 0;
    int vertices = 0;
    int triangles = 0;
    int morphTargets = 0;
    int truncatedInfluenceVertices = 0;  ///< verts with > 4 influences clipped to the 4 strongest
    int maxInfluencesSeen = 0;
    int skippedFaces = 0;
    QStringList warnings;
};

/// Build skeleton + mesh (one submesh per DNA mesh at the chosen LOD) + morph
/// targets from `rig` and create an entity under a new scene node. Returns the
/// node, or nullptr with `*error` set. Requires an active Ogre scene.
Ogre::SceneNode* importRigToScene(const Rig& rig, const QString& baseName,
                                  ImportReport* report = nullptr, QString* error = nullptr);

/// Read `path` and import it. Entry point used by MeshImporterExporter.
Ogre::SceneNode* importToScene(const QString& path, const ReadOptions& options = ReadOptions(),
                               ImportReport* report = nullptr, QString* error = nullptr);

}  // namespace Dna

#endif
