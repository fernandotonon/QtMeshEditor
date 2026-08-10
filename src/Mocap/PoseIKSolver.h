#ifndef POSEIKSOLVER_H
#define POSEIKSOLVER_H

// Analytic body-pose solver (epic #869, Slice E #874) — the always-available
// permissive fallback backend. Pure data: no Qt/Ogre/ONNX.
//
// Input: MediaPipe pose WORLD landmarks per frame (33 x xyz, metres,
// hip-centred, MediaPipe's frame: +x subject's-left, +y DOWN, +z toward the
// camera). Canonicalized to CMU (+Y up, +Z forward, LEFT at +X) via (x,-y,+z).
// Output: WORLD orientation quaternions (x,y,z,w) for the 22 canonical CMU
// roles MotionInbetween/AnimationMerger retarget with
// (`applyMotionClip(..., worldFrame=true)` takes the delta vs frame 0 and
// transports it onto the rig, so only CONSISTENCY over time matters, not the
// absolute basis).
//
// Per role the solver builds an orthonormal frame from landmark geometry:
//   - hips / spine / chest: hip line (or shoulder line) + the hips->shoulders
//     spine direction — a full 3-axis torso basis, so torso roll is captured;
//     abdomen blends the two.
//   - head: ear line + nose direction.
//   - limbs (upper/lower arm+leg): the segment direction is the primary axis;
//     the secondary (twist reference) axis is PARALLEL-TRANSPORTED from the
//     previous frame (projected perpendicular to the new segment), which
//     zeroes relative twist by construction and stays continuous even when
//     the segment sweeps past a torso axis. The first frame seeds from the
//     most orthogonal torso axis. This is the "zero the twist" hinge
//     treatment the slice spec asks for — no candy-wrapping.
//   - collar/buttock/neck twist roles stay identity (their delta is identity,
//     so the rig keeps its standing pose there).
//
// The solver is stateful (the transported axes): one Solver per take,
// frames fed in order; reset() between takes.

#include <array>
#include <cstdint>

namespace PoseIK {

constexpr int kLandmarkCount = 33;
constexpr int kCanonicalRoles = 22;

// canonical role indices (MotionInbetween's kCanonJoints order)
enum Role : int {
    Hip = 0, Abdomen = 1, Chest = 2, Neck = 3, Neck1 = 4, Head = 5,
    RCollar = 6, RShoulder = 7, RElbow = 8, RHand = 9,
    LCollar = 10, LShoulder = 11, LElbow = 12, LHand = 13,
    RButtock = 14, RHip = 15, RKnee = 16, RFoot = 17,
    LButtock = 18, LHip = 19, LKnee = 20, LFoot = 21,
};

struct FrameResult {
    // world orientation per canonical role, (x,y,z,w); identity if unresolved
    std::array<std::array<float, 4>, kCanonicalRoles> quats;
    uint32_t resolvedMask = 0;  // bit i set = role i solved this frame

    bool resolved(int role) const { return resolvedMask & (1u << role); }
};

class Solver {
public:
    // world: 33 x 3 floats (MediaPipe world landmarks). visibility: optional
    // 33 sigmoided visibilities; landmarks below minVisibility invalidate the
    // roles they feed (those roles hold their previous orientation if any,
    // else identity, and are not marked resolved).
    FrameResult solveFrame(const float* world, const float* visibility = nullptr,
                           float minVisibility = 0.3f);

    void reset();

    // Canonicalize MediaPipe world landmarks (+x subject-left, +y down, +z
    // toward camera) into the CMU frame (+Y up, +Z forward, LEFT at +X).
    static void canonicalizeMediaPipeWorld(
        const float* world33x3,
        std::array<std::array<float, 3>, kLandmarkCount>& out);

    // Unit segment direction for a limb role (shoulder→elbow, etc.), or false
    // when landmarks are missing / below minVisibility.
    static bool limbSegmentDirection(
        int role, const std::array<std::array<float, 3>, kLandmarkCount>& p,
        const float* visibility, float minVisibility,
        std::array<float, 3>& outDir);

    // Live canonical bone-axis direction for any role (limbs + torso/head).
    // Used by BodyRetargeter live drive to aim bind bones at landmark geometry
    // — the same directions PoseIK derives internally, without the quat/Mc path.
    static bool canonicalLiveDirection(
        int role, const std::array<std::array<float, 3>, kLandmarkCount>& p,
        const float* visibility, float minVisibility,
        std::array<float, 3>& outDir);

private:
    bool m_hasPrev = false;
    // previous secondary (twist-reference) + primary axes per role, world frame
    std::array<std::array<float, 3>, kCanonicalRoles> m_prevSecondary{};
    std::array<std::array<float, 3>, kCanonicalRoles> m_prevPrimary{};
    std::array<std::array<float, 4>, kCanonicalRoles> m_prevQuats{};
    // previous hip horizontal line (world frame) — keeps the torso frame
    // from flipping 180° on a noisy/occluded seated subject.
    std::array<float, 3> m_prevHipLine{0.f, 0.f, 0.f};
};

}  // namespace PoseIK

#endif  // POSEIKSOLVER_H
