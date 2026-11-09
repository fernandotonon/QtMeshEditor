#pragma once

// Seamless loops from Kimodo motion (pure data: no ONNX, no Ogre).
//
// Kimodo generates one open clip: it starts from a still pose (every training clip does) and ends wherever the
// motion stops. Looping it takes two steps (KimodoNative::predict with Options::loop):
//   1. find a cycle in a first generation: the two frames about one cycle apart whose WINDOWS of poses match
//      best (a window, not one frame, so the speed matches too). When they match well the cycle is cut out and
//      its last frames are cross-faded into the frames before its start (cutLoop) — no second generation.
//   2. otherwise generate again with the cycle's start window pinned at BOTH ends of the clip, the end shifted
//      by one stride (KimodoNative), and close it (closeLoop). A motion with no cycle at all (an attack, a jump)
//      pins its own first window at both ends instead, so it returns to where it began.
// Every looped clip has period + 1 frames and its last frame equals its first plus the stride, so an engine
// playing it on repeat (clip length = period / fps) neither skips nor repeats a frame at the wrap.

#include <QString>
#include <QStringList>

#include <array>
#include <limits>
#include <vector>

namespace AIMotion::KimodoLoop {

enum class Mode {
    Auto,         ///< cut when the best cycle matches within kCutThresholdDeg, else regenerate
    Cut,          ///< always cut (one generation)
    Regenerate,   ///< always regenerate with the start/end window pinned
};
QStringList modeIds();                               ///< "auto", "cut", "regenerate"
bool modeFromId(const QString& id, Mode* mode);      ///< empty id = Auto
QString modeId(Mode mode);

/// Mean joint-angle mismatch (degrees) under which the cut alone loops cleanly.
constexpr double kCutThresholdDeg = 3.0;
/// Above this no cycle is assumed (a one-shot motion): the clip returns to its own start pose instead.
constexpr double kNoCycleThresholdDeg = 12.0;

/// A decoded motion: J local rotation matrices per frame (row-major 3x3), the root position, C contact channels.
struct Motion {
    int T = 0;
    int J = 0;
    int C = 0;
    std::vector<float> local;      ///< [T*J*9]
    std::vector<float> root;       ///< [T*3]
    std::vector<float> contacts;   ///< [T*C]
};

struct SearchOptions {
    int window = 5;          ///< frames compared per candidate pair (the speed matches too)
    int minStart = 15;       ///< skip the lead-in (Kimodo starts from rest)
    int minPeriod = 12;
    int maxPeriod = 0;       ///< 0 = as long as the motion allows
    int targetPeriod = 0;    ///< preferred length (the requested duration); 0 = no preference
    double lengthWeightDeg = 4.0;   ///< cost added per 100 % of length difference from targetPeriod
    /// A cycle must move: its largest pose change away from its first frame has to reach this fraction of the
    /// whole motion's (after the lead-in). Stops a still stretch (the hold after an attack) passing as a loop.
    double minActivity = 0.35;
};

struct Cycle {
    bool found = false;
    int start = 0;
    int period = 0;
    double costDeg = std::numeric_limits<double>::infinity();   ///< mean joint-angle mismatch of the windows
};

/// Mean angle (degrees) between the joint rotations of frames [i, i+window) and [j, j+window), plus the root
/// height difference (1 cm = 1 degree). Rotations are local, the root joint's includes the heading, so a cycle
/// that turns the character is not a loop.
double windowCostDeg(const Motion& m, int i, int j, int window);

/// Joint-angle difference (degrees) between two single frames, with the root height (1 cm = 1 degree).
double frameCostDeg(const Motion& m, int a, int b);

/// The best (start, period) pair, scored by windowCostDeg plus the length preference; candidates that barely
/// move (SearchOptions::minActivity) are skipped.
Cycle findCycle(const Motion& m, const SearchOptions& o);

/// Frames [start, start+period] of `m`: the last `blend` frames before the wrap are cross-faded into the frames
/// before `start` (each pose slerped, the root shifted by the stride), and the final frame is the first plus the
/// stride. The root is re-based so the first frame stands at x = z = 0. Needs start >= blend.
Motion cutLoop(const Motion& m, int start, int period, int blend);

/// Frames [0, period] of a clip whose last window was pinned to its first plus `stride` (x, z): the final frame
/// becomes exactly the first plus the stride, and the root is re-based to x = z = 0.
Motion closeLoop(const Motion& m, int period, const std::array<float, 2>& stride);

/// Horizontal root travel between two frames.
std::array<float, 2> strideOf(const Motion& m, int from, int to);

} // namespace AIMotion::KimodoLoop
