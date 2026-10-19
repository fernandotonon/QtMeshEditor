#ifndef DNA_DNAFACEBAKE_H
#define DNA_DNAFACEBAKE_H

// Bake a MetaHuman face rig into morph targets (epic #1095, Phase 3).
//
// The face DNA is a JOINT rig: ~870 joints, ~700 of them skinned, driven by 263
// expression controls through PSD/RBF solvers. Ogre cannot hold a skeleton that
// large (256 bones), and the editor's face-capture, lipsync and export paths all
// speak morph targets. So instead of playing the joints, each shape is BAKED:
//
//   delta = skin(rig evaluated with the shape's controls) - skin(neutral)
//
// where skin() is full linear-blend skinning on the COMPLETE skeleton with every
// influence (MetaHuman uses up to 10 per vertex; Ogre would clip to 4) — so a
// single baked shape is exact for that control set. What a morph target cannot
// keep is the rig's nonlinearity: two shapes added together differ from the rig
// evaluated with both controls at once. `evaluatePositions()` exposes the exact
// rig result so that error is measurable (see the tests / `qtmesh dna face`).
//
// Pure data and Ogre-free: the evaluator is the only collaborator.

#include "DnaArkitMap.h"
#include "DnaReader.h"
#include "DnaRigLogic.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <vector>

namespace Dna {
namespace FaceBake {

/// One shape to bake: a name and the raw controls (short names, no
/// "CTRL_expressions." prefix, or full names) with the value each is driven to.
struct ShapeSpec {
    QString name;
    QList<QPair<QString, double>> controls;
};

struct SparseDelta {
    uint32_t vertex = 0;                 ///< POSITION index in the DNA mesh
    std::array<float, 3> delta{};        ///< in the rig's units (metres by default)
};

struct BakedShape {
    QString name;
    /// One list per entry of rig.meshes, in order. Only vertices that moved.
    std::vector<std::vector<SparseDelta>> perMesh;
    double maxDeltaMm = 0.0;             ///< largest vertex displacement, in millimetres
    int vertices = 0;                    ///< vertices that moved
    QStringList missingControls;         ///< controls the DNA does not have
};

struct Options {
    double epsilonMetres = 1e-6;         ///< ignore displacements below 0.001 mm
    double unitToMillimetres = 1000.0;   ///< rig units -> mm (metres by default)
};

struct Result {
    std::vector<BakedShape> shapes;
    QStringList warnings;
};

/// Skinned positions of every mesh for the given control values (everything else
/// at rest): x,y,z per vertex, one vector per entry of rig.meshes. Uses the full
/// skeleton and every skin influence. `rig` must be the UNPRUNED rig that the
/// evaluator's joint indices refer to.
std::vector<std::vector<float>> evaluatePositions(const Rig& rig, RigEvaluator& ev,
                                                  const QList<QPair<QString, double>>& controls,
                                                  QStringList* missing = nullptr);

/// Ground truth for a BODY pose: the full skeleton and every skin influence, with each named driver
/// joint rotated by `rotations` (joint name -> w,x,y,z, relative to its neutral orientation, composed
/// after it) and every rig-driven (corrective) joint taken from RigLogic. `rig` must be the UNPRUNED
/// rig the evaluator's joint indices refer to.
///
/// `withCorrectives=false` leaves every rig-driven joint at its neutral local pose, i.e. PLAIN skinning
/// of the same driver rotations (what the viewport shows with the corrective toggle off).
std::vector<std::vector<float>> evaluatePositionsPosed(const Rig& rig, RigEvaluator& ev,
                                                       const QList<QPair<QString, std::array<double, 4>>>& rotations,
                                                       bool withCorrectives = true);

/// The blended 3x3 (row-major) skinning matrix of every vertex at that pose — what a REST-space corrective
/// must be carried through. Same arguments as evaluatePositionsPosed.
std::vector<std::vector<std::array<float, 9>>> evaluateBlendedLinear(
    const Rig& rig, RigEvaluator& ev, const QList<QPair<QString, std::array<double, 4>>>& rotations,
    bool withCorrectives = false);

/// Bake `specs` against the neutral pose.
Result bake(const Rig& rig, RigEvaluator& ev, const QList<ShapeSpec>& specs, const Options& options = Options());

/// How far a sum of baked shapes is from the rig evaluated with every control
/// at once (the nonlinearity a morph target cannot keep), over all vertices.
struct LinearityReport {
    double maxMm = 0.0;
    double meanMm = 0.0;
    double combinedMaxMm = 0.0;   ///< size of the combined expression itself, for scale
    int vertices = 0;
};
LinearityReport linearityError(const Rig& rig, RigEvaluator& ev, const QList<ShapeSpec>& parts,
                               const Options& options = Options());

/// ARKit shapes from a mapping (every entry becomes one ShapeSpec).
QList<ShapeSpec> arkitSpecs(const QList<ArkitMap::Entry>& map);

/// One shape per expression raw control, named "CTRL_expressions_<name>".
QList<ShapeSpec> allExpressionSpecs(const RigEvaluator& ev);

/// Append the baked shapes to `rig` as blend-shape channels + per-mesh targets, so
/// the importer's existing morph attachment picks them up. Channels that already
/// exist are not duplicated. Returns the number of channels added.
int appendAsBlendShapes(Rig& rig, const Result& result);

}  // namespace FaceBake
}  // namespace Dna

#endif  // DNA_DNAFACEBAKE_H
