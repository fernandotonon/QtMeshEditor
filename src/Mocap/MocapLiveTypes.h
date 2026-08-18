#ifndef MOCAPLIVETYPES_H
#define MOCAPLIVETYPES_H

#ifdef ENABLE_MOCAP

#include "PoseIKSolver.h"

#include <array>
#include <cstdint>

struct HandLandmarks {
    bool valid = false;
    float presence = 0.f;
    std::array<float, 21 * 3> cropXyz{};  // x,y in [0,1] crop, z relative
    std::array<float, 21 * 3> worldXyz{}; // Identity_3 metric, wrist-relative
    std::array<float, 21 * 2> imageXy{};  // full-frame pixels
};

struct HandsLiveFrame {
    double timeSec = 0.0;
    HandLandmarks left;
    HandLandmarks right;
};

// One body frame marshalled from the inference worker to the main thread.
struct BodyLiveFrame {
    bool valid = false;
    double timeSec = 0.0;
    std::array<std::array<float, 4>, PoseIK::kCanonicalRoles> quats;
    uint32_t resolvedMask = 0;
    std::array<float, PoseIK::kLandmarkCount * 3> world{};
    std::array<float, PoseIK::kLandmarkCount * 3> screenCrop{};
    std::array<float, PoseIK::kLandmarkCount * 2> imageXy{};
    std::array<float, PoseIK::kLandmarkCount> visibility{};
    HandsLiveFrame hands;
};

#endif  // ENABLE_MOCAP
#endif  // MOCAPLIVETYPES_H
