#ifndef CHARACTERGEN_SKINTEXTURE_H
#define CHARACTERGEN_SKINTEXTURE_H

// Procedural skin texture of the character generator: an albedo baked in the body's UV layout from 3D noise sampled at each texel's
// surface position (so it is continuous across UV seams and has the same grain everywhere, whatever the UV stretch). Pure data:
// no Ogre, no assets. Tone variation, blush, freckles and pores around the requested skin colour.

#include "CharacterModel.h"

#include <QImage>

#include <cstdint>

namespace CharacterGen {

struct SkinTextureOptions {
    int size = 1024;                ///< width = height, pixels
    uint32_t seed = 1;              ///< the same seed bakes the same skin
    float freckles = -1.0f;         ///< 0..1 density; negative = by skin tone (fairer skin freckles more)
    float poreStrength = 1.0f;      ///< 0 = smooth skin
};

/// Bake the albedo of `body` (positions in metres, UVs + layouts + faces as the rig stores them: the V flip is applied here).
/// Returns a null image when the mesh has no UVs. Texels no triangle covers are filled from their neighbours.
QImage bakeSkinAlbedo(const Dna::Mesh& body, const Rgb& skin, const SkinTextureOptions& options = {});

/// The skin colour at a surface point (metres), before the texture's coverage logic. Exposed for tests.
Rgb skinColourAt(const std::array<float, 3>& position, const Rgb& skin, const SkinTextureOptions& options);

}  // namespace CharacterGen

#endif  // CHARACTERGEN_SKINTEXTURE_H
