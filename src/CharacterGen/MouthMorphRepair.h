#ifndef CHARACTERGEN_MOUTHMORPHREPAIR_H
#define CHARACTERGEN_MOUTHMORPHREPAIR_H

// Repair of the MHR head's expression shapes around the mouth corners. The corner of the MHR mouth is a crumpled patch of tiny
// triangles where upper-lip and lower-lip vertices interleave; the shipped ARKit deltas move the upper-lip ones with the upper lip and
// the lower-lip ones with the jaw, so jawOpen (and the other mouth shapes) tear the patch into a fan of long thin triangles that render
// as dark shards in the corners. The repair finds the triangles a shape stretches far past their rest size near the mouth and replaces
// the deltas of their vertices by a harmonic interpolation of the surrounding, untorn deltas: the corner then opens like a hinge.
// Pure data: no Ogre, no assets.

#include <array>
#include <cstdint>
#include <vector>

namespace CharacterGen {

struct MouthRepairOptions {
    double stretchRatio = 1.8;        ///< a triangle whose longest edge grows past this factor is torn...
    double minGrowthM = 0.002;        ///< ...and grows by at least this much (metres)
    double flipDot = 0.2;             ///< ...or turns over (posed . rest normal below this)
    int iterations = 400;             ///< Jacobi iterations of the harmonic fill
    int passes = 2;                   ///< detect-and-fill rounds, each widening the filled area
};

/// `positions` (V*3, metres, rest pose), `faces` (triangles into V), `arkit` (E*V*3, cm, edited in place). Only faces with every
/// corner within `radius` of `centre` (metres) are checked. Returns the number of (channel, vertex) deltas replaced.
int repairMouthMorphs(const std::vector<float>& positions, int vertexCount, const std::vector<int32_t>& faces,
                      const std::array<double, 3>& centre, double radius, std::vector<float>& arkit, int channels,
                      const MouthRepairOptions& options = {});

/// The faces (indices into `faces` / 3) that some channel turns over at full or half weight. The corner of the mouth is a fold:
/// opening it turns part of the inner lip to face into the mouth, and no smoothing of the deltas undoes that without moving the
/// lips by centimetres, so the caller renders those faces from both sides instead.
std::vector<int32_t> mouthFacesTurnedOver(const std::vector<float>& positions, int vertexCount, const std::vector<int32_t>& faces,
                                          const std::array<double, 3>& centre, double radius, const std::vector<float>& arkit,
                                          int channels);

/// Triangles (a flat index list, wound like the surrounding faces) that close the small holes (boundary loops of at most `maxLoop`
/// vertices, every vertex within `radius` of `centre`) next to the mouth. The MHR head's mouth opening leaves a few-vertex slit beside
/// each corner: flat while the mouth is closed, so it never showed, it opens into a hole with the jaw. The mouth opening itself is a
/// much longer loop and is left open.
std::vector<int32_t> mouthHoleCaps(const std::vector<float>& positions, int vertexCount, const std::vector<int32_t>& faces,
                                   const std::array<double, 3>& centre, double radius, int maxLoop = 8);

/// Replaces the normals of `vertices` by a harmonic blend of the normals around them (the neighbours not in `vertices` stay fixed,
/// each listed vertex repeatedly takes the normalised mean of its 1-ring over `faces`). The closed mouth corner is a crumpled fold
/// whose own normals point straight up or down; the expression shapes open it into a surface between the upper and the lower lip,
/// and the morphs do not carry normals. Blending from the upper lip's normals on one side to the lower lip's on the other puts the
/// change of shading in the middle of the opened corner instead of along one of the lips. `normals` is indexed by vertex.
void blendCornerNormals(int vertexCount, const std::vector<int32_t>& faces, const std::vector<int32_t>& vertices,
                        std::vector<float>& normals, int iterations = 200);

/// Largest posed/rest longest-edge ratio over the checked faces of one channel (for tests and diagnostics).
double worstMouthStretch(const std::vector<float>& positions, int vertexCount, const std::vector<int32_t>& faces,
                         const std::array<double, 3>& centre, double radius, const std::vector<float>& arkit, int channel);

}  // namespace CharacterGen

#endif  // CHARACTERGEN_MOUTHMORPHREPAIR_H
