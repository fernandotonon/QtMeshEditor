#ifndef SKIN_EVALUATE_H
#define SKIN_EVALUATE_H

#include "SkinWeights.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <vector>

namespace Ogre {
    class Entity;
}

// `qtmesh skin --evaluate` / `--compare` (issue #819, Slice E).
// Extracts an entity's EXISTING bone assignments (whatever tool
// authored them — ours, Mixamo, Blender…) into the pure-data shapes
// SkinMetrics / SkinWeightsPost consume, and produces the metric
// report the acceptance suite and the Mixamo comparison protocol
// (docs/SKINNING_QUALITY.md) are built on.

class SkinEvaluate {
public:
    // Flattened whole-mesh view of an entity's skinning, combined
    // across vertex-data owners the same way computeAndApply
    // computes (shared block first, then per-submesh blocks).
    // `weights[v]` bone indices are Ogre bone HANDLES.
    struct EvalData {
        std::vector<float>          positions;   // xyz per vertex
        std::vector<std::uint32_t>  indices;     // triangle list
        std::vector<SkinWeights::VertexWeights> weights;
        QStringList boneNames;                   // by handle
        int totalBones = 0;
    };
    static bool extract(Ogre::Entity* entity, EvalData& out,
                        QString* error = nullptr);

    /// Write `data.weights` back onto the entity's mesh — the exact inverse of
    /// `extract`, walking vertex-data owners in the SAME order (shared block
    /// first, then each non-shared submesh) so the flattened global vertex
    /// index maps back onto the right owner-local index. Getting that walk
    /// wrong scatters weights onto unrelated vertices, which is why this lives
    /// here beside `extract` rather than being re-derived per caller.
    ///
    /// This is the IN-PLACE write: `clearBoneAssignments` +
    /// `addBoneAssignment`* + `_compileBoneAssignments` per owner, with NO
    /// `Entity::_initialise`. Re-initialising would re-pack BLEND_INDICES /
    /// WEIGHTS into the same vertex buffer a live SkeletonInstance already
    /// references, which shatters the on-screen mesh (see SkinWeightController
    /// and the UV-unwrap notes). Headless callers are unaffected either way,
    /// but sharing one write path keeps GUI and CLI results identical.
    static bool applyWeights(Ogre::Entity* entity, const EvalData& data,
                             QString* error = nullptr);

    // Metric report on the entity's existing weights:
    //   influence histogram, Laplacian smoothness energy, and —
    //   when the mesh encloses volume — the geodesic bleed fraction
    //   (GeodesicVoxelBind field at `voxelResolution`).
    static QJsonObject evaluate(Ogre::Entity* entity,
                                int voxelResolution = 64,
                                QString* error = nullptr);

    // Weight comparison against a reference-skinned copy of the
    // same asset (the Mixamo protocol): vertices are matched by
    // POSITION (reference exports reorder them), then per-vertex
    // weight vectors compared with bones matched by NAME. Reports
    // mean/max L1 difference and the top differing bones.
    static QJsonObject compare(Ogre::Entity* entity,
                               Ogre::Entity* reference,
                               QString* error = nullptr);

    static QString reportToText(const QJsonObject& report);
};

#endif // SKIN_EVALUATE_H
