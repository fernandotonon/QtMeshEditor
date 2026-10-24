#ifndef CHARACTERGEN_CLOTHING_H
#define CHARACTERGEN_CLOTHING_H

// Procedural clothes of the character generator: a garment is the part of the body surface inside a smooth region (a scalar field
// built from the skeleton: hem height, neckline, sleeve / trouser-leg length along the limb), cut exactly along the field's zero
// contour and pushed outward by a few millimetres. It shares the body's UVs and skin weights, so it follows every body shape and every
// animation with no extra data. Pure data: no Ogre, no assets.

#include "CharacterModel.h"

#include <QStringList>

#include <array>
#include <map>

namespace CharacterGen {

enum class TopKind { None, TShirt, LongSleeve, Tank };
enum class BottomKind { None, Shorts, Pants };

/// Ids: "none", "tshirt", "longsleeve", "tank" / "none", "shorts", "pants".
QStringList topStyleIds();
QStringList bottomStyleIds();
bool parseTopStyle(const QString& id, TopKind* out);
bool parseBottomStyle(const QString& id, BottomKind* out);

/// A garment colour: "#rrggbb" or one of `clothColourNames()` (case and spacing insensitive).
QStringList clothColourNames();
bool parseClothColour(const QString& text, Rgb* out);

/// Joint positions of the built rig in the body's coordinates (metres; the face looks +Z, up is +Y, the character's left is +X).
using JointPositions = std::map<QString, std::array<double, 3>>;
/// World positions of every joint of a rig whose bones have identity orientations (the standard skeleton).
JointPositions jointWorldPositions(const Dna::Rig& rig);

/// Cut `body` along the zero contour of the per-POSITION scalar `field` (>= 0 is kept) and push the kept surface `thickness` metres
/// along the body normals. UVs, skin weights and (smooth) normals are carried over; cut vertices are interpolated. An empty mesh when
/// nothing is inside.
Dna::Mesh clipBodyByField(const Dna::Mesh& body, const std::vector<float>& field, float thickness);

/// The scalar fields (metres, positive inside the garment), exposed for tests. They are 0 / negative on the head, hands and feet.
float topField(TopKind style, const JointPositions& joints, const std::array<double, 3>& p);
float bottomField(BottomKind style, const JointPositions& joints, const std::array<double, 3>& p);

/// The garment meshes ("cloth_top" / "cloth_bottom"); empty when the style is None, a joint is missing or nothing is covered.
Dna::Mesh buildTop(TopKind style, const Dna::Mesh& body, const JointPositions& joints);
Dna::Mesh buildBottom(BottomKind style, const Dna::Mesh& body, const JointPositions& joints);

}  // namespace CharacterGen

#endif  // CHARACTERGEN_CLOTHING_H
