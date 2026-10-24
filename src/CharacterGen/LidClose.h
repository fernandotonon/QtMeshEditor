#ifndef CHARACTERGEN_LIDCLOSE_H
#define CHARACTERGEN_LIDCLOSE_H

// Relaxed lids of the character generator (Params::relaxedLids): a lower rest pose and an eyeBlink that closes the eye completely.
// The upper lid skin TURNS about the eyeball's horizontal axis (x through its centre), so it keeps its distance from the eyeball and
// cannot sink into it; what the turn leaves at the corners (the margins differ in depth there) is snapped onto the lower margin and
// spread into the skin. Pure data: no Ogre, no assets.

#include "Eyelashes.h"

#include <array>
#include <cstdint>
#include <vector>

namespace CharacterGen {

struct LidChannels {
    std::array<int, 2> blink{{-1, -1}};   ///< eyeBlinkLeft / eyeBlinkRight indices into the ARKit shape list (character sides: left = +X)
    std::array<int, 2> wide{{-1, -1}};    ///< eyeWideLeft / eyeWideRight (optional, -1 = none)
};

/// Re-base the lid shapes. `positions` (V*3, metres, the open pose) become the new rest pose: the old open pose plus the old blink.
/// `arkit` (E*V*3, cm) is the original shape table; the returned copy has the blink channels replaced by "the rest of the way to
/// closed" and the wide channels re-based so they still reach the old open pose. An eye that is not seated, or whose margins are not
/// found, is left exactly as it was: its rest pose, blink and wide shapes are untouched. `rebased` (optional) reports which eyes were
/// re-based, so callers only attach follow-up work (the lash tips turning) to those.
std::vector<float> relaxLids(std::vector<float>& positions, int vertexCount, const std::vector<int32_t>& faces,
                             const std::vector<uint8_t>& dropped, const std::array<LashEye, 2>& eyes, const LidChannels& channels,
                             const std::vector<float>& arkit, std::array<bool, 2>* rebased = nullptr);

}  // namespace CharacterGen

#endif  // CHARACTERGEN_LIDCLOSE_H
