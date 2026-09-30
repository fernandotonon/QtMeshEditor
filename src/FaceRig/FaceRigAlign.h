#ifndef FACERIGALIGN_H
#define FACERIGALIGN_H

// Face-rig orientation (#889 follow-up): the fit used to assume the user head
// faces the template's +Z "by contract" — the NRICP prealign is centroid +
// scale only, the anchor-quality gate and the left/right marker-swap
// heuristic scored a rotation-free similarity, and nothing ever estimated a
// rotation. Any head facing another way (a glTF whose face points -Z, a
// character turned to +X, a Z-up export) therefore had its auto anchors
// rejected as garbage, its marker pairs mirror-swapped, and the template
// draped over the BACK of the head. This is the rotation-aware piece:
//
//   - `alignPoints` / `alignTemplateToUser`: Horn's closed-form similarity
//     (proper rotation + uniform scale + translation) from anchor pairs.
//   - `rotationAwareResidual`: constellation residual AFTER the best
//     similarity, so a correctly paired constellation scores low under any
//     yaw/pitch/roll while a mirrored pairing (which no rotation explains)
//     still scores high.
//
// Pure data (reuses FaceCapPose::solve) — headless-tested.

#include "NonRigidICP.h"   // NricpLandmark

#include <array>
#include <vector>

namespace FaceRig {

struct SimilarityAlign {
    std::array<float, 9> R{1, 0, 0, 0, 1, 0, 0, 0, 1};  // row-major, src -> dst
    float scale = 1.0f;
    std::array<float, 3> translation{0, 0, 0};          // dst ~= scale*R*src + t
    float residual = 1e9f;   // mean |dst - fit| / mean spread of dst (dimensionless)
    float angleDeg = 0.0f;   // rotation angle of R
    int count = 0;
    bool ok = false;
};

// Similarity fit of `src[i]` -> `dst[i]` (>= 3 non-collinear pairs).
SimilarityAlign alignPoints(const std::vector<std::array<float, 3>>& src,
                            const std::vector<std::array<float, 3>>& dst);

// Same, pairing template vertex positions with the anchors' user targets.
SimilarityAlign alignTemplateToUser(const std::vector<float>& tmplV,
                                    const std::vector<NricpLandmark>& anchors);

// Constellation residual after the best similarity INCLUDING rotation
// (normalised by the mean spread of `dst`, like the old rotation-free
// residual). 1e9 when < 3 pairs or degenerate.
double rotationAwareResidual(const std::vector<std::array<float, 3>>& src,
                             const std::vector<std::array<float, 3>>& dst);

// R * v (or R^T * v when `transpose`).
std::array<float, 3> rotateVec(const std::array<float, 9>& R,
                               const std::array<float, 3>& v, bool transpose);

// Rotate every xyz triple of `xyz` about `pivot` by R (or R^T).
void rotateInPlace(std::vector<float>& xyz, const std::array<float, 9>& R,
                   const std::array<float, 3>& pivot, bool transpose);

// Smallest rotation taking `faceDir` (a face-forward direction) onto +Z —
// the template's forward. Handles yaw AND pitch (a Z-up head facing -Y
// maps with its up onto +Y); a direction alone cannot recover ROLL about
// the view axis, so that stays unsolved on the hint-only path. The
// antipodal case (facing -Z) turns 180 deg about +Y, keeping the head
// upright. Identity when `faceDir` is degenerate or already within
// `minAngleDeg` of +Z. Used when a bake has no anchors but the landmark
// ranking still told us which way the face points.
std::array<float, 9> rotationToPlusZ(const std::array<float, 3>& faceDir,
                                     float minAngleDeg, float* outAngleDeg = nullptr);

}  // namespace FaceRig

#endif  // FACERIGALIGN_H
