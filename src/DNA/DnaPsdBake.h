#ifndef DNA_DNAPSDBAKE_H
#define DNA_DNAPSDBAKE_H

// Bake a DNA rig's correctives into generic pose-space RBF sets (epic #1095, Phase 4).
//
// The DNA drives its corrective BONES from RBF solvers whose data assumes the original proportions;
// a body fitted to another character keeps that data unchanged. PoseSpace::Set holds the same
// information as per-vertex offsets that live on the MESH, independent of RigLogic, so they can be
// carried through the fit warp (or authored by hand on a stylised character) and evaluated without the
// DNA at all.
//
// One set per driver joint. Sample poses rotate that joint alone about each local axis; the offset of
// a sample is the full rig's skinned position minus PLAIN skinning of the same pose, expressed in REST
// space (through the inverse of the vertex's blended skinning matrix) because that is where Ogre applies
// a pose offset — before skinning. Vertex indices run over every mesh of the rig, in order.
//
// Pure data and Ogre-free, like the rest of the DNA layer.

#include "../PoseSpaceRbf.h"
#include "DnaFaceBake.h"
#include "DnaFit.h"

#include <QStringList>

#include <vector>

namespace Dna {
namespace Psd {

struct BakeOptions {
    std::vector<double> anglesDeg{-90, -60, -30, 30, 60, 90};   ///< per axis, per driver
    double minDeltaMm = 0.05;                                   ///< offsets below this are dropped (sparse)
    double unitToMillimetres = 1000.0;
};

struct BakeResult {
    std::vector<PoseSpace::Set> sets;
    int driversBaked = 0;
    int driversWithoutEffect = 0;     ///< drivers whose rotation moves no corrective
    QStringList warnings;
};

BakeResult bake(const Rig& rig, RigEvaluator& ev, const BakeOptions& options = BakeOptions());

/// Plain skinning + the sets' offsets versus the full rig, over random multi-joint poses.
struct Validation {
    int poses = 0;
    double plainMeanMm = 0, plainP95Mm = 0, plainMaxMm = 0;   ///< error of plain skinning (what correctives fix)
    double psdMeanMm = 0, psdP95Mm = 0, psdMaxMm = 0;         ///< error with the baked sets applied
};

Validation validate(const Rig& rig, RigEvaluator& ev, const std::vector<PoseSpace::Set>& sets, int poses = 24,
                    unsigned seed = 1, double maxAngleDeg = 60.0, double unitToMillimetres = 1000.0);

/// Carry every offset through a per-vertex 3x3 (row-major; indexed like Set vertices: meshes in order) — used for a
/// rig whose joints were re-oriented (limb alignment / facing yaw): see Fit::blendedAlignmentLinear.
void applyVertexLinear(std::vector<PoseSpace::Set>& sets, const std::vector<std::vector<std::array<float, 9>>>& linearPerMesh);

/// The sets for the DNA at `path` / `rig`: read from <AppData>/dna_psd/ when a file baked from the same DNA (path,
/// size, mtime) and sampling exists, otherwise baked now and cached. Empty when the rig has nothing to bake.
/// `notes` receives a line saying which happened.
std::vector<PoseSpace::Set> cachedBake(const QString& path, const Rig& rig, RigEvaluator& ev, QStringList* notes = nullptr);

/// Attach every sample of every set to `rig` as a blend-shape channel named PoseSpace::channelName(set, i), with the
/// offsets split per mesh (Set vertex indices run over all meshes in order). The importer turns channels into Ogre
/// poses on the MESH before the entity exists, which is the only way a skinned mesh can carry poses; the corrective
/// manager then drives their weights from the driver joints. Returns the number of channels added.
int appendAsBlendShapes(Rig& rig, const std::vector<PoseSpace::Set>& sets);

/// Carry baked sets onto a rig that was fitted by `warp` (original rest -> fitted rest): every offset is a
/// rest-space vector, so it is transformed by the warp's local linear part J = I + d(displacement)/dp at the
/// vertex (central differences of the RBF field). A uniform scale s therefore scales every offset by s exactly,
/// and a limb that grew thicker gets a proportionally bigger bulge — the recalibration the DNA's own RBF data
/// cannot do. Valid when the fit did not re-pose the rig (no limb alignment): the driver axes are the same.
void transferThroughWarp(std::vector<PoseSpace::Set>& sets, const Rig& original, const Fit::Warp& warp);

/// Driver rotations (joint -> relative quaternion) of a set at `q`, as the evaluator wants them.
QList<QPair<QString, std::array<double, 4>>> toRotations(const QStringList& drivers, const std::vector<PoseSpace::Quat>& q);

}  // namespace Psd
}  // namespace Dna

#endif  // DNA_DNAPSDBAKE_H
