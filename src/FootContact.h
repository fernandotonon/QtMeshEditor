#ifndef FOOT_CONTACT_H
#define FOOT_CONTACT_H

#include <array>
#include <vector>

// #856 — foot-contact cleanup, pure-data core (Ogre-free, unit-tested).
//
// Retargeted clips slide/float feet on rigs whose proportions differ from the
// source: the bind-referenced direction retarget (PR #843) transfers bone
// DIRECTIONS, not world foot positions. This core provides the two pieces the
// track post-process (AnimationMerger::pinFeet) composes:
//   1. contact detection — frames where a foot is near the clip's ground
//      level AND nearly stationary horizontally → contact spans,
//   2. an analytic two-bone IK step — where must the knee sit so the foot
//      reaches its pinned position, preserving the pose's own bend plane,
// plus the smooth edge-blend weight that avoids knee pops at span borders.
namespace FootContact {

using V3 = std::array<float, 3>;

/// Inclusive frame range during which a foot is planted.
struct Span {
    int start = 0;
    int end = 0;
};

struct DetectOptions {
    /// Height band above the detected ground level counting as "on the
    /// ground", as a fraction of leg length. Deliberately TIGHT: a walking
    /// foot lifts only ~5-15% of leg length, and a generous band swallows
    /// most of the swing phase — the whole clip pins and the character
    /// shuffles (legs frozen while the upper body keeps full rate).
    float heightBandFrac = 0.06f;
    /// Max horizontal speed (units/frame) counting as "stationary", as a
    /// fraction of leg length.
    float velThreshFrac = 0.012f;
    /// Spans shorter than this are noise, not contacts.
    int minFrames = 4;
    /// Up axis: 0=X, 1=Y, 2=Z (canonical rigs are +Y-up).
    int upAxis = 1;
};

/// Ground level = low percentile of the foot's height trajectory (robust to
/// a clip that never plants, e.g. a jump apex-only window).
float groundLevel(const std::vector<V3>& foot, int upAxis);

/// Contact spans of one foot trajectory. `legLength` scales both thresholds
/// so detection is rig-size-independent.
std::vector<Span> detectContacts(const std::vector<V3>& foot,
                                 float legLength,
                                 const DetectOptions& opt = {});

/// Analytic two-bone IK: given the CURRENT chain (hip → knee → foot) and a
/// new foot `target`, return the knee position that keeps both segment
/// lengths and stays in the pose's own bend plane (pole derived from the
/// current knee). Targets beyond reach are clamped along hip→target;
/// degenerate poses (straight leg) bend toward the current knee offset or,
/// failing that, any perpendicular.
V3 solveKnee(const V3& hip, const V3& knee, const V3& foot, const V3& target);

/// Smooth 0→1→0 weight for frame `f` inside span `s`, ramping linearly over
/// `blend` frames at each edge (1 in the interior, 0 outside).
float blendWeight(const Span& s, int f, int blend);

}  // namespace FootContact

#endif  // FOOT_CONTACT_H
