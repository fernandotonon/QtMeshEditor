#ifndef CHARACTERGEN_SKINTEXTURE_H
#define CHARACTERGEN_SKINTEXTURE_H

// Procedural skin texture of the character generator: an albedo baked in the body's UV layout from 3D noise sampled at each texel's
// surface position (so it is continuous across UV seams and has the same grain everywhere, whatever the UV stretch). Pure data:
// no Ogre, no assets. Tone variation, blush, freckles and pores around the requested skin colour.

#include "CharacterModel.h"

#include <QImage>

#include <cstdint>
#include <vector>

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
    float bodyHair = 0.0f;          ///< 0..1 amount of body hair (needs hairDensity)
    std::vector<float> hairDensity; ///< 0..1 per POSITION vertex of the baked mesh: where body hair may grow
    std::vector<float> scalp;       ///< 0..1 per POSITION vertex: the scalp (hair-covered skin of the head), painted in `scalpColour`
    Rgb scalpColour{0.24f, 0.145f, 0.09f};   ///< the hair colour the scalp is shaded towards
    QImage photo;                   ///< valid = project this photo onto the vertices that have a weight
    std::vector<float> photoPixel;  ///< 2 per POSITION vertex: where the vertex lands in `photo` (pixels)
    std::vector<float> photoWeight; ///< 0..1 per POSITION vertex: how much the photo replaces the procedural skin
    float photoTolerance = 0.24f;   ///< RGB distance from the skin tone beyond which a photo pixel fades out (hair, hats, make-up)
    float photoStrength = 0.92f;    ///< cap of the blend (the rest keeps pores and lips from the procedural skin)
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
/// `hairDensity` (0..1) is the body-hair density at this point (interpolated from the vertices); it scales with options.bodyHair.
Rgb skinColourAt(const std::array<float, 3>& position, const Rgb& skin, const SkinTextureOptions& options, float hairDensity = 0.0f, float scalp = 0.0f);

}  // namespace CharacterGen

#endif  // CHARACTERGEN_SKINTEXTURE_H
