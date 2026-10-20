#pragma once

#include <array>
#include <cstdint>
#include <vector>

/// #1100: MHR joint rotations -> the 22 canonical CMU roles the animation retargeter consumes.
///
/// SAM 3D Body regresses a full MHR pose per frame, finger joints included, with no landmark detour: the joint's own
/// global rotation is the quantity the retargeter wants. The MHR frame is already the retargeter's canonical frame
/// (+Y up, +Z forward, the character's LEFT at +X: checked on the neutral skeleton, l_upleg x = +8 cm, l_ball z > l_foot z),
/// so no basis change is applied. Only WORLD DELTAS matter downstream (`applyMotionClip(worldFrame=true)` transports
/// q_frame * q_calibration^-1 onto the rig), and a rigid body's world delta does not depend on its local axes, so the
/// MHR joints' differing local frames need no per-role correction.
namespace SamBody {

constexpr int kRoleCount = 22;                        ///< PoseIK::kCanonicalRoles

/// Names of the 127 MHR joints, in the order of the model's joint arrays.
const std::array<const char*, 127>& mhrJointNames();
int mhrJointIndex(const char* name);                  ///< -1 when unknown
/// Parent of each MHR joint (-1 for the root); every parent precedes its children.
const std::array<int, 127>& mhrJointParents();

struct RoleFrame {
    std::array<std::array<float, 4>, kRoleCount> quats;   ///< (x,y,z,w), identity where unresolved
    uint32_t resolvedMask = 0;                            ///< bit i = role i taken from an MHR joint
};

/// `jointQuat`: 127 * 4 global joint rotations (x,y,z,w) as Body::jointQuat. Returns identity roles on malformed input.
RoleFrame canonicalRoles(const std::vector<float>& jointQuat);

/// Role order (PoseIK::Role): Hip, Abdomen, Chest, Neck, Neck1, Head, RCollar, RShoulder, RElbow, RHand, LCollar,
/// LShoulder, LElbow, LHand, RButtock, RHip, RKnee, RFoot, LButtock, LHip, LKnee, LFoot.
const std::array<const char*, kRoleCount>& roleSourceJoints();   ///< MHR joint per role, nullptr = left identity

} // namespace SamBody
