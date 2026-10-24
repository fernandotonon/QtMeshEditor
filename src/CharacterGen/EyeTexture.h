#ifndef CHARACTERGEN_EYETEXTURE_H
#define CHARACTERGEN_EYETEXTURE_H

// Procedural eye of the character generator: an eyeball sphere with a polar UV map and a texture painted from code (sclera with
// faint veins, a fibrous iris with a collarette and a dark limbal ring, a pupil). Everything is generated, so there is no asset to
// license, the iris takes ANY colour, and it ships with the editor. Pure data: no Ogre, no assets.

#include "CharacterModel.h"

#include <QByteArray>
#include <QImage>

#include <array>
#include <cstdint>
#include <vector>

namespace CharacterGen {

/// A unit eyeball. The iris pole is -Y (the layout of the textured-eye OBJ convention; buildEyeMesh turns it to +Z).
struct EyeSphere {
    std::vector<float> positions;      ///< xyz, unit length
    std::vector<float> uvs;            ///< uv per vertex, in [0,1]: a polar map centred on the iris pole
    std::vector<uint32_t> indices;     ///< triangles, counter-clockwise seen from outside
};

/// A UV sphere with single-vertex poles. Rings sit at half the pupil angle, at the pupil edge, twice in between pupil and limbus, at
/// the limbus, and then `rings` more evenly up to the back pole; `segments` around.
EyeSphere makeEyeSphere(int rings = 22, int segments = 48);

/// Radius in the texture (0 = the iris pole, 1 = the edge of the painted disc) of a point `theta` radians from the iris pole.
/// Linear up to theta = 72 deg (the front of the eye, where the detail is), compressed beyond it.
float eyeUvRadius(float theta);

/// Angles (radians from the iris pole) of the pupil edge and of the limbus on the eyeball. Rings of the sphere sit exactly there.
float eyePupilAngle();
float eyeIrisAngle();

/// Move a point of the unit eyeball (pole -Y) so the pupil edge, which is at eyePupilAngle(), lands at `newPupilAngle`: everything
/// nearer the pole scales with it, the band between the pupil and the limbus stretches / compresses, the limbus and beyond stay.
/// The texture keeps its UVs, so the iris texture follows the geometry (the way a real iris does).
std::array<float, 3> dilateEyePoint(const std::array<float, 3>& p, float newPupilAngle);

/// Radii in the texture of the pupil edge and of the iris edge (the limbus).
float eyePupilRadius();
float eyeIrisRadius();

/// The eye texture (RGB, opaque) for an iris colour. Same inputs, same pixels.
QImage bakeEyeTexture(const Rgb& iris, int size = 1024, uint32_t seed = 1);

/// The same as PNG bytes (empty when it cannot be encoded).
QByteArray bakeEyeTexturePng(const Rgb& iris, int size = 1024, uint32_t seed = 1);

}  // namespace CharacterGen

#endif  // CHARACTERGEN_EYETEXTURE_H
