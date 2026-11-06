// Body proportions on top of the 45 MHR identity modes: those modes barely change height or limb length (MHR keeps those in its 68
// scale parameters, which the generator does not carry), so a character built from them is always about 173 cm tall with the same
// legs and arms. Here the skeleton is scaled like a rig is: each joint has an inherited uniform scale and a bone-length factor, the
// scaled hierarchy gives the new joint positions, and every vertex follows by linear blend skinning. Pure data.
#pragma once

#include "CharacterModel.h"

#include <array>
#include <vector>

namespace CharacterGen {

struct ProportionFrame {
    bool active = false;
    std::vector<std::array<float, 3>> oldPos, newPos;   ///< per joint, world cm (newPos is lifted so the lowest vertex stays where it was)
    std::vector<float> scale;                            ///< per joint, the accumulated uniform scale
    float lift = 0.0f;                                   ///< cm
};

/// Scale the joint hierarchy (`gt`: world positions in cm, parents before children) and warp `positions` (3 floats per vertex, cm) by
/// linear blend skinning with the assets' weights. `gt` is replaced by the new joint positions. Returns an inactive frame (and leaves
/// everything alone) when `p.isDefault()` or the assets lack skin weights.
ProportionFrame applyProportions(const Assets& a, const Proportions& p, std::vector<std::array<float, 3>>& gt, std::vector<float>& positions);

}  // namespace CharacterGen
