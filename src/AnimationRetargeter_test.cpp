// Headless tests for the #523 retarget core — pure data (no Ogre::Root, no GL).

#include <gtest/gtest.h>

#include "AnimationRetargeter.h"

#include <QDir>
#include <QTemporaryDir>

#include <OgreMatrix3.h>

#include <cmath>
#include <map>
#include <random>

using Retarget::BoneMap;
using Retarget::LocalPose;
using Retarget::RigDesc;

namespace {

// Bone layout shared by the test rigs: name suffix, parent index, rest
// WORLD position of a 1.8-unit-tall Y-up humanoid in a T-pose, facing +Z,
// character-left at +X.
struct Def { const char* mixamo; const char* unreal; int parent; float x, y, z; };
const Def kDefs[] = {
    {"Hips",          "pelvis",     -1,  0.00f, 1.00f, 0.0f},
    {"Spine",         "spine_01",    0,  0.00f, 1.15f, 0.0f},
    {"Spine1",        "spine_02",    1,  0.00f, 1.30f, 0.0f},
    {"Neck",          "neck_01",     2,  0.00f, 1.55f, 0.0f},
    {"Head",          "head",        3,  0.00f, 1.65f, 0.0f},
    {"LeftShoulder",  "clavicle_l",  2,  0.08f, 1.50f, 0.0f},
    {"LeftArm",       "upperarm_l",  5,  0.20f, 1.50f, 0.0f},
    {"LeftForeArm",   "lowerarm_l",  6,  0.48f, 1.50f, 0.0f},
    {"LeftHand",      "hand_l",      7,  0.74f, 1.50f, 0.0f},
    {"RightShoulder", "clavicle_r",  2, -0.08f, 1.50f, 0.0f},
    {"RightArm",      "upperarm_r",  9, -0.20f, 1.50f, 0.0f},
    {"RightForeArm",  "lowerarm_r", 10, -0.48f, 1.50f, 0.0f},
    {"RightHand",     "hand_r",     11, -0.74f, 1.50f, 0.0f},
    {"LeftUpLeg",     "thigh_l",     0,  0.10f, 0.95f, 0.0f},
    {"LeftLeg",       "calf_l",     13,  0.10f, 0.52f, 0.0f},
    {"LeftFoot",      "foot_l",     14,  0.10f, 0.08f, 0.0f},
    {"RightUpLeg",    "thigh_r",     0, -0.10f, 0.95f, 0.0f},
    {"RightLeg",      "calf_r",     16, -0.10f, 0.52f, 0.0f},
    {"RightFoot",     "foot_r",     17, -0.10f, 0.08f, 0.0f},
};
constexpr int kN = int(sizeof(kDefs) / sizeof(kDefs[0]));

// Build a rig from WORLD rest positions + WORLD rest rotations.
RigDesc rigFromWorld(const std::vector<std::string>& names,
                     const std::vector<Ogre::Vector3>& wp,
                     const std::vector<Ogre::Quaternion>& wr)
{
    RigDesc r;
    for (int i = 0; i < kN; ++i) {
        const int p = kDefs[i].parent;
        r.names.push_back(names[size_t(i)]);
        r.parent.push_back(p);
        const Ogre::Quaternion pr = p >= 0 ? wr[size_t(p)] : Ogre::Quaternion::IDENTITY;
        const Ogre::Vector3 pp = p >= 0 ? wp[size_t(p)] : Ogre::Vector3::ZERO;
        r.bindRot.push_back(pr.Inverse() * wr[size_t(i)]);
        r.bindPos.push_back(pr.Inverse() * (wp[size_t(i)] - pp));
    }
    return r;
}

// Source: Mixamo names, Y-up, identity bone frames.
RigDesc mixamoRig(const char* ns = "mixamorig:")
{
    std::vector<std::string> n;
    std::vector<Ogre::Vector3> wp;
    std::vector<Ogre::Quaternion> wr;
    for (const auto& d : kDefs) {
        n.push_back(std::string(ns) + d.mixamo);
        wp.emplace_back(d.x, d.y, d.z);
        wr.push_back(Ogre::Quaternion::IDENTITY);
    }
    return rigFromWorld(n, wp, wr);
}

// Target: Unreal names, Z-UP (whole rig rotated −90° about X), 1.4× larger,
// and every bone with its own arbitrary local axes — i.e. nothing about the
// two skeletons lines up except their anatomy.
RigDesc unrealRig(Ogre::Quaternion* outUp = nullptr)
{
    const Ogre::Quaternion up(Ogre::Degree(-90), Ogre::Vector3::UNIT_X);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    std::vector<std::string> n;
    std::vector<Ogre::Vector3> wp;
    std::vector<Ogre::Quaternion> wr;
    for (const auto& d : kDefs) {
        n.push_back(d.unreal);
        wp.push_back(up * (Ogre::Vector3(d.x, d.y, d.z) * 1.4f));
        Ogre::Quaternion q(u(rng), u(rng), u(rng), u(rng));
        q.normalise();
        wr.push_back(q);
    }
    if (outUp) *outUp = up;
    return rigFromWorld(n, wp, wr);
}

float angleBetween(Ogre::Vector3 a, Ogre::Vector3 b)
{
    a.normalise(); b.normalise();
    return std::acos(std::clamp(a.dotProduct(b), -1.0f, 1.0f));
}

std::vector<std::string> namesOf(const RigDesc& r) { return r.names; }

// A short clip on the source: arm swings down, elbow bends, spine leans,
// hips travel forward and bob.
std::vector<LocalPose> sourceClip(const RigDesc& src, int frames)
{
    std::vector<LocalPose> out;
    for (int f = 0; f < frames; ++f) {
        const float t = float(f) / float(frames - 1);
        LocalPose lp = Retarget::bindPose(src);
        auto at = [&](const char* suffix) { return src.indexOf(std::string("mixamorig:") + suffix); };
        lp.rot[size_t(at("LeftArm"))] = Ogre::Quaternion(Ogre::Degree(-60.0f * t), Ogre::Vector3::UNIT_Z);
        lp.rot[size_t(at("LeftForeArm"))] = Ogre::Quaternion(Ogre::Degree(45.0f * t), Ogre::Vector3::UNIT_Y);
        lp.rot[size_t(at("Spine"))] = Ogre::Quaternion(Ogre::Degree(20.0f * t), Ogre::Vector3::UNIT_X);
        lp.rot[size_t(at("RightUpLeg"))] = Ogre::Quaternion(Ogre::Degree(-30.0f * t), Ogre::Vector3::UNIT_X);
        lp.pos[size_t(at("Hips"))] = src.bindPos[size_t(at("Hips"))]
                                     + Ogre::Vector3(0.0f, 0.05f * std::sin(6.28f * t), 0.4f * t);
        out.push_back(lp);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Auto-mapping
// ---------------------------------------------------------------------------

TEST(AnimationRetargeter, AutoMapsMixamoOntoUnrealWithoutManualEdits)
{
    const RigDesc src = mixamoRig();
    const RigDesc tgt = unrealRig();
    const auto rep = Retarget::autoMap(src.names, tgt.names);
    // every body bone of the fixture must map, and map to the RIGHT bone
    std::map<std::string, std::string> want;
    for (const auto& d : kDefs) want[std::string("mixamorig:") + d.mixamo] = d.unreal;
    EXPECT_EQ(int(rep.map.pairs.size()), kN) << "unmapped source bones: " << rep.unmappedSource.size();
    for (const auto& p : rep.map.pairs)
        EXPECT_EQ(p.target, want[p.source]) << p.source;
    EXPECT_TRUE(rep.unmappedTarget.empty());
}

TEST(AnimationRetargeter, AutoMapStripsNamespacesForIdenticalRigs)
{
    const RigDesc a = mixamoRig("mixamorig:");
    const RigDesc b = mixamoRig("mixamorig1:");
    const auto rep = Retarget::autoMap(a.names, b.names);
    EXPECT_EQ(rep.byExactName, kN);
    for (const auto& p : rep.map.pairs)
        EXPECT_EQ(p.source.substr(std::string("mixamorig:").size()),
                  p.target.substr(std::string("mixamorig1:").size()));
}

TEST(AnimationRetargeter, AutoMapFuzzyHandlesSynonymsAndSides)
{
    const std::vector<std::string> src = {"Bip01 L Thigh", "Bip01 L Calf", "Bip01 R UpperArm", "Bip01 Pelvis"};
    const std::vector<std::string> tgt = {"upperleg.L", "lowerleg.L", "upper_arm.R", "hips"};
    const auto rep = Retarget::autoMap(src, tgt);
    EXPECT_EQ(rep.map.targetFor("Bip01 L Thigh"), "upperleg.L");
    EXPECT_EQ(rep.map.targetFor("Bip01 L Calf"), "lowerleg.L");
    EXPECT_EQ(rep.map.targetFor("Bip01 R UpperArm"), "upper_arm.R");
    EXPECT_EQ(rep.map.targetFor("Bip01 Pelvis"), "hips");
}

TEST(AnimationRetargeter, AutoMapNeverReusesATargetBone)
{
    const std::vector<std::string> src = {"LeftHand", "lefthand", "Left_Hand"};
    const std::vector<std::string> tgt = {"hand_l"};
    const auto rep = Retarget::autoMap(src, tgt);
    EXPECT_EQ(rep.map.pairs.size(), 1u);
    EXPECT_EQ(rep.unmappedSource.size(), 2u);
}

// ---------------------------------------------------------------------------
// Bone maps
// ---------------------------------------------------------------------------

TEST(AnimationRetargeter, BoneMapJsonRoundTripAndValidation)
{
    BoneMap m;
    m.name = QStringLiteral("test");
    m.setPair("A", "x");
    m.setPair("B", "y");
    m.setPair("C", "x");          // steals "x" from A — stays one-to-one
    EXPECT_EQ(m.targetFor("A"), "");
    EXPECT_EQ(m.targetFor("C"), "x");

    QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath("t.bonemap");
    QString err;
    ASSERT_TRUE(m.save(path, &err)) << err.toStdString();
    BoneMap back;
    ASSERT_TRUE(BoneMap::load(path, &back, &err)) << err.toStdString();
    EXPECT_EQ(back.name, QStringLiteral("test"));
    EXPECT_EQ(back.pairs.size(), 2u);
    EXPECT_EQ(back.targetFor("B"), "y");

    EXPECT_FALSE(BoneMap::fromJson("{\"schema\":\"other\",\"pairs\":[]}", &back, &err));
    EXPECT_FALSE(BoneMap::fromJson(
        "{\"schema\":\"qtmesh-bonemap-v1\",\"pairs\":[{\"source\":\"a\",\"target\":\"x\"},"
        "{\"source\":\"b\",\"target\":\"x\"}]}", &back, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("twice")));
}

TEST(AnimationRetargeter, BundledMapsResolveAgainstOtherNamespaceSpellings)
{
    EXPECT_EQ(Retarget::bundledBoneMapNames().size(), 3);
    BoneMap unity;
    ASSERT_TRUE(Retarget::bundledBoneMap(QStringLiteral("mixamo_to_unity"), &unity));
    EXPECT_EQ(unity.targetFor("mixamorig:LeftForeArm"), "LeftForeArm");
    BoneMap unreal;
    ASSERT_TRUE(Retarget::bundledBoneMap(QStringLiteral("mixamo_to_unreal"), &unreal));
    EXPECT_EQ(unreal.targetFor("mixamorig:LeftForeArm"), "lowerarm_l");
    EXPECT_EQ(unreal.targetFor("mixamorig:RightHandIndex2"), "index_02_r");
    EXPECT_FALSE(Retarget::bundledBoneMap(QStringLiteral("nope"), nullptr));

    // bundled map is written for `mixamorig:`; a `mixamorig1:` rig resolves
    const RigDesc src = mixamoRig("mixamorig1:");
    const RigDesc tgt = unrealRig();
    QStringList unresolved;
    const BoneMap r = Retarget::resolveMap(unreal, src.names, tgt.names, &unresolved);
    EXPECT_EQ(r.targetFor("mixamorig1:LeftArm"), "upperarm_l");
    EXPECT_EQ(int(r.pairs.size()), kN);
    EXPECT_FALSE(unresolved.isEmpty()) << "fingers/toes absent from the fixture must be listed";
}

// ---------------------------------------------------------------------------
// Retargeting
// ---------------------------------------------------------------------------

// The acceptance case: a clip on a Mixamo rig onto a differently named,
// Z-up, 1.4× larger rig whose bones all have different local axes. Every
// mapped bone must POINT where the source bone points (in the target's
// frame), every frame, and the hips must travel the scaled distance.
TEST(AnimationRetargeter, RetargetsAcrossIncompatibleSkeletons)
{
    const RigDesc src = mixamoRig();
    Ogre::Quaternion up;
    const RigDesc tgt = unrealRig(&up);
    BoneMap map;
    ASSERT_TRUE(Retarget::bundledBoneMap(QStringLiteral("mixamo_to_unreal"), &map));
    map = Retarget::resolveMap(map, src.names, tgt.names);

    const auto frames = sourceClip(src, 11);
    Retarget::RetargetReport rep;
    const auto out = Retarget::retargetFrames(src, frames, tgt, map, {}, &rep);
    ASSERT_EQ(out.size(), frames.size()) << rep.error.toStdString();
    EXPECT_TRUE(rep.globalAlignment);
    EXPECT_NEAR(rep.heightScale, 1.4f, 1e-3f);
    EXPECT_EQ(rep.rootTarget, "pelvis");

    std::vector<Ogre::Quaternion> sw, tw;
    std::vector<Ogre::Vector3> sp, tp, sp0, tp0;
    {
        std::vector<Ogre::Quaternion> q;
        Retarget::forwardKinematics(src, Retarget::bindPose(src), q, sp0);
        Retarget::forwardKinematics(tgt, Retarget::bindPose(tgt), q, tp0);
    }
    float worst = 0.0f;
    for (size_t f = 0; f < frames.size(); ++f) {
        Retarget::forwardKinematics(src, frames[f], sw, sp);
        Retarget::forwardKinematics(tgt, out[f], tw, tp);
        for (int i = 0; i < kN; ++i) {
            const int p = kDefs[i].parent;
            if (p < 0) continue;
            const Ogre::Vector3 ds = up * (sp[size_t(i)] - sp[size_t(p)]);
            const Ogre::Vector3 dt = tp[size_t(i)] - tp[size_t(p)];
            worst = std::max(worst, angleBetween(ds, dt));
        }
        // hips: scaled world displacement, in the target frame
        const Ogre::Vector3 dS = up * (sp[0] - sp0[0]) * 1.4f;
        const Ogre::Vector3 dT = tp[0] - tp0[0];
        EXPECT_LT((dS - dT).length(), 1e-4f) << "frame " << f;
    }
    EXPECT_LT(worst, 1e-3f) << "a retargeted bone points the wrong way (" << worst << " rad)";
    // the clip actually moved something
    Retarget::forwardKinematics(tgt, out.back(), tw, tp);
    const int la = tgt.indexOf("upperarm_l"), lf = tgt.indexOf("lowerarm_l");
    EXPECT_GT(angleBetween(tp[size_t(lf)] - tp[size_t(la)], tp0[size_t(lf)] - tp0[size_t(la)]), 0.5f);
}

// Roll survives too: with matching rest directions the full world rotation
// delta (not just the bone direction) is reproduced on every mapped bone.
TEST(AnimationRetargeter, ReproducesTheFullRotationDelta)
{
    const RigDesc src = mixamoRig();
    Ogre::Quaternion up;
    const RigDesc tgt = unrealRig(&up);
    const BoneMap map = Retarget::autoMap(src.names, tgt.names).map;
    const auto frames = sourceClip(src, 5);
    const auto out = Retarget::retargetFrames(src, frames, tgt, map, {});
    ASSERT_EQ(out.size(), frames.size());

    std::vector<Ogre::Quaternion> sr0, tr0, sw, tw;
    std::vector<Ogre::Vector3> p;
    Retarget::forwardKinematics(src, Retarget::bindPose(src), sr0, p);
    Retarget::forwardKinematics(tgt, Retarget::bindPose(tgt), tr0, p);
    Retarget::forwardKinematics(src, frames.back(), sw, p);
    Retarget::forwardKinematics(tgt, out.back(), tw, p);
    for (int i = 0; i < kN; ++i) {
        const Ogre::Quaternion ds = up * (sw[size_t(i)] * sr0[size_t(i)].Inverse()) * up.Inverse();
        const Ogre::Quaternion dt = tw[size_t(i)] * tr0[size_t(i)].Inverse();
        EXPECT_GT(std::fabs(ds.Dot(dt)), 1.0f - 1e-4f) << kDefs[i].unreal;
    }
}

// A-pose source onto a T-pose target: with direction alignment the target's
// arms adopt the source's 45°-down rest (otherwise they would stay level).
TEST(AnimationRetargeter, AlignsRestDirectionsBetweenAPoseAndTPose)
{
    std::vector<std::string> n;
    std::vector<Ogre::Vector3> wp;
    std::vector<Ogre::Quaternion> wr;
    for (const auto& d : kDefs) {
        n.push_back(std::string("mixamorig:") + d.mixamo);
        Ogre::Vector3 v(d.x, d.y, d.z);
        if (std::string(d.mixamo).find("Arm") != std::string::npos
            || std::string(d.mixamo).find("Hand") != std::string::npos) {
            const float side = d.x > 0 ? 1.0f : -1.0f;
            const float r = std::fabs(d.x) - 0.08f;   // from the shoulder
            v = Ogre::Vector3(side * (0.08f + r * 0.7071f), 1.5f - r * 0.7071f, 0.0f);
        }
        wp.push_back(v);
        wr.push_back(Ogre::Quaternion::IDENTITY);
    }
    const RigDesc aPose = rigFromWorld(n, wp, wr);
    const RigDesc tPose = unrealRig();
    const BoneMap map = Retarget::autoMap(aPose.names, tPose.names).map;
    const std::vector<LocalPose> still = {Retarget::bindPose(aPose), Retarget::bindPose(aPose)};

    Retarget::Options aligned;
    const auto out = Retarget::retargetFrames(aPose, still, tPose, map, aligned);
    ASSERT_FALSE(out.empty());
    std::vector<Ogre::Quaternion> q;
    std::vector<Ogre::Vector3> tp;
    Retarget::forwardKinematics(tPose, out[0], q, tp);
    const int ua = tPose.indexOf("upperarm_l"), la = tPose.indexOf("lowerarm_l");
    const int pel = tPose.indexOf("pelvis"), hd = tPose.indexOf("head");
    Ogre::Vector3 upAxis = tp[size_t(hd)] - tp[size_t(pel)];
    upAxis.normalise();
    Ogre::Vector3 arm = tp[size_t(la)] - tp[size_t(ua)];
    arm.normalise();
    EXPECT_NEAR(arm.dotProduct(upAxis), -0.7071f, 1e-3f) << "arm should hang 45° like the source rest";

    Retarget::Options raw;
    raw.alignDirections = false;
    const auto out2 = Retarget::retargetFrames(aPose, still, tPose, map, raw);
    Retarget::forwardKinematics(tPose, out2[0], q, tp);
    arm = tp[size_t(la)] - tp[size_t(ua)];
    arm.normalise();
    EXPECT_NEAR(arm.dotProduct(upAxis), 0.0f, 1e-3f) << "without alignment the target keeps its T-pose";
}

TEST(AnimationRetargeter, TranslationModes)
{
    const RigDesc src = mixamoRig();
    const RigDesc tgt = unrealRig();
    const BoneMap map = Retarget::autoMap(src.names, tgt.names).map;
    const auto frames = sourceClip(src, 4);

    Retarget::Options none;
    none.translation = Retarget::TranslationMode::None;
    const auto a = Retarget::retargetFrames(src, frames, tgt, map, none);
    ASSERT_FALSE(a.empty());
    for (int i = 0; i < kN; ++i)
        EXPECT_LT((a.back().pos[size_t(i)] - tgt.bindPos[size_t(i)]).length(), 1e-6f)
            << "rotation-only must keep the target's bone lengths exactly";

    const auto b = Retarget::retargetFrames(src, frames, tgt, map, {});
    EXPECT_GT((b.back().pos[0] - tgt.bindPos[0]).length(), 0.1f) << "root mode moves the pelvis";
    for (int i = 1; i < kN; ++i)
        EXPECT_LT((b.back().pos[size_t(i)] - tgt.bindPos[size_t(i)]).length(), 1e-6f);
}

TEST(AnimationRetargeter, FirstFrameRestTreatsFrameZeroAsRest)
{
    const RigDesc src = mixamoRig();
    const RigDesc tgt = unrealRig();
    const BoneMap map = Retarget::autoMap(src.names, tgt.names).map;
    // A clip whose every frame carries the same constant spine lean (the
    // "armature offset" case): measured from frame 0 it has no motion.
    auto frames = sourceClip(src, 3);
    for (auto& f : frames) f = frames.front();
    Retarget::Options o;
    o.sourceRest = Retarget::SourceRest::FirstFrame;
    o.translation = Retarget::TranslationMode::None;
    o.alignDirections = false;
    const auto out = Retarget::retargetFrames(src, frames, tgt, map, o);
    ASSERT_FALSE(out.empty());
    for (int i = 0; i < kN; ++i)
        EXPECT_GT(std::fabs(out.back().rot[size_t(i)].Dot(tgt.bindRot[size_t(i)])), 1.0f - 1e-5f);
}

TEST(AnimationRetargeter, RejectsBadInput)
{
    const RigDesc src = mixamoRig();
    const RigDesc tgt = unrealRig();
    Retarget::RetargetReport rep;
    EXPECT_TRUE(Retarget::retargetFrames(src, {}, tgt, {}, {}, &rep).empty());
    EXPECT_FALSE(rep.error.isEmpty());
    BoneMap none;
    none.setPair("nope", "nada");
    EXPECT_TRUE(Retarget::retargetFrames(src, sourceClip(src, 2), tgt, none, {}, &rep).empty());
    EXPECT_TRUE(rep.error.contains(QStringLiteral("no bone pair")));
}

TEST(AnimationRetargeter, OptionIds)
{
    Retarget::TranslationMode m;
    EXPECT_TRUE(Retarget::translationModeFromId(QStringLiteral("ALL"), &m));
    EXPECT_EQ(m, Retarget::TranslationMode::All);
    EXPECT_TRUE(Retarget::translationModeFromId(QStringLiteral("rotation-only"), &m));
    EXPECT_EQ(m, Retarget::TranslationMode::None);
    EXPECT_FALSE(Retarget::translationModeFromId(QStringLiteral("x"), &m));
    Retarget::SourceRest r;
    EXPECT_TRUE(Retarget::sourceRestFromId(QStringLiteral("first-frame"), &r));
    EXPECT_EQ(r, Retarget::SourceRest::FirstFrame);
    EXPECT_EQ(Retarget::sourceRestId(r), QStringLiteral("first-frame"));
    EXPECT_EQ(Retarget::translationModeId(Retarget::TranslationMode::Root), QStringLiteral("root"));
}

// Real-rig naming seen on Quaternius characters: two Mixamo lower-spine
// bones share one role, "Torso" is the chest and "Palm" is the hand.
TEST(AnimationRetargeter, AutoMapPairsSharedRolesInHierarchyOrder)
{
    const std::vector<std::string> src = {"mixamorig:Hips", "mixamorig:Spine", "mixamorig:Spine1",
                                          "mixamorig:Spine2", "mixamorig:LeftHand", "mixamorig:RightHand"};
    const std::vector<std::string> tgt = {"Hips", "Abdomen", "Torso", "Palm.L", "Palm.R"};
    const auto rep = Retarget::autoMap(src, tgt);
    EXPECT_EQ(rep.map.targetFor("mixamorig:Spine"), "Abdomen") << "lowest spine to lowest spine";
    EXPECT_EQ(rep.map.targetFor("mixamorig:Spine1"), "");
    EXPECT_EQ(rep.map.targetFor("mixamorig:Spine2"), "Torso");
    EXPECT_EQ(rep.map.targetFor("mixamorig:LeftHand"), "Palm.L");
    EXPECT_EQ(rep.map.targetFor("mixamorig:RightHand"), "Palm.R");
}

// The root translation must follow the source's hips even when the target
// lists another mapped bone first (IK feet parented above the hips).
TEST(AnimationRetargeter, RootFollowsTheSourceHipsNotTargetOrder)
{
    const RigDesc src = mixamoRig();
    RigDesc tgt = unrealRig();
    // Re-parent foot_l to the root slot by moving it in front: build a rig
    // whose FIRST bone is an unmapped "root" with foot_l as its first child.
    RigDesc r;
    r.names = {"root", "foot_ik_l"};
    r.parent = {-1, 0};
    r.bindPos = {Ogre::Vector3::ZERO, Ogre::Vector3(0.1f, 0.0f, 0.0f)};
    r.bindRot = {Ogre::Quaternion::IDENTITY, Ogre::Quaternion::IDENTITY};
    for (int i = 0; i < tgt.size(); ++i) {
        r.names.push_back(tgt.names[size_t(i)]);
        r.parent.push_back(tgt.parent[size_t(i)] >= 0 ? tgt.parent[size_t(i)] + 2 : 0);
        r.bindPos.push_back(tgt.bindPos[size_t(i)]);
        r.bindRot.push_back(tgt.bindRot[size_t(i)]);
    }
    BoneMap map = Retarget::autoMap(src.names, r.names).map;
    map.setPair("mixamorig:LeftFoot", "foot_ik_l");
    Retarget::RetargetReport rep;
    const auto out = Retarget::retargetFrames(src, sourceClip(src, 3), r, map, {}, &rep);
    ASSERT_FALSE(out.empty()) << rep.error.toStdString();
    EXPECT_EQ(rep.rootTarget, "pelvis");
}

// A branching bone aligns to its CENTRAL child. Target: arms attached
// straight to the chest (UniRig-style, no clavicles) at a different height
// than the source's shoulders; the chest must NOT roll toward an arm.
TEST(AnimationRetargeter, BranchingBoneAlignsToItsCentralChild)
{
    const RigDesc src = mixamoRig();
    // target = mixamo layout minus the clavicles; arms re-parented to the
    // upper spine and raised so chest→arm differs from the source a lot
    std::vector<std::string> n;
    std::vector<int> parent;
    std::vector<Ogre::Vector3> wp;
    for (int i = 0; i < kN; ++i) {
        const std::string m = kDefs[i].mixamo;
        if (m == "LeftShoulder" || m == "RightShoulder") continue;
        n.push_back(m);
        wp.emplace_back(kDefs[i].x, kDefs[i].y + (m.find("Arm") != std::string::npos
                                                  || m.find("Hand") != std::string::npos ? 0.05f : 0.0f),
                        kDefs[i].z);
    }
    auto idx = [&](const std::string& name) { return int(std::find(n.begin(), n.end(), name) - n.begin()); };
    for (const auto& name : n) {
        std::string pn;
        for (const auto& d : kDefs) if (name == d.mixamo) pn = d.parent >= 0 ? kDefs[d.parent].mixamo : "";
        if (pn == "LeftShoulder" || pn == "RightShoulder") pn = "Spine1";
        parent.push_back(pn.empty() ? -1 : idx(pn));
    }
    RigDesc tgt;
    for (size_t i = 0; i < n.size(); ++i) {
        const int p = parent[i];
        tgt.names.push_back(n[i]);
        tgt.parent.push_back(p);
        tgt.bindRot.push_back(Ogre::Quaternion::IDENTITY);
        tgt.bindPos.push_back(p >= 0 ? wp[i] - wp[size_t(p)] : wp[i]);
    }
    const BoneMap map = Retarget::autoMap(src.names, tgt.names).map;
    const std::vector<LocalPose> still = {Retarget::bindPose(src), Retarget::bindPose(src)};
    const auto out = Retarget::retargetFrames(src, still, tgt, map, {});
    ASSERT_FALSE(out.empty());
    const int chest = tgt.indexOf("Spine1");
    EXPECT_GT(std::fabs(out[0].rot[size_t(chest)].Dot(Ogre::Quaternion::IDENTITY)), 1.0f - 1e-5f)
        << "the chest rolled toward an arm instead of following the neck";
}

// Height ratio with DIFFERENT up axes: a Z-up source driving a Y-up target
// must measure each rig along its own up (review finding — measuring along
// G·Y measured the Z-up source's depth).
TEST(AnimationRetargeter, HeightScaleUsesEachRigsOwnUpAxis)
{
    Ogre::Quaternion up;
    const RigDesc zUp = unrealRig(&up);      // Z-up, 1.4x
    const RigDesc yUp = mixamoRig();         // Y-up, 1x
    // source = Z-up rig, so build a clip in ITS names
    const BoneMap map = Retarget::autoMap(zUp.names, yUp.names).map;
    const std::vector<LocalPose> still = {Retarget::bindPose(zUp), Retarget::bindPose(zUp)};
    Retarget::RetargetReport rep;
    const auto out = Retarget::retargetFrames(zUp, still, yUp, map, {}, &rep);
    ASSERT_FALSE(out.empty()) << rep.error.toStdString();
    ASSERT_TRUE(rep.globalAlignment);
    EXPECT_NEAR(rep.heightScale, 1.0f / 1.4f, 1e-3f);
}

// Synonym outputs must not be rewritten by later entries (review finding:
// "upperarm" became "uuarm" via the trailing "arm" rule, so upper arm and
// forearm only matched by edit distance). With the upper arm already taken,
// a source "arm" must find nothing rather than fall onto the forearm.
TEST(AnimationRetargeter, ArmSynonymsMatchExactlyAndNeverFallOntoTheForearm)
{
    const auto a = Retarget::autoMap({"Bip01 L UpperArm", "Bip01 L ForeArm"}, {"lowerarm_l", "upperarm_l"});
    EXPECT_EQ(a.map.targetFor("Bip01 L UpperArm"), "upperarm_l");
    EXPECT_EQ(a.map.targetFor("Bip01 L ForeArm"), "lowerarm_l");
    const auto b = Retarget::autoMap({"Bip01 L UpperArm", "L_Arm"}, {"upperarm_l", "lowerarm_l"});
    EXPECT_EQ(b.map.targetFor("L_Arm"), "") << "an arm must not be mapped onto the forearm";
}

// A static helper root shared by both rigs ("Root" mapped by name) must not
// steal the root translation from the hips (Codex review finding).
TEST(AnimationRetargeter, RootTranslationGoesToTheHipsNotAHelperRoot)
{
    auto withHelper = [](const RigDesc& r) {
        RigDesc o;
        o.names = {"Root"};
        o.parent = {-1};
        o.bindPos = {Ogre::Vector3::ZERO};
        o.bindRot = {Ogre::Quaternion::IDENTITY};
        for (int i = 0; i < r.size(); ++i) {
            o.names.push_back(r.names[size_t(i)]);
            o.parent.push_back(r.parent[size_t(i)] >= 0 ? r.parent[size_t(i)] + 1 : 0);
            o.bindPos.push_back(r.bindPos[size_t(i)]);
            o.bindRot.push_back(r.bindRot[size_t(i)]);
        }
        return o;
    };
    const RigDesc src = withHelper(mixamoRig());
    const RigDesc tgt = withHelper(unrealRig());
    const BoneMap map = Retarget::autoMap(src.names, tgt.names).map;
    ASSERT_EQ(map.targetFor("Root"), "Root");
    std::vector<LocalPose> frames;
    for (const auto& f : sourceClip(mixamoRig(), 3)) {
        LocalPose lp;
        lp.pos.push_back(Ogre::Vector3::ZERO);
        lp.rot.push_back(Ogre::Quaternion::IDENTITY);
        lp.pos.insert(lp.pos.end(), f.pos.begin(), f.pos.end());
        lp.rot.insert(lp.rot.end(), f.rot.begin(), f.rot.end());
        frames.push_back(lp);
    }
    Retarget::RetargetReport rep;
    const auto out = Retarget::retargetFrames(src, frames, tgt, map, {}, &rep);
    ASSERT_FALSE(out.empty()) << rep.error.toStdString();
    EXPECT_EQ(rep.rootTarget, "pelvis");
    const int pel = tgt.indexOf("pelvis");
    EXPECT_GT((out.back().pos[size_t(pel)] - tgt.bindPos[size_t(pel)]).length(), 0.1f)
        << "the hips must carry the travel";
}
