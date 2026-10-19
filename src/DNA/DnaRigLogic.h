#ifndef DNA_DNARIGLOGIC_H
#define DNA_DNARIGLOGIC_H

// RigLogic evaluation of a MetaHuman DNA (epic #1095, Phase 2).
//
// Pure data, Ogre-free: a driver pose goes in (the rotation of each driver
// joint relative to its neutral pose), and the evaluated absolute LOCAL
// transforms of every rig-driven joint come out. For a MetaHuman body those
// driven joints are the corrective joints (the `_in_/_out_/_fwd_/_bck_`,
// bulge and twist joints) that RBF solvers blend in as the limbs bend.
//
// Conventions, all verified on Epic's SKM_Body.dna:
//  * a raw control named "<joint>.qx|qy|qz|qw" is that joint's rotation
//    quaternion RELATIVE TO ITS NEUTRAL pose, about its own local axes
//    (identity at rest; RBF targets are e.g. calf_l z = -0.707, w = 0.707);
//  * RigLogic joint outputs are DELTAS on the neutral pose, laid out
//    joint*9 + attribute (tx,ty,tz, rx,ry,rz, sx,sy,sz), translations in the
//    DNA's units (cm) and rotations as Euler degrees in the descriptor's
//    sequence — so the final rotation is eulerToQuaternion(neutral + delta),
//    not a quaternion product.
//
// Like DnaReader, every library symbol sits behind ENABLE_OPENRIGLOGIC; a
// build without it returns nullptr from create() with the rebuild message.

#include "DnaReader.h"

#include <QString>
#include <QStringList>

#include <array>
#include <memory>
#include <vector>

namespace Dna {

/// Absolute parent-relative transform of one joint after evaluation.
struct JointPose {
    int joint = -1;                          ///< index in the DNA joint table
    std::array<double, 3> translation{};     ///< in the requested units
    std::array<double, 4> orientation{1, 0, 0, 0};  ///< (w,x,y,z)
    std::array<double, 3> scale{1, 1, 1};
};

class RigEvaluator {
public:
    virtual ~RigEvaluator();
    RigEvaluator(const RigEvaluator&) = delete;
    RigEvaluator& operator=(const RigEvaluator&) = delete;

    /// Load the behaviour layers of `path` and build a RigLogic instance.
    /// Honours options.units / convertCoordinateSystem / lod (as read()). Returns
    /// nullptr with `*error` set (actionable) on failure.
    static std::unique_ptr<RigEvaluator> create(const QString& path,
                                                const ReadOptions& options = ReadOptions(),
                                                QString* error = nullptr);

    virtual int jointCount() const;
    virtual QString jointName(int joint) const;
    int jointIndex(const QString& name) const;          ///< -1 if absent

    virtual int rawControlCount() const;
    virtual QString rawControlName(int index) const;
    virtual int rawControlIndex(const QString& name) const;     ///< -1 if absent

    /// Joints that feed rotation quaternions into the rig (raw controls named
    /// "<joint>.qx"). For a body: the ~44 driver joints.
    virtual QStringList driverJoints() const;
    /// Joints the rig can move (any output attribute) — the corrective joints.
    virtual const std::vector<int>& drivenJoints() const;

    virtual int rbfSolverCount() const;
    virtual int rbfPoseCount() const;

    virtual void resetControls();
    virtual void setRawControl(int index, float value);
    /// Feed a driver joint's rotation relative to neutral, quaternion (w,x,y,z).
    /// Returns false when `joint` is not a driver. The sign is normalised to
    /// w >= 0 (the RBF targets are authored in that hemisphere).
    virtual bool setDriverRotation(const QString& joint, const std::array<double, 4>& wxyz);

    /// Evaluate. Returns the elapsed time in milliseconds.
    virtual double calculate();

    /// Absolute local transform of every driven joint for the current controls.
    virtual std::vector<JointPose> drivenPoses() const;
    /// Absolute local transform of one joint (neutral + delta).
    virtual JointPose jointPose(int joint) const;

    /// Raw joint output delta (9 floats per joint), as RigLogic produced them.
    std::vector<float> jointDeltas() const;

protected:
    /// Protected so a test can substitute a fake rig (the scene-level corrective
    /// manager tests do, which keeps them independent of a MetaHuman asset).
    RigEvaluator();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace Dna

#endif  // DNA_DNARIGLOGIC_H
