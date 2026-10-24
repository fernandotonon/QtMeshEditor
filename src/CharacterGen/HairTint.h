#ifndef CHARACTERGEN_HAIRTINT_H
#define CHARACTERGEN_HAIRTINT_H

// Hair colour of the character generator. The hair bundle stores each style's texture as a neutral SHADING map (greyscale strand
// detail + alpha, see scripts/hair-shading-maps.py); the colour is applied here, so one texture serves every hair colour and the
// picker is continuous. Pure data (QImage in / out), no Ogre.

#include "CharacterModel.h"

#include <QByteArray>
#include <QImage>
#include <QString>

#include <vector>

namespace CharacterGen {

struct HairColourPreset {
    QString name;   ///< lowercase id: black, dark_brown, brown, light_brown, blonde, platinum_blonde, red, gray, white
    QString label;  ///< display name
    Rgb colour;     ///< the colour of the texture's mid tones
};

/// The nine presets, sampled from the reference hair colour chart (black ... white).
const std::vector<HairColourPreset>& hairColourPresets();

/// A preset id ("dark_brown", "dark brown", "Dark-Brown") or a "#rrggbb" colour. False (and `out` untouched) when it is neither.
bool parseHairColour(const QString& text, Rgb* out);

/// "#rrggbb" of a colour (the inverse of the hex branch of parseHairColour).
QString hairColourHex(const Rgb& colour);

/// Tint a shading map: the opaque pixels' median luminance maps to `colour`, darker pixels towards a darker shade of it (the
/// strands' depth) and brighter ones towards a lighter one (the highlights). Alpha is kept. Normalising on the median makes a map
/// that still has its original colour (an older bundle) work too. A null image stays null.
QImage tintHairShading(const QImage& shading, const Rgb& colour);

/// The same from / to PNG bytes (empty on a decode failure).
QByteArray tintHairTexturePng(const QByteArray& shadingPng, const Rgb& colour);

}  // namespace CharacterGen

#endif  // CHARACTERGEN_HAIRTINT_H
