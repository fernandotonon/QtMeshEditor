#ifndef DNA_DNAFIT_H
#define DNA_DNAFIT_H

// Fit a MetaHuman body rig to an EDITED body mesh (epic #1095, Phase 4 prototype).
//
// What OpenRigLogic does and does not give us (verified against the 5.8 sources):
// it reads and WRITES DNA and evaluates the rig; it contains NO calibration code
// (the DNA calibration library is a separate Epic product, absent from the repo).
// So "fit the skeleton to a changed body" is our own solver, and this is a
// deliberately small, honest one:
//
//   1. correspondence — the target must have the SAME TOPOLOGY as the source
//      (vertex-for-vertex), e.g. a sculpt/lattice edit of the imported body, or
//      another MetaHuman body of the same LOD. Arbitrary topology is refused, not
//      guessed (see positionsFromExpanded for how seam-split vertices map back).
//   2. displacement field — target - source at every vertex.
//   3. warp — a smooth RBF (polyharmonic kernel + affine term, so affine edits such
//      as scaling are reproduced EXACTLY) interpolates that field; every joint is
//      moved by the warp evaluated at its neutral position. Joint ORIENTATIONS and
//      the skin weights are kept; local translations are re-derived.
//   4. the bind pose becomes the target mesh exactly (skinning at rest reproduces it).
//
// The same warp is a function of position, so it propagates to every LOD without a
// second correspondence (evaluateLodPropagation measures how well).
//
// NOT done, and reported as such: recalibration of the corrective data. RBF poses
// and joint-group deltas stay as authored for the original proportions, so large
// edits drift (qtmesh dna fit --poses quantifies pose quality without them).
//
// Pure data and Ogre-free.

#include "DnaReader.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include <array>
#include <vector>

namespace Dna {
namespace Fit {

struct Options {
    int maxControlPoints = 900;       ///< RBF centres, voxel-subsampled from the vertices
    double regularisation = 1e-9;     ///< diagonal ridge, relative to the kernel scale
    double unitToMillimetres = 1000;  ///< rig units -> mm (metres by default)
};

/// A smooth displacement field: polyharmonic RBF (phi(r) = r) + affine term.
struct Warp {
    std::vector<std::array<double, 3>> centres;
    std::vector<std::array<double, 3>> weights;   ///< one RBF weight vector per centre
    std::array<double, 3> constant{};             ///< affine part: d(p) += constant + M p
    std::array<std::array<double, 3>, 3> linear{};  ///< linear[axis][component]
    bool valid = false;

    /// Displacement at `p`.
    std::array<double, 3> displacement(const std::array<double, 3>& p) const;
};

/// Build the warp from matching source/target positions (x,y,z per vertex, any
/// number of meshes concatenated by the caller). Returns an invalid warp + error
/// when there are too few / degenerate points.
Warp buildWarp(const std::vector<std::array<double, 3>>& source,
               const std::vector<std::array<double, 3>>& target, const Options& options,
               QString* error = nullptr);

struct Result {
    bool ok = false;
    QString error;
    QStringList warnings;
    Rig fitted;                                         ///< same joints / hierarchy / skin; moved
    Warp warp;
    int controlPoints = 0;
    int verticesMatched = 0;
    double meanVertexShiftMm = 0, maxVertexShiftMm = 0;   ///< target vs source
    double meanJointShiftMm = 0, maxJointShiftMm = 0;
    double warpResidualMm = 0;                            ///< worst vertex the warp fails to reproduce
    std::vector<std::array<double, 3>> jointShift;        ///< world displacement per joint (rig units)
};

/// `target` holds one array per entry of rig.meshes (x,y,z per POSITION index). An
/// empty array means "not part of the field": that mesh simply follows the warp.
Result fit(const Rig& rig, const std::vector<std::vector<float>>& target, const Options& options = Options());

/// World position of every joint at the neutral pose (rig units).
std::vector<std::array<double, 3>> jointWorldPositions(const Rig& rig);

// ---- correspondence -----------------------------------------------------------

/// The entries of rig.meshes that the importer turns into submeshes (meshes with no
/// valid triangle are skipped), in submesh order.
std::vector<size_t> importedMeshIndices(const Rig& rig, bool flipV = true);

struct Correspondence {
    bool ok = false;
    QString error;
    std::vector<std::vector<float>> positions;   ///< per rig mesh, per POSITION (empty = not covered)
    double maxSeamSplitMm = 0;                   ///< how far duplicates of one position disagree
    bool matchedByUv = false;                    ///< true when UVs (not vertex order) set the correspondence
    double matchedFraction = 1.0;                ///< share of target vertices that found their source vertex
    double sourceCoverage = 1.0;                 ///< share of the character's vertices that received a target vertex
    int ambiguousVertices = 0;                   ///< target vertices whose UV had several candidates (nearest won)
    bool flippedV = false;                       ///< the target's V axis was inverted relative to the source
};

/// One submesh of the IMPORTED target, as the editor sees it.
struct ImportedSubmesh {
    std::vector<float> positions;   ///< x,y,z per vertex
    std::vector<float> uvs;         ///< u,v per vertex (may be empty)
};

/// Correspondence for an EDITED mesh coming back through the editor's importer.
/// The importer does NOT keep vertex order stable across an import/export round
/// trip (measured: the same file re-exported came back with 99.98 % of the indices
/// permuted), so order is the fallback, not the rule. UV0 survives sculpting and
/// identifies a surface point, so each target vertex is matched to the source
/// vertex with the same UV (V orientation is detected automatically); when a UV has
/// several candidates the nearest in 3D wins. Fails — rather than guesses — if the
/// UVs or the topology were changed (under ~99.5 % of vertices matched).
Correspondence matchImported(const Rig& rig, const std::vector<ImportedSubmesh>& submeshes, bool flipV = true,
                             double unitToMillimetres = 1000);

/// Collapse positions of the IMPORTED (layout-expanded) submeshes back to one value
/// per DNA position: duplicates created at UV/normal seams are averaged. `expanded`
/// has one array per imported submesh, in submesh order. Refuses a count mismatch
/// with an error naming the submesh.
Correspondence positionsFromExpanded(const Rig& rig, const std::vector<std::vector<float>>& expanded,
                                     bool flipV = true, double unitToMillimetres = 1000);

// ---- pose quality -------------------------------------------------------------

struct PoseSpec {
    QString name;
    /// joint name -> Euler degrees (xyz, local axes), applied on top of the neutral pose.
    QList<QPair<QString, std::array<double, 3>>> rotations;
};

/// Shoulder / elbow / hip / knee flexions on the body's driver joints.
QList<PoseSpec> referencePoses();

/// Linear-blend-skinned positions of every mesh in a pose (NO correctives), one array
/// per entry of rig.meshes. Names that do not exist are reported through `missing`.
std::vector<std::vector<float>> posedPositions(const Rig& rig, const PoseSpec& pose,
                                               QStringList* missing = nullptr);

struct PoseReport {
    QString name;
    bool joints = true;                 ///< false when a joint of the pose is missing
    double maxMovementMm = 0;           ///< how far the pose moves the fitted mesh
    double stretchMaxDelta = 0;         ///< worst |edge stretch (fitted) - edge stretch (original)|
    double stretchP99Delta = 0;
    double similarityMaxMm = -1;        ///< vs s * posed original (only for a uniform-scale edit)
    double similarityMeanMm = -1;
};

/// Compare how the ORIGINAL and the FITTED rig deform in each pose. Edge stretch (the
/// ratio of every triangle edge's posed length to its bind length) is the
/// edit-independent measure of "usable deformation": a fit that keeps skinning sane
/// reproduces the source rig's stretch. When `uniformScale` > 0 the edit is a known
/// similarity, so the fitted pose can also be checked against `uniformScale * posed
/// original` exactly.
std::vector<PoseReport> evaluatePoses(const Rig& original, const Rig& fitted, const QList<PoseSpec>& poses,
                                      double uniformScale = 0.0, const Options& options = Options());

// ---- LOD propagation ----------------------------------------------------------

struct LodPropagation {
    int vertices = 0;
    double meanGapMm = 0, maxGapMm = 0;   ///< distance of warped LODn vertices to the fitted LOD0 surface samples
    double baselineMeanGapMm = 0;         ///< the same gap BEFORE fitting (the surfaces' own distance)
};

/// Apply the warp to another LOD's meshes and measure how far the result sits from the
/// fitted LOD 0 vertices (nearest-vertex distance), against the unwarped baseline.
LodPropagation evaluateLodPropagation(const Rig& lod0Original, const Rig& lod0Fitted, const Rig& lodN,
                                      const Warp& warp, const Options& options = Options());

}  // namespace Fit
}  // namespace Dna

// ---- DNA serialisation ----------------------------------------------------------

namespace Dna {
namespace Fit {

struct WriteResult {
    bool ok = false;
    QString error;
    QStringList notes;
    int meshesWritten = 0;       ///< meshes whose positions were rewritten (every LOD)
    int lodMeshesWarped = 0;     ///< of those, meshes outside `original` moved by the warp
};

/// Write `fitted` (the result of fit()) as a new binary DNA based on `sourcePath`.
/// All other layers (behaviour, RBF, neural nets …) are copied untouched, so this is
/// VALID SERIALISATION of the fitted skeleton and bind geometry — NOT recalibration:
/// corrective data stays as authored for the original proportions. The file is
/// written in the source's own units and axes (the mapping from the importer's
/// frame is derived from the data, never assumed); meshes of lower LODs follow
/// `warp`. Normals are copied unchanged. `original` / `fitted` must come from read()
/// with the default NeutralSpace::ParentRelative (the file's own joint values). Needs
/// -DENABLE_OPENRIGLOGIC.
WriteResult writeFittedDna(const QString& sourcePath, const Rig& original, const Rig& fitted, const Warp& warp,
                           const QString& outPath);

}  // namespace Fit
}  // namespace Dna

#endif  // DNA_DNAFIT_H
