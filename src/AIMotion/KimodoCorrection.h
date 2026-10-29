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
    int leftHand = -1;
    int rightHand = -1;
    int leftFoot = -1;
    int rightFoot = -1;
    bool hasDoubleAnkleJoints = false;
};

struct Masks {   ///< per frame, 0/1 (empty = all zeros)
    std::vector<float> fullBody;
    std::vector<float> leftHand;
    std::vector<float> rightHand;
    std::vector<float> leftFoot;
    std::vector<float> rightFoot;
    std::vector<float> root;
};

/// The motion corrected IN PLACE plus its constraint targets. Root translations [T*3], joint rotations (w,x,y,z)
/// [T*J*4]; targets use the same layout (identity/zero where unconstrained); contacts [T*4] are Kimodo's
/// (L heel, L toe, R heel, R toe).
struct MotionArrays {
    std::vector<float>* rootTranslations = nullptr;
    std::vector<float>* jointRotationsWxyz = nullptr;
    const std::vector<float>* rootTarget = nullptr;
    const std::vector<float>* rotationsTargetWxyz = nullptr;
    const std::vector<float>* contacts = nullptr;
};

struct Params {
    float contactThreshold = 0.5f;
    float rootMargin = 0.04f;
};

/// False + error on a size mismatch or a bad rig.
bool correctMotion(const MotionArrays& motion, const Masks& masks, const Rig& rig, const Params& params,
                   std::string* error = nullptr);

} // namespace AIMotion::KimodoCorrection
