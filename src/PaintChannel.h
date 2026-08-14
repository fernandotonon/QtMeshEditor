/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#ifndef PAINTCHANNEL_H
#define PAINTCHANNEL_H

// Paint v2 Slice D (#547) — the PBR channels a paint layer can target.
//
// Pure-data, no Qt/Ogre runtime dependency, so it is unit-testable and shared
// by PaintLayerStack, TexturePaintController and PaintChannelPresets. Each
// channel maps to a canonical Ogre material texture-unit-state (TUS) slot name
// (the same names RTShaderHelper::wirePbrSlotsForFFP / the Cook-Torrance SRS
// recognise), except:
//   * Height has NO direct slot — painted grayscale accumulates as a heightmap
//     and is Sobel-baked into the Normal (normal_map) slot.
//   * VertexColor is not a texture channel at all — it routes to the existing
//     per-vertex paint path (EditModeController), so it has no slot name.

#include <string>

namespace PaintChannelNS {

enum class Channel {
    BaseColor = 0,  ///< albedo / diffuse_map (sRGB colour)
    Normal,         ///< normal_map (tangent-space; RTSS SRS_NORMALMAP)
    Roughness,      ///< scalar 0..1 -> ORM .g
    Metallic,       ///< scalar 0..1 -> ORM .b
    AO,             ///< scalar 0..1 -> ORM .r (ambient occlusion)
    Emissive,       ///< emissive (RGB, opacity = intensity)
    Height,         ///< scalar heightmap; baked -> Normal
    VertexColor,    ///< per-vertex colour (not a texture slot)
    Count
};

/// Number of texture-painting channels shown in the brush-panel picker
/// (everything except VertexColor, which has its own Texture/Vertex toggle).
inline constexpr int kTexturePaintChannelCount = 7;

/// True for single-channel 0..1 data (painted grayscale, collapsed at bake).
inline bool isScalar(Channel c)
{
    return c == Channel::Roughness || c == Channel::Metallic
        || c == Channel::AO || c == Channel::Height;
}

/// True for full-RGB colour channels (blitted straight to their slot).
inline bool isColor(Channel c)
{
    return c == Channel::BaseColor || c == Channel::Emissive;
}

/// Canonical Ogre TUS slot name for a channel, or "" when the channel has no
/// direct texture slot (Height bakes into Normal; VertexColor has none).
inline const char* slotName(Channel c)
{
    switch (c) {
    case Channel::BaseColor: return "albedo";
    case Channel::Normal:    return "normal_map";
    case Channel::Roughness: return "roughness";
    case Channel::Metallic:  return "metallic";
    case Channel::AO:        return "ao";
    case Channel::Emissive:  return "emissive";
    case Channel::Height:    return "";  // bakes into Normal
    case Channel::VertexColor: return "";
    default: return "";
    }
}

/// Stable lowercase id used for QML, telemetry breadcrumbs and session keys.
inline const char* id(Channel c)
{
    switch (c) {
    case Channel::BaseColor:   return "basecolor";
    case Channel::Normal:      return "normal";
    case Channel::Roughness:   return "roughness";
    case Channel::Metallic:    return "metallic";
    case Channel::AO:          return "ao";
    case Channel::Emissive:    return "emissive";
    case Channel::Height:      return "height";
    case Channel::VertexColor: return "vertexcolor";
    default: return "";
    }
}

/// Human label for the channel-picker buttons.
inline const char* label(Channel c)
{
    switch (c) {
    case Channel::BaseColor:   return "Base Color";
    case Channel::Normal:      return "Normal";
    case Channel::Roughness:   return "Roughness";
    case Channel::Metallic:    return "Metallic";
    case Channel::AO:          return "AO";
    case Channel::Emissive:    return "Emissive";
    case Channel::Height:      return "Height";
    case Channel::VertexColor: return "Vertex Color";
    default: return "";
    }
}

/// Default brush RGBA the picker seeds when a channel is first selected.
/// Colour channels start white; scalar/height start mid-to-white grayscale so
/// a first stroke is visible against a black (0) empty layer. Returns packed
/// 0xRRGGBBAA-agnostic components via out params (0..255).
inline void defaultBrushColor(Channel c, int& r, int& g, int& b)
{
    if (isScalar(c)) { r = g = b = 255; return; }   // full roughness/metallic/ao/height
    r = g = b = 255;                                 // white colour default
}

/// Resolve a channel back from its stable id (for QML / presets). Returns
/// Channel::Count on no match.
inline Channel fromId(const std::string& s)
{
    for (int i = 0; i < static_cast<int>(Channel::Count); ++i) {
        auto c = static_cast<Channel>(i);
        if (s == id(c)) return c;
    }
    return Channel::Count;
}

} // namespace PaintChannelNS

#endif // PAINTCHANNEL_H
