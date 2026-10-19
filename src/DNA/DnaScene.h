#ifndef DNA_DNASCENE_H
#define DNA_DNASCENE_H

// MetaHuman DNA -> Ogre scene (epic #1095, Phase 1). Ogre side of the importer;
// the pure-data reader is DnaReader.h.

#include "DnaReader.h"

#include <QString>
#include <QStringList>

#include <memory>

namespace Ogre { class SceneNode; }

namespace Dna {

class RigEvaluator;

struct ImportReport {
    int joints = 0;
    int meshes = 0;
    int vertices = 0;
    int triangles = 0;
    int morphTargets = 0;
    int truncatedInfluenceVertices = 0;  ///< verts with > 4 influences clipped to the 4 strongest
    int maxInfluencesSeen = 0;
    int skippedFaces = 0;
    int correctiveJoints = 0;       ///< rig-driven joints present in the imported skeleton
    int correctiveJointsFolded = 0; ///< rig-driven joints lost to the 256-bone limit
    bool correctivesAttached = false;
    int faceShapes = 0;             ///< morph targets baked from the face rig
    int faceShapesApproximate = 0;  ///< of those, ARKit channels that are close composites
    double faceBakeMs = 0.0;
    double faceMaxDeltaMm = 0.0;    ///< largest single-vertex displacement over all baked shapes
    QStringList warnings;
};

/// Build skeleton + mesh (one submesh per DNA mesh at the chosen LOD) + morph
/// targets from `rig` and create an entity under a new scene node. Returns the
/// node, or nullptr with `*error` set. Requires an active Ogre scene. When an
/// evaluator with driver joints is given, the live body correctives are attached
/// to the new entity (DnaCorrectiveManager).
Ogre::SceneNode* importRigToScene(const Rig& rig, const QString& baseName,
                                  ImportReport* report = nullptr, QString* error = nullptr,
                                  std::shared_ptr<RigEvaluator> evaluator = nullptr);

/// What the importer builds on top of the neutral character.
struct ImportOptions {
    /// A face DNA (it exposes expression controls) gets its rig baked into morph
    /// targets: the 51 ARKit channels by default. See DnaFaceBake.h for why a
    /// bake rather than live joints.
    bool faceMorphs = true;
    /// Bake every "CTRL_expressions.*" control as its own target instead of the
    /// ARKit set (large: ~260 targets).
    bool allFaceControls = false;
    /// JSON override of the ARKit mapping (qtmesh-dna-arkit-map-v1).
    QString arkitMapPath;
};

/// Read `path` and import it, with the face bake controlled by `import`.
Ogre::SceneNode* importToScene(const QString& path, const ReadOptions& options, const ImportOptions& import,
                               ImportReport* report = nullptr, QString* error = nullptr);

/// Read `path` and import it. Entry point used by MeshImporterExporter.
Ogre::SceneNode* importToScene(const QString& path, const ReadOptions& options = ReadOptions(),
                               ImportReport* report = nullptr, QString* error = nullptr);

}  // namespace Dna

#endif
