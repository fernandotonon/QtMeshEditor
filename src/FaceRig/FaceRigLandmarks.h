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

#include <QString>

#include <array>
#include <utility>
#include <vector>

namespace Ogre { class Entity; }

namespace FaceRig {

class ArkitTemplate;

// A user-adjustable face marker: a canonical facial point (eye/nose/mouth/…)
// with the TEMPLATE vertex it anchors and the current USER-mesh position (seeded
// from auto-detection, then draggable). label is a short guidance string.
struct FaceMarker {
    QString label;
    int mediapipeIndex = -1;         // canonical MediaPipe FaceMesh index
    int tmplVertex = -1;             // template vertex it pins (from tmpl detect)
    std::array<float, 3> userPos{0, 0, 0};  // user-mesh position (editable)
    bool placed = false;             // seeded-or-user-set (vs unresolved)
};

// The canonical marker set (label + MediaPipe index), fixed order. These are
// the anatomical anchors the fit needs to lock orientation/scale; the user only
// has to get these few roughly right, not all 478.
const std::vector<std::pair<QString, int>>& faceMarkerCatalog();

struct MeshLandmarks {
    // 3D landmark positions in MESH-LOCAL space (same frame as the geometry the
    // fit uses), one per MediaPipe index; `valid[i]` is false when landmark i
    // didn't hit the surface (skipped as a correspondence).
    std::vector<std::array<float, 3>> points;
    std::vector<char> valid;
    float confidence = 0.0f;
    // Direction the FACE points, in MESH-LOCAL space — derived from the view
    // whose render won the presence-logit ranking (the face points toward that
    // camera). Valid even when `ok` is false: a weak detection is still a
    // usable FACING signal, and the proportional-default marker placement
    // needs it so defaults land on the face instead of the back of the head
    // for meshes that don't face the template's +Z.
    std::array<float, 3> faceDirLocal{0, 0, 1};
    bool faceDirValid = false;
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

// Seed the editable face-marker set: detect on the template (reliable — a real
// human face) to resolve each marker's template vertex, then AUTO-DETECT on the
// user head to seed userPos when it works (marker.placed=true) or fall back to
// the template landmark position mapped into the user's head box when it
// doesn't (cartoon faces — the user then drags to correct). Always returns the
// full catalog with template verts resolved; `outConfident` reports whether the
// user auto-detect looked trustworthy.
std::vector<FaceMarker> seedFaceMarkers(
    Ogre::Entity* userEntity,
    const std::vector<float>& userLocalV,
    const std::vector<int>& userLocalF,
    const ArkitTemplate& tmpl,
    bool* outConfident = nullptr);

// Build NRICP anchors from the (possibly user-edited) markers — one per placed
// marker with a resolved template vertex. Auto-corrects a MIRRORED placement:
// if swapping the left/right pair targets matches the template constellation
// better (the user assumed the opposite left/right convention), the swapped
// pairing is used — so either convention works.
std::vector<NricpLandmark> anchorsFromMarkers(const std::vector<FaceMarker>& markers,
                                              const ArkitTemplate& tmpl);

}  // namespace FaceRig

#endif  // FACERIGLANDMARKS_H
