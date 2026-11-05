#ifndef CHARACTERGEN_HAIRFITTING_H
#define CHARACTERGEN_HAIRFITTING_H

// Hair Studio, fitting half: adapts an IMPORTED hairstyle (any triangle mesh authored for some head) to the shaped MHR head.
// Pure data, conservative by design — it does not shrink-wrap every vertex to the scalp:
//   1. normalise: the source's crown (its upper part) is matched to the head width, its top to the head top, centred on the head;
//   2. root band: only vertices within a thin band of the head surface (or inside it) are corrected, with a displacement that
//      falls off smoothly with the distance to the surface, then relaxed over the mesh so neighbours follow without kinks;
//   3. everything beyond the band (the outer silhouette, long lengths, curls) keeps its authored shape.
// Generic: it takes a `BodyCollider` + `HeadFrame`, not MHR specifics, so another character type can reuse it.

#include "ProceduralHair.h"

namespace CharacterGen {
namespace Hair {

/// A hairstyle extracted from a source file: a triangle soup with one UV per vertex, in the source's own units and orientation.
struct ImportedHair {
    QString id, name;
    std::vector<V3> pos;
    std::vector<V3> nrm;               ///< optional (recomputed when empty)
    std::vector<float> uv;             ///< 2 per vertex
    std::vector<uint32_t> idx;
    QByteArray texturePng;             ///< diffuse/albedo (PNG bytes, alpha preserved); may be empty
    int triangles() const { return int(idx.size() / 3); }
};

/// The user's manual correction on top of the automatic fit.
struct FitAdjust {
    V3 translate;                      ///< metres, in the head frame (x character's left, y up, z forward)
    V3 rotateDeg;                      ///< pitch (x), yaw (y), roll (z)
    float scale = 1.0f;                ///< multiplier on the automatic scale
    float snug = 0.35f;                ///< 0..1 how much the root band is pulled towards the scalp
    float clearance = 0.003f;          ///< metres kept between hair and skin
    bool flipFront = false;            ///< the source faces -Z: turn it 180 degrees
    bool autoFit = true;               ///< false: use the source as it is (still moved by `translate`...)
};

QJsonObject toJson(const FitAdjust& a);
FitAdjust fitAdjustFromJson(const QJsonObject& o);

struct FitReport {
    float scale = 1.0f;
    int penetrating = 0;               ///< vertices that were inside the head and pushed out
    int snapped = 0;                   ///< vertices that were pulled in
    float maxPush = 0;                 ///< metres
    bool facingFlipped = false;
    QStringList warnings;
};

/// Fit `hair` to the head. Returns the new positions (same count/order as `hair.pos`) and normals recomputed from the triangles.
void fitToHead(const ImportedHair& hair, const HeadFrame& head, const BodyCollider& collider, const FitAdjust& adj,
               std::vector<V3>* outPos, std::vector<V3>* outNrm, FitReport* report = nullptr);

/// Count vertices that are inside the body by more than `tolerance` (a quality check for tests and the UI's warning).
int countPenetrations(const std::vector<V3>& pos, const BodyCollider& collider, float tolerance = 0.0f);

/// A DNA mesh ("head_hair") from fitted geometry; skin rows come from the nearest body vertex.
Dna::Mesh importedToDnaMesh(const ImportedHair& hair, const std::vector<V3>& pos, const std::vector<V3>& nrm, const BodyCollider& collider,
                            const std::vector<std::vector<Dna::SkinInfluence>>& bodySkin);

}  // namespace Hair
}  // namespace CharacterGen

#endif
