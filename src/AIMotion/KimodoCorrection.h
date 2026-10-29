#pragma once

// Kimodo's foot-skate / constraint post-processing (NVIDIA MotionCorrection, Apache-2.0, vendored in
// src/dependencies/kimodo_motion_correction) called natively: the same setup its pybind11 entry point
// (MotionCorrection/src/cpp/BindingsPython.cpp correct_motion) does, without Python.

#include <array>
#include <string>
#include <vector>

namespace AIMotion::KimodoCorrection {

struct Rig {
    std::vector<int> parents;                         ///< -1 for the root
    std::vector<std::array<float, 3>> translations;   ///< rest local translations (root lifted onto the floor)
    std::vector<std::array<float, 4>> rotationsXyzw;  ///< rest local rotations
    int leftHand = -1, rightHand = -1, leftFoot = -1, rightFoot = -1;
    bool hasDoubleAnkleJoints = false;
};

struct Masks {   ///< per frame, 0/1 (empty = all zeros)
    std::vector<float> fullBody, leftHand, rightHand, leftFoot, rightFoot, root;
};

/// In place: rootTranslations [T*3], jointRotationsWxyz [T*J*4]. Targets (constrained frames) the same layout;
/// contacts [T*4] = Kimodo's (L heel, L toe, R heel, R toe). False + error on a size mismatch.
bool correctMotion(std::vector<float>& rootTranslations, std::vector<float>& jointRotationsWxyz,
                   const std::vector<float>& rootTarget, const std::vector<float>& rotationsTargetWxyz,
                   const Masks& masks, const std::vector<float>& contacts, const Rig& rig,
                   float contactThreshold, float rootMargin, std::string* error = nullptr);

} // namespace AIMotion::KimodoCorrection
