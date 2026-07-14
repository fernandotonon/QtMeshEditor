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
};

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

}  // namespace FaceRig

#endif  // FACERIGATTACH_H
