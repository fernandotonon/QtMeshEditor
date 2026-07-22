#ifndef FACECAPPOSE_H
#define FACECAPPOSE_H

// Head-pose solve for face capture (epic #869, Slice C #872). Pure data —
// no Qt/Ogre/ONNX — headless-tested.
//
// Weighted rigid fit (Horn's closed-form quaternion method — the weighted
// Kabsch/Umeyama equivalent, solved with a 4x4 Jacobi eigensolver so no
// linear-algebra dependency is needed) of the MediaPipe canonical face model
// onto the detected 3D screen landmarks. Scale is solved and DISCARDED for
// the pose; translation is returned in the destination units.
//
// Convention (documented contract): the destination frame is +X right,
// +Y up, camera looking down -Z; identity rotation = the face looking
// straight at the camera. Callers build destination points from screen
// landmarks as (x_px, -y_px, -z_px) — image y grows downward and the
// landmark z grows away from the camera, so both flip. This intentionally
// replaces MediaPipe's perspective geometry pipeline (delta vs its facial
// transformation matrix measured at 4-9 degrees, systematic; the recorder
// neutral-calibrates the first confident frame anyway). See docs/MOCAP_SPIKE.md.

#include <array>

namespace FaceCapPose {

struct Result {
    std::array<float, 4> rotation{0.f, 0.f, 0.f, 1.f};  // (x,y,z,w), src -> dst
    std::array<float, 3> translation{0.f, 0.f, 0.f};    // dst units
    float scale = 1.f;                                   // solved, informational
    bool ok = false;
};

// General weighted rigid fit: dst ~= scale * R * src + t.
// src/dst are count x 3 (xyz interleaved); weights length count (>= 0, at
// least 3 non-zero non-collinear points required).
Result solve(const float* src, const float* dst, const float* weights, int count);

// Face-capture convenience: fits the embedded canonical face model
// (FaceCapCanonicalData.h, MediaPipe Procrustes basis weights) onto 478 (or
// 468) screen landmarks given as xyz triples in PIXELS (image coords, y down,
// z from the landmark model). Applies the (x, -y, -z) frame flip internally.
Result solveHeadPose(const float* landmarksXyz, int landmarkCount);

}  // namespace FaceCapPose

#endif  // FACECAPPOSE_H
