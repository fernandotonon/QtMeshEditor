#ifndef FACERIGLANDMARKS_H
#define FACERIGLANDMARKS_H

// Landmark acquisition for the face auto-rig (#889): render a head mesh
// front-on, detect the MediaPipe 478 face landmarks (FaceLandmarkDetector), and
// back-project each 2D landmark through the render camera onto the mesh surface
// to get a 3D landmark ON THE MESH. Running this on BOTH the ARKit template and
// the user head, then pairing by MediaPipe index, yields the correspondences
// that anchor NRICP so the fit lands on the real face features.
//
// Ogre-touching (renders via MeshDepthRenderer::renderShadedView) — main thread.

#include "NonRigidICP.h"   // NricpLandmark

#include <array>
#include <vector>

namespace Ogre { class Entity; }

namespace FaceRig {

class ArkitTemplate;

struct MeshLandmarks {
    // 3D landmark positions in MESH-LOCAL space (same frame as the geometry the
    // fit uses), one per MediaPipe index; `valid[i]` is false when landmark i
    // didn't hit the surface (skipped as a correspondence).
    std::vector<std::array<float, 3>> points;
    std::vector<char> valid;
    float confidence = 0.0f;
    bool ok = false;
};

// Render `entity` front-on, detect face landmarks, back-project to the surface.
// `localV`/`localF` are the MESH-LOCAL vertices/indices the fit operates on
// (the head sub-mesh in local space) — the ray cast intersects these so the
// returned points live in the same frame as the fit. Returns ok=false when the
// detector is unavailable / no face is found.
MeshLandmarks detectMeshLandmarks(Ogre::Entity* entity,
                                  const std::vector<float>& localV,
                                  const std::vector<int>& localF);

// Build NRICP landmark anchors (template vertex → user position) by detecting
// face landmarks on BOTH the ARKit template and the user head and pairing them
// by MediaPipe index. `userEntity` is rendered for the user side; `userLocalV/F`
// are the head sub-mesh (local frame) the fit uses, both for the user raycast
// and the frame the returned targets live in. Returns empty when the detector
// is unavailable / either face isn't found (caller then fits without anchors).
std::vector<NricpLandmark> buildLandmarkAnchors(
    Ogre::Entity* userEntity,
    const std::vector<float>& userLocalV,
    const std::vector<int>& userLocalF,
    const ArkitTemplate& tmpl);

}  // namespace FaceRig

#endif  // FACERIGLANDMARKS_H
