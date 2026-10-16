/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

// Skel slice E (#559): pose mirroring across a symmetry plane.
// Pure-data — no Ogre scene, no display, safe under Xvfb / headless CI.

#include <gtest/gtest.h>

#include "PoseMirror.h"

using BoneNaming::CustomRule;
using PoseMirror::BoneTransform;
using PoseMirror::MirrorOptions;
using PoseMirror::MirrorReport;
using PoseMirror::Plane;
using PoseMirror::Pose;

namespace {

constexpr float kEps = 1e-5f;

void expectVecNear(const Ogre::Vector3& a, const Ogre::Vector3& b)
{
    EXPECT_NEAR(a.x, b.x, kEps);
    EXPECT_NEAR(a.y, b.y, kEps);
    EXPECT_NEAR(a.z, b.z, kEps);
}

void expectQuatNear(const Ogre::Quaternion& a, const Ogre::Quaternion& b)
{
    // q and -q are the same orientation, so compare up to sign.
    const float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    const Ogre::Quaternion bb = (dot < 0) ? Ogre::Quaternion(-b.w, -b.x, -b.y, -b.z) : b;
    EXPECT_NEAR(a.w, bb.w, kEps);
    EXPECT_NEAR(a.x, bb.x, kEps);
    EXPECT_NEAR(a.y, bb.y, kEps);
    EXPECT_NEAR(a.z, bb.z, kEps);
}

BoneTransform makeT(const Ogre::Vector3& t,
                    const Ogre::Quaternion& r = Ogre::Quaternion::IDENTITY,
                    const Ogre::Vector3& s = Ogre::Vector3(1, 1, 1))
{
    BoneTransform b;
    b.translate = t;
    b.rotation = r;
    b.scale = s;
    return b;
}

// ---------------------------------------------------------------------------
// Plane ids
// ---------------------------------------------------------------------------
TEST(PoseMirror, PlaneIdRoundTrip)
{
    for (Plane p : {Plane::X, Plane::Y, Plane::Z}) {
        Plane back;
        ASSERT_TRUE(PoseMirror::planeFromId(PoseMirror::planeId(p), &back));
        EXPECT_EQ(back, p);
    }
}

TEST(PoseMirror, PlaneIdRejectsUnknown)
{
    // Must REJECT, not silently default: mirroring across the wrong plane
    // produces a plausible-looking but wrong pose.
    Plane p = Plane::Z;
    EXPECT_FALSE(PoseMirror::planeFromId("w", &p));
    EXPECT_FALSE(PoseMirror::planeFromId("", &p));
    EXPECT_FALSE(PoseMirror::planeFromId("xy", &p));
    EXPECT_EQ(p, Plane::Z) << "out-param must be untouched on failure";
}

TEST(PoseMirror, PlaneIdIsCaseAndSpaceTolerant)
{
    Plane p;
    ASSERT_TRUE(PoseMirror::planeFromId(" X ", &p));
    EXPECT_EQ(p, Plane::X);
}

// ---------------------------------------------------------------------------
// Reflection math
// ---------------------------------------------------------------------------
TEST(PoseMirror, PositionReflectsOnlyTheNormalComponent)
{
    const Ogre::Vector3 p(2, 3, 5);
    expectVecNear(PoseMirror::mirrorPosition(p, Plane::X), Ogre::Vector3(-2, 3, 5));
    expectVecNear(PoseMirror::mirrorPosition(p, Plane::Y), Ogre::Vector3(2, -3, 5));
    expectVecNear(PoseMirror::mirrorPosition(p, Plane::Z), Ogre::Vector3(2, 3, -5));
}

TEST(PoseMirror, RotationAboutThePlaneNormalIsPreserved)
{
    // A rotation about the mirror normal is unchanged by the reflection
    // (spinning about X looks the same reflected in the YZ plane).
    const Ogre::Quaternion rx(Ogre::Degree(40), Ogre::Vector3::UNIT_X);
    expectQuatNear(PoseMirror::mirrorRotation(rx, Plane::X), rx);

    const Ogre::Quaternion ry(Ogre::Degree(40), Ogre::Vector3::UNIT_Y);
    expectQuatNear(PoseMirror::mirrorRotation(ry, Plane::Y), ry);

    const Ogre::Quaternion rz(Ogre::Degree(40), Ogre::Vector3::UNIT_Z);
    expectQuatNear(PoseMirror::mirrorRotation(rz, Plane::Z), rz);
}

TEST(PoseMirror, RotationPerpendicularToTheNormalIsNegated)
{
    // A rotation about Y, mirrored in the YZ plane (normal X), reverses.
    const Ogre::Quaternion ry(Ogre::Degree(30), Ogre::Vector3::UNIT_Y);
    const Ogre::Quaternion expected(Ogre::Degree(-30), Ogre::Vector3::UNIT_Y);
    expectQuatNear(PoseMirror::mirrorRotation(ry, Plane::X), expected);
}

TEST(PoseMirror, RotationReflectionIsItsOwnInverse)
{
    // Mirroring an orientation twice must return it, for every plane.
    const Ogre::Quaternion q =
        Ogre::Quaternion(Ogre::Degree(25), Ogre::Vector3(0.3f, 0.7f, 0.2f).normalisedCopy());
    for (Plane p : {Plane::X, Plane::Y, Plane::Z})
        expectQuatNear(PoseMirror::mirrorRotation(PoseMirror::mirrorRotation(q, p), p), q);
}

TEST(PoseMirror, MirroredRotationStaysUnitLength)
{
    // A reflected rotation must still be a valid (unit) rotation — the whole
    // reason the conjugation is used instead of reflecting the matrix.
    const Ogre::Quaternion q =
        Ogre::Quaternion(Ogre::Degree(77), Ogre::Vector3(1, 2, 3).normalisedCopy());
    for (Plane p : {Plane::X, Plane::Y, Plane::Z}) {
        const Ogre::Quaternion m = PoseMirror::mirrorRotation(q, p);
        EXPECT_NEAR(m.Norm(), 1.0f, 1e-4f);
    }
}

TEST(PoseMirror, MirroredRotationReflectsVectorsCorrectly)
{
    // The defining property: mirroring an orientation and then applying it to
    // a mirrored vector must equal mirroring the originally-rotated vector.
    //   M(R) * M(v) == M(R * v)
    const Ogre::Quaternion R(Ogre::Degree(35), Ogre::Vector3(0.2f, 0.5f, 0.84f).normalisedCopy());
    const Ogre::Vector3 v(0.4f, -0.7f, 1.3f);
    for (Plane p : {Plane::X, Plane::Y, Plane::Z}) {
        const Ogre::Vector3 lhs = PoseMirror::mirrorRotation(R, p) * PoseMirror::mirrorPosition(v, p);
        const Ogre::Vector3 rhs = PoseMirror::mirrorPosition(R * v, p);
        expectVecNear(lhs, rhs);
    }
}

TEST(PoseMirror, ScaleIsUntouchedByDefault)
{
    MirrorOptions opts;
    const BoneTransform in = makeT({1, 2, 3}, Ogre::Quaternion::IDENTITY, {2, 3, 4});
    const BoneTransform out = PoseMirror::mirrorTransform(in, opts);
    expectVecNear(out.scale, Ogre::Vector3(2, 3, 4));
}

TEST(PoseMirror, LegacyNegateScaleIsOptIn)
{
    MirrorOptions opts;
    opts.negateScaleAlongNormal = true;
    const BoneTransform in = makeT({1, 2, 3}, Ogre::Quaternion::IDENTITY, {2, 3, 4});
    expectVecNear(PoseMirror::mirrorTransform(in, opts).scale, Ogre::Vector3(-2, 3, 4));
}

// ---------------------------------------------------------------------------
// Whole-pose mirroring
// ---------------------------------------------------------------------------
TEST(PoseMirror, PairedBonesSwapSides)
{
    // The acceptance criterion: a one-sided gesture flips sides. The left hand
    // is raised (+Y) and pushed out to +X; after mirroring the RIGHT hand must
    // carry that pose, at −X.
    BoneNaming::Map naming;
    Pose pose;
    pose.insert("LeftHand", makeT({1, 2, 0},
                                  Ogre::Quaternion(Ogre::Degree(30), Ogre::Vector3::UNIT_Y)));
    pose.insert("RightHand", makeT({-1, 0, 0}));

    MirrorReport rep;
    const Pose out = PoseMirror::mirror(pose, naming, {}, &rep);

    ASSERT_TRUE(out.contains("RightHand"));
    expectVecNear(out.value("RightHand").translate, Ogre::Vector3(-1, 2, 0));
    expectQuatNear(out.value("RightHand").rotation,
                   Ogre::Quaternion(Ogre::Degree(-30), Ogre::Vector3::UNIT_Y));
    // ...and the right hand's own (neutral) pose has moved to the left.
    ASSERT_TRUE(out.contains("LeftHand"));
    expectVecNear(out.value("LeftHand").translate, Ogre::Vector3(1, 0, 0));

    EXPECT_EQ(rep.pairedBonesMirrored, 2);
    EXPECT_TRUE(rep.collisions.isEmpty());
    EXPECT_TRUE(rep.unpairedSided.isEmpty());
}

TEST(PoseMirror, CentreBonesAreReflectedInPlace)
{
    // The issue: "bones with no detected mirror are reflected across the
    // mirror plane in-place".
    BoneNaming::Map naming;
    Pose pose;
    pose.insert("Spine", makeT({0.5f, 1, 0},
                               Ogre::Quaternion(Ogre::Degree(20), Ogre::Vector3::UNIT_Z)));
    MirrorReport rep;
    const Pose out = PoseMirror::mirror(pose, naming, {}, &rep);

    ASSERT_TRUE(out.contains("Spine"));
    expectVecNear(out.value("Spine").translate, Ogre::Vector3(-0.5f, 1, 0));
    expectQuatNear(out.value("Spine").rotation,
                   Ogre::Quaternion(Ogre::Degree(-20), Ogre::Vector3::UNIT_Z));
    EXPECT_EQ(rep.centerBonesMirrored, 1);
    EXPECT_EQ(rep.pairedBonesMirrored, 0);
}

TEST(PoseMirror, CentreBonesCanBeLeftAlone)
{
    BoneNaming::Map naming;
    Pose pose;
    pose.insert("Spine", makeT({0.5f, 1, 0}));
    MirrorOptions opts;
    opts.mirrorCenterBonesInPlace = false;
    MirrorReport rep;
    const Pose out = PoseMirror::mirror(pose, naming, opts, &rep);
    expectVecNear(out.value("Spine").translate, Ogre::Vector3(0.5f, 1, 0));
    EXPECT_EQ(rep.centerBonesMirrored, 0);
    EXPECT_EQ(rep.untouched, 1);
}

TEST(PoseMirror, MirroringTwiceIsIdentity)
{
    // The strongest guard on the whole op: mirror twice == no-op. This is what
    // fails if scale is negated, or if the quaternion conjugation is wrong.
    BoneNaming::Map naming;
    Pose pose;
    pose.insert("LeftArm", makeT({1, 2, 3},
                                 Ogre::Quaternion(Ogre::Degree(41), Ogre::Vector3::UNIT_Y),
                                 {1, 1, 1}));
    pose.insert("RightArm", makeT({-1, 0.5f, 1},
                                  Ogre::Quaternion(Ogre::Degree(-12), Ogre::Vector3::UNIT_Z)));
    pose.insert("Spine", makeT({0, 1, 0},
                               Ogre::Quaternion(Ogre::Degree(7), Ogre::Vector3::UNIT_X)));

    const Pose once = PoseMirror::mirror(pose, naming);
    const Pose twice = PoseMirror::mirror(once, naming);

    ASSERT_EQ(twice.size(), pose.size());
    for (auto it = pose.constBegin(); it != pose.constEnd(); ++it) {
        ASSERT_TRUE(twice.contains(it.key())) << it.key().toStdString();
        expectVecNear(twice.value(it.key()).translate, it.value().translate);
        expectQuatNear(twice.value(it.key()).rotation, it.value().rotation);
        expectVecNear(twice.value(it.key()).scale, it.value().scale);
    }
}

TEST(PoseMirror, SidedBoneWithNoPartnerInPoseIsKeptAndReported)
{
    // Applying a saved LEFT-arm-only pose mirrored: the left arm has no right
    // counterpart IN THE POSE, so it is reported rather than silently dropped.
    BoneNaming::Map naming;
    Pose pose;
    pose.insert("LeftArm", makeT({1, 2, 3}));
    MirrorReport rep;
    const Pose out = PoseMirror::mirror(pose, naming, {}, &rep);

    EXPECT_EQ(rep.unpairedSided, QStringList({"LeftArm"}));
    EXPECT_EQ(rep.pairedBonesMirrored, 0);
    // Value survives, unreflected.
    ASSERT_TRUE(out.contains("LeftArm"));
    expectVecNear(out.value("LeftArm").translate, Ogre::Vector3(1, 2, 3));
}

TEST(PoseMirror, CollisionsAreReportedNotSilentlyOverwritten)
{
    // Two bones mapping onto one destination: the pre-slice-E hash insert
    // dropped one silently. Here the first (sorted) wins and the loser is
    // named, so a caller can tell the user the mirror was lossy.
    CustomRule rule;
    rule.leftPattern      = "^(armA_l|armB_l)$";
    rule.leftReplacement  = "arm_r";
    rule.rightPattern     = "^arm_r$";
    rule.rightReplacement = "armA_l";
    BoneNaming::Map naming({rule});

    Pose pose;
    pose.insert("armA_l", makeT({1, 0, 0}));
    pose.insert("armB_l", makeT({2, 0, 0}));
    pose.insert("arm_r", makeT({-1, 0, 0}));

    MirrorReport rep;
    const Pose out = PoseMirror::mirror(pose, naming, {}, &rep);
    EXPECT_FALSE(rep.collisions.isEmpty());
    EXPECT_TRUE(out.contains("arm_r"));
}

TEST(PoseMirror, EmptyPoseIsSafe)
{
    BoneNaming::Map naming;
    MirrorReport rep;
    const Pose out = PoseMirror::mirror(Pose{}, naming, {}, &rep);
    EXPECT_TRUE(out.isEmpty());
    EXPECT_EQ(rep.pairedBonesMirrored, 0);
}

TEST(PoseMirror, MirrorsAcrossNonDefaultPlanes)
{
    BoneNaming::Map naming;
    Pose pose;
    pose.insert("Spine", makeT({1, 2, 3}));
    MirrorOptions opts;
    opts.plane = Plane::Z;
    const Pose out = PoseMirror::mirror(pose, naming, opts);
    expectVecNear(out.value("Spine").translate, Ogre::Vector3(1, 2, -3));
}

TEST(PoseMirror, ResultIsDeterministicAcrossRuns)
{
    // QHash iteration order is unspecified; `mirror` sorts internally so the
    // collision winner (and hence the output) can't vary between runs.
    CustomRule rule;
    rule.leftPattern      = "^(a_l|b_l)$";
    rule.leftReplacement  = "c_r";
    rule.rightPattern     = "^c_r$";
    rule.rightReplacement = "a_l";
    BoneNaming::Map naming({rule});
    Pose pose;
    pose.insert("a_l", makeT({1, 0, 0}));
    pose.insert("b_l", makeT({2, 0, 0}));
    pose.insert("c_r", makeT({3, 0, 0}));

    MirrorReport r1, r2;
    const Pose o1 = PoseMirror::mirror(pose, naming, {}, &r1);
    const Pose o2 = PoseMirror::mirror(pose, naming, {}, &r2);
    EXPECT_EQ(r1.collisions, r2.collisions);
    expectVecNear(o1.value("c_r").translate, o2.value("c_r").translate);
}

// ---------------------------------------------------------------------------
// Mirror selection
// ---------------------------------------------------------------------------
TEST(PoseMirror, MirrorSelectionFlipsNames)
{
    // The issue: select `Hand_l` → mirror-selection → `Hand_r` is selected.
    BoneNaming::Map naming;
    int matched = 0;
    const QStringList out = PoseMirror::mirrorSelection({"Hand_l"}, naming, &matched);
    EXPECT_EQ(out, QStringList({"Hand_r"}));
    EXPECT_EQ(matched, 1);
}

TEST(PoseMirror, MirrorSelectionDropsCentreBones)
{
    // A centre bone's "mirror" is itself; returning it would read as the op
    // having done nothing, so it is dropped and not counted as matched.
    BoneNaming::Map naming;
    int matched = 0;
    const QStringList out = PoseMirror::mirrorSelection({"Spine", "Hips"}, naming, &matched);
    EXPECT_TRUE(out.isEmpty());
    EXPECT_EQ(matched, 0);
}

TEST(PoseMirror, MirrorSelectionDeduplicates)
{
    BoneNaming::Map naming;
    // Both sides selected → both flip onto each other; the result must not
    // list a bone twice.
    const QStringList out = PoseMirror::mirrorSelection({"Hand_l", "Hand_r"}, naming);
    EXPECT_EQ(out.size(), 2);
    EXPECT_TRUE(out.contains("Hand_l"));
    EXPECT_TRUE(out.contains("Hand_r"));
}

TEST(PoseMirror, MirrorSelectionHandlesMixedSelection)
{
    BoneNaming::Map naming;
    int matched = 0;
    const QStringList out =
        PoseMirror::mirrorSelection({"LeftArm", "Spine", "RightLeg"}, naming, &matched);
    EXPECT_EQ(matched, 2);
    EXPECT_EQ(out, QStringList({"RightArm", "LeftLeg"}));
}

} // namespace
