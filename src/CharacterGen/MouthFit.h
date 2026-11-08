// Mouth containment metric for a built character: are the teeth / gums / tongue inside the head skin, at rest and under the jaw and
// lip shapes? Pure data (no Ogre): it works on the Dna::Rig the generator produces, so a placement change can be judged by numbers
// instead of by close-up renders.
#pragma once

#include "../DNA/DnaReader.h"

#include <QString>
#include <QStringList>

#include <array>
#include <vector>

namespace CharacterGen {
namespace MouthFit {

struct Sample {
    QString shape;          ///< ARKit channel ("" = rest pose)
    float weight = 0.0f;
    int interiorVertices = 0;
    int outside = 0;        ///< interior vertices that sit OUTSIDE the head skin
    float worst = 0.0f;     ///< largest distance (m) of an outside vertex to the nearest skin vertex
    float outsideMin[3] = {0, 0, 0};   ///< bounding box of the outside vertices (m) relative to the interior's own box centre
    float outsideMax[3] = {0, 0, 0};
};

/// Ray-parity inside test of every vertex of the meshes `interiorNames` (default head_mouth + head_teeth) against the triangles of the
/// mesh `skinName` (default body), with the named channels applied at their weights to BOTH sides. A vertex is inside when the
/// majority of three axis rays (forward, back, up) cross the skin an odd number of times.
std::vector<Sample> measure(const Dna::Rig& rig, const std::vector<std::pair<QString, float>>& shapes,
                            const QString& skinName = QStringLiteral("body"),
                            const QStringList& interiorNames = {QStringLiteral("head_mouth"), QStringLiteral("head_teeth")});

/// Extra translation (m) that brings the interior vertices inside the skin of `skin` (positions + faces as the rig stores them): the
/// outside vertices vote for the direction of their nearest skin vertices, the interior moves that way (plus `margin`) and is tested
/// again, up to `maxIterations` times. Only every `stride`-th interior vertex is tested. `outsideBefore` / `outsideAfter` (optional)
/// report how many of the tested vertices were outside before and after. Only skin within `nearRadius` (m) of the interior is considered: the rays leave through the face, the forehead and the back of the skull. The result is capped at `maxShift` per axis.
std::array<float, 3> clearanceShift(const Dna::Mesh& skin, const std::vector<float>& interior, float margin = 0.0015f, float maxShift = 0.025f,
                                    int maxIterations = 12, int stride = 4, int* outsideBefore = nullptr, int* outsideAfter = nullptr,
                                    float nearRadius = 0.3f);

/// Jaw travel of the mouth interior relative to its ICT deltas, chosen per face: of `candidates`, the factor under which the fewest
/// sampled interior vertices leave the skin when `skinDelta` (3 floats per skin vertex, the jaw shape of the skin) and
/// `factor * interiorDelta` are applied at weights 1 and 0.6. Ties and an empty / unmeasurable problem give `fallback`.
float chooseJawFollow(const Dna::Mesh& skin, const std::vector<float>& skinDelta, const std::vector<float>& interior, const std::vector<float>& interiorDelta,
                      const std::vector<float>& candidates, float fallback, std::vector<int>* outsideCounts = nullptr, float nearRadius = 0.35f);

/// Odd/even crossing count of a ray against a triangle soup (exposed for tests).
int rayCrossings(const std::vector<float>& positions, const std::vector<uint32_t>& triangles, const float origin[3], const float dir[3]);

}  // namespace MouthFit
}  // namespace CharacterGen
