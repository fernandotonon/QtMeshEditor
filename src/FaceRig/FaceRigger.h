#ifndef FACERIGGER_H
#define FACERIGGER_H

// FaceRigger — the face auto-rig orchestrator (#889, Slice E #893). Chains the
// pure-data stages into per-USER-vertex ARKit blendshape deltas:
//
//   ArkitTemplate  (neutral + 52 expression deltas, template topology)
//        │
//        ▼  NonRigidICP  (#891): fit template neutral → user neutral
//   correspondence X   (template-topology verts lying on the user surface)
//        │
//        ▼  DeformationTransfer (#892): per shape, transfer the template's
//        │   per-triangle deformation onto X → per-TEMPLATE-vertex delta
//        │
//        ▼  resample template-topology deltas → the real USER vertices
//   52 × per-user-vertex deltas, named per FaceCap::kBlendshapeNames
//
// Ogre-free + headless-tested; the Ogre attach (create Pose + VAT_POSE per
// shape on the user entity) + CLI/MCP surfaces live in the FaceRig CLI/MCP
// layer, reusing MorphCommands' buildPosesFromSlices pattern.
//
// Humanoid-only (risk #1 in docs/FACE_RIG_SPIKE.md): the fit residual is a
// quality gate — a non-face mesh fits poorly and is rejected with a clear
// reason, mirroring the AutoRig/Pinocchio precedent.

#include "NonRigidICP.h"   // NricpLandmark

#include <QString>

#include <functional>
#include <string>
#include <vector>

namespace FaceRig {

class ArkitTemplate;

struct FaceRigShape {
    QString name;                    // a FaceCap::kBlendshapeNames entry
    std::vector<float> userDeltas;   // userVertexCount*3, (expr - neutral)
    int nonZeroVerts = 0;            // verts this shape actually moves
    float maxDisp = 0.0f;            // max |delta| (mesh units)
};

struct FaceRigResult {
    std::vector<FaceRigShape> shapes;
    int userVertexCount = 0;
    double fitMeanResidualPct = 0.0;   // NRICP mean residual / user diag (%)
    double fitMaxResidualPct = 0.0;
    bool ok = false;
    std::string error;                 // set when !ok
};

struct FaceRigOptions {
    // Reject the rig when the NRICP fit is worse than this (% of user diag).
    // A human template only fits a roughly human face; a prop/creature blows
    // past this and is refused rather than emitting garbage shapes.
    double maxFitResidualPct = 8.0;
    // Drop per-vertex deltas below this fraction of the user diagonal (noise
    // floor from the transfer solve) so shapes stay sparse.
    double deltaEpsPct = 0.02;
    // Cap the shapes generated (0 = all in the template). Names are matched
    // against the template's own shape order.
    int maxShapes = 0;
};

// Progress callback: (done, total, phase). `phase` is a short static label
// ("Fitting…", "Transferring shapes…"). Return false to request cancellation —
// buildFaceRig then returns ok=false with error "cancelled". May be called from
// a worker thread, so the callee must marshal any UI work itself.
using FaceRigProgressFn =
    std::function<bool(int done, int total, const char* phase)>;

// userV/userF: the user neutral mesh (Nu*3 verts + Fu*3 tris). tmpl: a loaded
// ArkitTemplate. Returns per-user-vertex delta sets (size Nu*3 each), or
// ok=false + error on failure / a poor fit.
// `headMask` (optional, size Nu): when non-empty, ONLY vertices flagged 1 are
// fitted against the face template and receive blendshape deltas — the fix for
// full-body characters (the face template must not smear over the body). Empty
// = fit the whole mesh (a bare-face crop).
// `landmarks` (optional): facial-landmark correspondences pinning template
// vertices to USER positions (in the SAME frame as userV), computed externally
// by the Ogre landmark layer. When present they anchor the NRICP fit so the
// template lands on the real eyes/nose/mouth instead of a mis-oriented drape.
// `progress` (optional) reports fit + per-shape steps and can cancel.
FaceRigResult buildFaceRig(const std::vector<float>& userV,
                           const std::vector<int>& userF,
                           const ArkitTemplate& tmpl,
                           const FaceRigOptions& opts = {},
                           const std::vector<char>& headMask = {},
                           const std::vector<NricpLandmark>& landmarks = {},
                           const FaceRigProgressFn& progress = {});

}  // namespace FaceRig

#endif  // FACERIGGER_H
