#pragma once

// NVIDIA SOMA skeleton naming -> the standard (Mixamo-style) spellings the
// project's role matcher (Retarget::autoMap) understands.
//
// SOMA's names LOOK like Mixamo's but several mean different bones, so they
// must never go through the matcher as-is (measured on the somaskel77
// standard T-pose):
//   * `LeftLeg` is the THIGH root (hip height, 0.92 m); Mixamo's `LeftLeg` is
//     the shin. SOMA's shin is `LeftShin`.
//   * `LeftHandIndex1` sits 3 cm past the wrist: the metacarpal. The knuckle
//     is `Index2`, so SOMA Index2..4 are Mixamo Index1..3 (same for Middle,
//     Ring, Pinky). The thumb has no metacarpal joint: Thumb1..3 map 1:1.
//   * The spine has three segments under `Chest`: Spine1/Spine2/Chest are
//     Mixamo Spine/Spine1/Spine2; Neck1/Neck2 are Neck/Neck1.
//
// The table is versioned (kSomaMapVersion). Change a row -> bump the version,
// since clips made with the old mapping are not reproducible with the new one.
// Unmapped SOMA joints are REPORTED (somaJointRole), never dropped silently.

#include <string>
#include <vector>

namespace AIMotion {

constexpr const char* kSomaMapVersion = "somaskel77-standard-v1";

enum class SomaRole {
    Mapped,        ///< has a standard counterpart (somaStandardName)
    EndEffector,   ///< *End / HeadEnd: a leaf marker, no rotation of its own
    Facial,        ///< Jaw, eyes: not carried by body rigs
    Metacarpal,    ///< Hand<Finger>1 of the four long fingers
    Unknown,       ///< not in the table (a newer SOMA revision?)
};

/// Standard spelling of a SOMA joint, or "" when it has none.
std::string somaStandardName(const std::string& somaName);

SomaRole somaJointRole(const std::string& somaName);
const char* somaRoleId(SomaRole r);   ///< mapped|end|facial|metacarpal|unknown

/// True when `names` is a SOMA skeleton this table was written for (has the
/// SOMA-specific Hips/Spine1/Chest/LeftShin/RightShin set).
bool looksLikeSoma(const std::vector<std::string>& names);

} // namespace AIMotion
