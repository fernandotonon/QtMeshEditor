#ifndef CHARACTERGEN_SKINTEXTURE_H
#define CHARACTERGEN_SKINTEXTURE_H

// Procedural skin texture of the character generator: an albedo baked in the body's UV layout from 3D noise sampled at each texel's
// surface position (so it is continuous across UV seams and has the same grain everywhere, whatever the UV stretch). Pure data:
// no Ogre, no assets. Tone variation, blush, freckles and pores around the requested skin colour.

#include "CharacterModel.h"

#include <QImage>

#include <cstdint>

namespace CharacterGen {

/// Where to paint the lips (a copy of the built body's lip frame).
using SkinLips = LipFrame;
using SkinBrows = BrowFrame;
using SkinNipples = NippleFrame;

struct SkinTextureOptions {
    int size = 1024;                ///< width = height, pixels
    uint32_t seed = 1;              ///< the same seed bakes the same skin
    float freckles = -1.0f;         ///< 0..1 density; negative = by skin tone (fairer skin freckles more)
    float poreStrength = 1.0f;      ///< 0 = smooth skin
    SkinLips lips;                  ///< valid = paint the lips
    SkinBrows brows;                ///< valid = paint the eyebrows
    SkinNipples nipples;            ///< valid = paint the areolae and nipples
};

/// Bake the albedo of `body` (positions in metres, UVs + layouts + faces as the rig stores them: the V flip is applied here).
/// Returns a null image when the mesh has no UVs. Texels no triangle covers are filled from their neighbours.
QImage bakeSkinAlbedo(const Dna::Mesh& body, const Rgb& skin, const SkinTextureOptions& options = {});

/// Lip colour for a skin colour: a warm pink-red a little darker than the skin (browner on dark skin).
Rgb lipColourFor(const Rgb& skin);

/// 0..1 coverage of the lips at a surface point (metres): the band around the lip seam, soft at its edges.
float lipMask(const std::array<float, 3>& position, const SkinLips& lips);

/// Eyebrow hair colour for a skin colour (fair skin: light brown, dark skin: near black).
Rgb browColourFor(const Rgb& skin);

/// 0..1 coverage of the eyebrows at a surface point (metres): an arched band above each eye, thick at the inner end and tapering to the tail.
float browMask(const std::array<float, 3>& position, const SkinBrows& brows);

/// Areola and nipple colours for a skin colour (a little darker and redder than the skin; browner on dark skin).
Rgb areolaColourFor(const Rgb& skin);
Rgb nippleColourFor(const Rgb& skin);

struct NippleMask {
    float areola = 0.0f;   ///< 0..1 coverage of the areola disc (includes the nipple)
    float core = 0.0f;     ///< 0..1 coverage of the nipple itself
};
/// Coverage of the two areola discs (radius 1.6 cm, soft edge) and nipples (0.5 cm) at a surface point (metres), scaled by the body.
NippleMask nippleMask(const std::array<float, 3>& position, const SkinNipples& nipples);

/// The skin colour at a surface point (metres), before the texture's coverage logic. Exposed for tests.
Rgb skinColourAt(const std::array<float, 3>& position, const Rgb& skin, const SkinTextureOptions& options);

}  // namespace CharacterGen

#endif  // CHARACTERGEN_SKINTEXTURE_H
