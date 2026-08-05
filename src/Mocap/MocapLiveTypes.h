#ifndef MOCAPLIVETYPES_H
#define MOCAPLIVETYPES_H

#ifdef ENABLE_MOCAP

#include "PoseIKSolver.h"

#include <array>
#include <cstdint>

// One body frame marshalled from the inference worker to the main thread.
struct BodyLiveFrame {
    bool valid = false;
    double timeSec = 0.0;
    std::array<std::array<float, 4>, PoseIK::kCanonicalRoles> quats;
    uint32_t resolvedMask = 0;
    std::array<float, PoseIK::kLandmarkCount * 3> world{};
    std::array<float, PoseIK::kLandmarkCount> visibility{};
};

#endif  // ENABLE_MOCAP
#endif  // MOCAPLIVETYPES_H
