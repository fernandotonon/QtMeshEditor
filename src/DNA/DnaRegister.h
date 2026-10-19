#ifndef DNA_DNAREGISTER_H
#define DNA_DNAREGISTER_H

// Registration of a MetaHuman body onto an ARBITRARY character surface (epic #1095,
// Phase 4 follow-up). `Fit::fit` needs one target position per source vertex; a
// character with its own topology does not provide that, so this builds it: the
// body is pre-aligned (uniform scale by height, feet and centre matched), then
// non-rigidly deformed onto the target surface with the same Amberg optimal-step
// NRICP the face rig uses. The deformed positions ARE the correspondence, and go
// straight into Fit::fit, which moves the skeleton by the resulting field.
//
// Honest limits (measured, see docs/DNA_FITTING.md): a bare body draped over a
// clothed or stylised character follows the clothing silhouette, and a pose
// difference (A-pose body, T-pose target) is only partly absorbed by the stiffness
// schedule — optional landmarks (joint positions) can anchor the limbs.
//
// Pure data and Ogre-free.

#include "DnaFit.h"

#include <QString>
#include <QStringList>

#include <array>
#include <vector>

namespace Dna {
namespace Fit {

struct RegisterOptions {
    /// Smooth (default): coarse-to-fine ICP whose deformation is a GLOBAL smooth RBF warp that gains
    /// control points level by level — few degrees of freedom early, so correspondences cannot slide
    /// along the surface. Nricp: the per-vertex-affine Amberg solver the face rig uses (kept for
    /// comparison; it lands on the surface but drifts tangentially).
    enum class Method { Smooth, Nricp };
    Method method = Method::Smooth;
    /// Smooth method: ICP rounds per resolution level (7 levels, 24 -> 1500 control points). Measured on a
    /// re-meshed, edited MetaHuman body: 1 round = 8.7 mm mean joint error, 3 = 7.5, 6 = 6.1, 10 = 5.2.
    int rounds = 8;
    bool alignHeight = true;          ///< uniform scale by Y extent + match feet / centre before the fit
    std::vector<double> stiffness;    ///< NRICP schedule (empty = the library default)
    /// Optional anchors: body joint name -> position in the TARGET's frame. The vertex
    /// of the aligned body nearest that joint is pinned there.
    std::vector<std::pair<QString, std::array<float, 3>>> jointAnchors;
};

struct RegisterResult {
    bool ok = false;
    QString error;
    QStringList warnings;
    /// One entry per rig mesh, per POSITION — the body deformed onto the target
    /// (in the TARGET's frame and units). Empty for meshes that are not imported.
    std::vector<std::vector<float>> positions;
    double alignScale = 1.0;                   ///< uniform scale applied by the pre-align
    double meanResidualPct = 0, maxResidualPct = 0;   ///< distance to the target surface, % of its diagonal
    double p95ResidualPct = 0;
    int templateVertices = 0;
};

// ---- skeleton-anchored registration ----------------------------------------------
// Surface registration (above) drapes the body over the target's silhouette; on a
// stylised character with different proportions that collapses the body into the
// outline of its clothes. When the target carries its own skeleton the sturdier
// route is to match JOINTS: a handful of bone-to-bone correspondences define a
// smooth warp (affine + RBF) that carries the whole body and the MetaHuman
// skeleton onto the target's proportions, with no surface drape at all.

struct JointAnchor {
    QString bodyJoint;                 ///< joint name in the DNA rig
    std::array<double, 3> target{};    ///< where the target character has that joint
};

/// The target-side bone names that stand for each MetaHuman body joint, covering the
/// Mixamo / generic-biped conventions. Returns pairs (bodyJoint, candidate names).
std::vector<std::pair<QString, QStringList>> defaultJointNameMap();

/// Pair each MetaHuman joint with the first matching target bone. `targetBones` maps a
/// bone name to its position in the target's frame. Unmatched joints are listed in
/// `unmatched`.
std::vector<JointAnchor> matchJointAnchors(const std::vector<std::pair<QString, std::array<double, 3>>>& targetBones,
                                           QStringList* unmatched = nullptr);

struct JointRegisterResult {
    bool ok = false;
    QString error;
    QStringList warnings;
    std::vector<std::vector<float>> positions;   ///< per rig mesh, per POSITION, warped onto the target proportions
    Rig alignedRig;                              ///< the rig after limb pose alignment (== input when none ran) — pass THIS to Fit::fit
    double yawDegrees = 0;                       ///< turn applied so the body faces the target's way (0 = none)
    int limbChainsAligned = 0;
    QStringList limbAngles;                      ///< rotation applied per chain
    int anchorsUsed = 0;
    double anchorResidualMm = 0;                 ///< worst anchor the warp fails to reproduce (rig units * 1000)
};

/// Needs at least 6 anchors that are not coplanar. The warp is built from the anchors'
/// neutral world positions to their targets; every mesh position follows it.
JointRegisterResult registerByJoints(const Rig& rig, const std::vector<JointAnchor>& anchors, bool alignLimbPose = true);

/// `targetV` / `targetF`: the target surface in the rig's axis convention (+Y up),
/// x,y,z per vertex and triangle indices.
RegisterResult registerToSurface(const Rig& rig, const std::vector<float>& targetV, const std::vector<int>& targetF,
                                 const RegisterOptions& options = RegisterOptions());

}  // namespace Fit
}  // namespace Dna

#endif
