#ifndef CHARACTERGEN_EYELASHES_H
#define CHARACTERGEN_EYELASHES_H

// Eyelashes of the character generator: a thin curved strip (an alpha card) along each eyelid margin with a procedurally painted
// strand texture. The margin is the boundary of the almond opening cut through the MHR lids (the dropped triangles), so every strip
// root IS a body vertex: the strip follows the lids' morph targets (blink, squint, ...) simply by copying those vertices' deltas.
// Pure data: no Ogre, no assets.

#include "CharacterModel.h"

#include <QImage>

#include <array>
#include <cstdint>
#include <vector>

namespace CharacterGen {

struct LashOptions {
    float upperLength = 0.011f;   ///< metres at the longest point of the upper lid
    float lowerLength = 0.005f;   ///< metres; 0 = no lower lashes
    float curl = 0.55f;           ///< 0 = straight out from the eyeball, 1 = strongly curled up (upper) / down (lower)
};

struct LashMesh {
    std::vector<float> positions;              ///< xyz, metres
    std::vector<float> normals;                ///< xyz, unit
    std::vector<float> uvs;                    ///< uv per vertex: u along the strip 0..1, v 1 at the root to 0 at the tip
    std::vector<uint32_t> indices;             ///< triangles
    std::vector<uint32_t> sourceVertex;        ///< per vertex: the body (position) vertex it is rooted on
    int upperStrips = 0, lowerStrips = 0;      ///< strips built (0..2 each)
};

/// Build the strips. `positions` = the body's positions (V*3, metres), `faces` = F*3 position indices, `dropped` = one flag per
/// triangle (the opening). `eyeCentres[0]` is the character's left eyeball (+X, the face looks +Z, up is +Y), `[1]` the right; a
/// `seated` false entry builds nothing for that eye.
struct LashEye { std::array<float, 3> centre{}; bool seated = false; };
LashMesh buildLashes(const std::vector<float>& positions, const std::vector<int32_t>& faces, const std::vector<uint8_t>& dropped,
                     const std::array<LashEye, 2>& eyes, const LashOptions& options = {});

/// The two lid margins of one eye, each ordered from the inner canthus (nose side) to the outer one: body vertex indices.
struct LidMargins {
    std::vector<uint32_t> upper, lower;
    bool valid() const { return upper.size() >= 2 && lower.size() >= 2; }
};
/// Find the margins of both eyes (same inputs as buildLashes). An eye that is not seated, or whose opening is not found, comes back
/// invalid.
std::array<LidMargins, 2> findLidMargins(const std::vector<float>& positions, const std::vector<int32_t>& faces,
                                         const std::vector<uint8_t>& dropped, const std::array<LashEye, 2>& eyes);

/// The strand texture (RGBA, 1024x128 by default): dark opaque root band, tapering curved strands, soft alpha. `colour` is the lash
/// colour. Same inputs, same pixels.
QImage bakeLashTexture(const Rgb& colour, int width = 1024, int height = 128, uint32_t seed = 1);
QByteArray bakeLashTexturePng(const Rgb& colour, int width = 1024, int height = 128, uint32_t seed = 1);

}  // namespace CharacterGen

#endif  // CHARACTERGEN_EYELASHES_H
