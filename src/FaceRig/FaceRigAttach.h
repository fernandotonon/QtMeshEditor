#ifndef FACERIGATTACH_H
#define FACERIGATTACH_H

// FaceRigAttach — the Ogre-touching bridge for the face auto-rig (#889,
// Slice E #893). Extracts a user entity's mesh geometry, runs the Ogre-free
// FaceRigger (NRICP → DeformationTransfer → resample), and attaches the
// resulting ARKit blendshapes as Ogre::Pose + VAT_POSE morph targets on the
// entity — reusing the exact pose-build mechanic MorphCommands uses so face
// capture (#869) drives them with no new playback code.
//
// Shared by the CLI (`qtmesh facerig`), MCP (`add_arkit_blendshapes`), and the
// GUI "Add ARKit Blendshapes" button.

#include "FaceRigger.h"

#include <QString>

#include <cstdint>
#include <vector>

namespace Ogre { class Entity; }

namespace FaceRig {

struct AttachReport {
    bool ok = false;
    QString error;
    int shapesAttached = 0;
    int userVertexCount = 0;
    double fitMeanResidualPct = 0.0;
    double fitMaxResidualPct = 0.0;
    QString templateFallback;   // set when the template had to be downloaded/etc.
    std::vector<QString> shapeNames;   // attached shape names, in order
};

// One geometry owner (shared vertex pool, or a per-submesh vertex data) with
// the morph pose target handle it maps to and its base offset into the combined
// vertex set. Ogre's 1-based convention: 0 = shared, 1..N = submesh index+1.
struct GeometryOwner {
    unsigned short handle;
    std::uint32_t base;
    int count;
};

// The user entity's geometry read out of Ogre once, so the heavy Ogre-free
// buildFaceRig() can run OFF the main thread (the GUI path) while extraction +
// attach stay on the main thread. Pure data — no Ogre handles retained.
struct FaceRigGeometry {
    std::vector<float> userV;   // combined positions (Nu*3)
    std::vector<int> userF;     // combined triangle indices
    std::vector<GeometryOwner> owners;
    bool valid() const { return userV.size() >= 9 && userF.size() >= 3; }
};

// MAIN-thread: read the entity's combined geometry (locks Ogre hardware
// buffers — milliseconds). Empty/invalid result on failure.
FaceRigGeometry extractGeometry(Ogre::Entity* entity);

// MAIN-thread: attach a computed FaceRigResult's shapes to `entity` as
// Ogre::Pose + VAT_POSE morph targets (via AddMorphTargetCommand), splitting
// the combined per-vertex deltas back onto each owner's handle. Fills the
// report's shapesAttached / ok. `geo.owners` must match the geometry the
// result was computed from.
void attachShapes(Ogre::Entity* entity, const FaceRigGeometry& geo,
                  const FaceRigResult& result, AttachReport& report);

// Runs the whole pipeline on `entity` using the given (already-loaded) template
// and attaches the shapes as poses + a per-target VAT_POSE clip. Re-initialises
// the live entity so the pose buffers exist (mirrors the morph/auto-rig path).
// On failure nothing is attached and report.error explains why.
AttachReport attachFaceRig(Ogre::Entity* entity,
                           const ArkitTemplate& tmpl,
                           const FaceRigOptions& opts = {});

// Convenience: ensure the bundled template is available (download-on-first-use),
// load it, then attachFaceRig. Returns a clear error if the template can't be
// obtained (offline + not present, or the build lacks the model).
AttachReport attachFaceRigWithBundledTemplate(Ogre::Entity* entity,
                                               const FaceRigOptions& opts = {});

// Write a `<meshPath>.arkit.json` sidecar with the ordered ARKit shape names,
// so downstream tools recover the names even though Assimp 6.0's glTF exporter
// drops mesh.extras.targetNames. Returns false on write failure.
bool writeArkitSidecar(const QString& meshPath,
                       const std::vector<QString>& shapeNames);

}  // namespace FaceRig

#endif  // FACERIGATTACH_H
