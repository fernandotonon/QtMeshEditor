// Canonical quadruped skeleton (#1073). Pure data — no Ogre, no GL.
//
// Every bone name asserted here is taken from a REAL rig in the corpus
// (Quaternius LowPoly Animated Farm Animal Pack, CC0) rather than invented,
// so the mapper is pinned against the thing it actually has to handle.
#include "QuadrupedSkeleton.h"

#include <gtest/gtest.h>

#include <QString>

#include <vector>

using CreatureSkeleton::QuadrupedSkeleton::indexForBone;
using CreatureSkeleton::QuadrupedSkeleton::jointCount;
using CreatureSkeleton::QuadrupedSkeleton::jointName;
using CreatureSkeleton::QuadrupedSkeleton::parentOf;
using CreatureSkeleton::QuadrupedSkeleton::roleSpecificity;

TEST(QuadrupedSkeleton, TopologyIsWellFormed)
{
    ASSERT_EQ(jointCount(), 18);
    EXPECT_EQ(parentOf(0), -1) << "root must be the only parentless joint";
    for (int i = 1; i < jointCount(); ++i) {
        const int p = parentOf(i);
        EXPECT_GE(p, 0) << jointName(i).toStdString() << " has no parent";
        EXPECT_LT(p, i) << "parents must precede children so a single forward "
                           "pass can accumulate world transforms";
    }
}

TEST(QuadrupedSkeleton, FrontAndBackLegsAreDistinctRoles)
{
    // THE bug this module exists for: the humanoid mapper matches "upleg"
    // with no front/back distinction, so FrontUpLeg.R and BackUpLeg.R both
    // resolved to the same canonical joint — four legs collapsed onto two,
    // and the front legs were driven by hind-leg rotations ("legs moving
    // sideways"). All four upper-leg roles must be different.
    const int fl = indexForBone("FrontUpLeg.L");
    const int fr = indexForBone("FrontUpLeg.R");
    const int bl = indexForBone("BackUpLeg.L");
    const int br = indexForBone("BackUpLeg.R");
    for (int v : {fl, fr, bl, br}) ASSERT_GE(v, 0);
    const std::vector<int> all{fl, fr, bl, br};
    for (size_t i = 0; i < all.size(); ++i)
        for (size_t j = i + 1; j < all.size(); ++j)
            EXPECT_NE(all[i], all[j])
                << "leg roles collided: " << all[i] << " == " << all[j];
}

TEST(QuadrupedSkeleton, MapsEveryCoreBoneOfTheReferenceRig)
{
    // Exact bone names from Horse.fbx (Quaternius, 40 bones).
    struct Case { const char* bone; const char* role; };
    const Case cases[] = {
        {"Hips",            "root"},
        {"Body",            "spine"},
        {"Shoulders",       "chest"},
        {"Neck",            "neck"},
        {"Head",            "head"},
        {"FrontUpLeg.L",    "frontUpLeg.L"},
        {"FrontLowLeg.L",   "frontLowLeg.L"},
        {"FrontFoot.L",     "frontFoot.L"},
        {"FrontUpLeg.R",    "frontUpLeg.R"},
        {"FrontLowLeg.R",   "frontLowLeg.R"},
        {"FrontFoot.R",     "frontFoot.R"},
        {"BackUpLeg.L",     "backUpLeg.L"},
        {"BackLowLeg.L",    "backLowLeg.L"},
        {"BackFoot.L",      "backFoot.L"},
        {"BackUpLeg.R",     "backUpLeg.R"},
        {"BackLowLeg.R",    "backLowLeg.R"},
        {"BackFoot.R",      "backFoot.R"},
        {"Tail1",           "tail1"},
    };
    for (const auto& c : cases) {
        const int i = indexForBone(QString::fromLatin1(c.bone));
        ASSERT_GE(i, 0) << c.bone << " did not map";
        EXPECT_EQ(jointName(i).toStdString(), c.role) << "for bone " << c.bone;
    }
}

TEST(QuadrupedSkeleton, BackLegDoesNotLeakIntoTheSpine)
{
    // "BackUpLeg.L" contains "back", which is also the spine keyword on this
    // rig ("Back"). The leg test must run FIRST or every hind leg silently
    // becomes the spine — a subtle ordering bug with no visible error.
    EXPECT_EQ(jointName(indexForBone("Back")).toStdString(), "spine");
    EXPECT_EQ(jointName(indexForBone("BackUpLeg.L")).toStdString(), "backUpLeg.L");
    EXPECT_EQ(jointName(indexForBone("BackFoot.R")).toStdString(), "backFoot.R");
}

TEST(QuadrupedSkeleton, LeafAndHelperBonesAreRejected)
{
    // FBX exports append _end leaf markers; those alias a real joint and
    // would double-map if not stripped... but they must still resolve to the
    // SAME role, not a different one.
    EXPECT_EQ(indexForBone("Head_end"), indexForBone("Head"));
    EXPECT_EQ(indexForBone("FrontFoot.L_end"), indexForBone("FrontFoot.L"));
    // Non-core appendages carry no canonical role.
    EXPECT_EQ(indexForBone("Ear.L"), -1);
    EXPECT_EQ(indexForBone("Horn.R"), -1);
    EXPECT_EQ(indexForBone("IK_target"), -1);
}

TEST(QuadrupedSkeleton, OnlyTheFirstTailSegmentIsCanonical)
{
    // Tail2..4 are follow-through this skeleton deliberately does not model;
    // mapping them would need joints that do not exist.
    EXPECT_GE(indexForBone("Tail1"), 0);
    EXPECT_EQ(indexForBone("Tail2"), -1);
    EXPECT_EQ(indexForBone("Tail4_end"), -1);
}

TEST(QuadrupedSkeleton, PlausibilityNeedsSpineAndThreeLegs)
{
    std::vector<bool> roles(static_cast<size_t>(jointCount()), false);
    auto set = [&](std::initializer_list<int> idx) {
        roles.assign(static_cast<size_t>(jointCount()), false);
        for (int i : idx) roles[static_cast<size_t>(i)] = true;
    };
    auto plausible = [&] {
        std::vector<char> raw(roles.begin(), roles.end());
        return CreatureSkeleton::QuadrupedSkeleton::isPlausibleQuadruped(
            reinterpret_cast<const bool*>(raw.data()));
    };

    set({0, 2, 5, 8, 11, 14});          // spine + all four legs
    EXPECT_TRUE(plausible());
    set({0, 2, 5, 8, 11});              // three legs — still fine
    EXPECT_TRUE(plausible());
    set({0, 2, 5, 8});                  // two legs = a biped, must refuse
    EXPECT_FALSE(plausible());
    set({5, 8, 11, 14});                // legs but no spine anchor
    EXPECT_FALSE(plausible());
    EXPECT_FALSE(CreatureSkeleton::QuadrupedSkeleton::isPlausibleQuadruped(nullptr));
}

TEST(QuadrupedSkeleton, SideSpellingVariantsResolve)
{
    // Different rigs spell sides differently; all must reach the same role.
    EXPECT_EQ(jointName(indexForBone("FrontUpLeg.L")).toStdString(), "frontUpLeg.L");
    EXPECT_EQ(jointName(indexForBone("FrontUpLeg_L")).toStdString(), "frontUpLeg.L");
    EXPECT_EQ(jointName(indexForBone("LeftFrontUpLeg")).toStdString(), "frontUpLeg.L");
    // A bare "foreleg" is the TOPMOST limb bone on the three-segment rigs
    // (Leg -> UpLeg -> LowLeg), so it maps to the canonical UPPER leg.
    EXPECT_EQ(jointName(indexForBone("foreleg.r")).toStdString(), "frontUpLeg.R");
}

TEST(QuadrupedSkeleton, BareLegIsTheLimbROOT_NotTheLowerLeg)
{
    // The Quaternius rigs are THREE-segment: FrontLeg -> FrontUpLeg ->
    // FrontLowLeg, against the canonical UpLeg -> LowLeg -> Foot. The bare
    // `FrontLeg.L` is therefore the limb ROOT (the shoulder/hip attachment),
    // not the middle segment.
    //
    // It was previously mapped to the LOWER leg, where it lost the role to
    // the explicitly-named `FrontLowLeg.L` and so received no animation at
    // all, while the canonical upper-leg role drove `FrontUpLeg.L` -- the
    // SECOND segment. The limb swung from the wrong joint, which renders as
    // a cramped stride with the legs bunched under the body even though every
    // per-joint angle still matches the source exactly. Measured as ~40 deg
    // of world-orientation error on a SELF-retarget, which must be ~0.
    EXPECT_EQ(jointName(indexForBone("FrontLeg.L")).toStdString(), "frontUpLeg.L");
    EXPECT_EQ(jointName(indexForBone("BackLeg.R")).toStdString(), "backUpLeg.R");

    // And it must BEAT the explicit `FrontUpLeg` for that role: hierarchy
    // position is stronger evidence than a segment word here.
    EXPECT_GT(roleSpecificity("FrontLeg.L"), roleSpecificity("FrontUpLeg.L"));
    EXPECT_GT(roleSpecificity("BackLeg.R"), roleSpecificity("BackUpLeg.R"));

    // An explicitly-named segment still beats a generic keyword match.
    EXPECT_GT(roleSpecificity("FrontLowLeg.L"), roleSpecificity("Body"));
}

// ---- winged biped (dragon / bat) -------------------------------------------
namespace WB = CreatureSkeleton::WingedBiped;

TEST(WingedBipedSkeleton, MapsTheReferenceDragonRig)
{
    // Exact bone names from Monster_Pack_Animated Dragon.fbx / Bat.fbx —
    // the two share an identical structure, which is why one plan serves both.
    struct Case { const char* bone; const char* role; };
    const Case cases[] = {
        {"BodyRoot",   "root"},
        {"Body",       "spine"},
        {"Neck",       "neck"},
        {"Head",       "head"},
        {"Shoulder.L", "wingRoot.L"},
        {"Wing1.L",    "wing1.L"},
        {"Wing2.L",    "wing2.L"},
        {"Shoulder.R", "wingRoot.R"},
        {"Wing1.R",    "wing1.R"},
        {"UpperLeg.L", "upLeg.L"},
        {"LowerLeg.L", "lowLeg.L"},
        {"UpperLeg.R", "upLeg.R"},
        {"Tail1",      "tail1"},
    };
    for (const auto& c : cases) {
        const int i = WB::indexForBone(QString::fromLatin1(c.bone));
        ASSERT_GE(i, 0) << c.bone << " did not map";
        EXPECT_EQ(WB::jointName(i).toStdString(), c.role) << "for bone " << c.bone;
    }
}

TEST(WingedBipedSkeleton, ShoulderIsAWingRootNotAHumanoidClavicle)
{
    // On these rigs "Shoulder.L" carries the WING, so treating it as a
    // humanoid collar would drive a wing with arm motion — the same class of
    // silent mis-retarget as the quadruped leg collision.
    EXPECT_EQ(WB::jointName(WB::indexForBone("Shoulder.L")).toStdString(), "wingRoot.L");
    EXPECT_NE(WB::indexForBone("Shoulder.L"), WB::indexForBone("Shoulder.R"));
}

TEST(WingedBipedSkeleton, MembraneTipAndFaceBonesCarryNoRole)
{
    EXPECT_EQ(WB::indexForBone("Wing4.L"), -1);   // follow-through only
    EXPECT_EQ(WB::indexForBone("Face"), -1);
    EXPECT_EQ(WB::indexForBone("Nose"), -1);
    EXPECT_EQ(WB::indexForBone("EyeArmature"), -1);
}

TEST(WingedBipedSkeleton, PlausibilityRequiresBothWings)
{
    std::vector<char> roles(static_cast<size_t>(WB::jointCount()), 0);
    auto p = [&] { return WB::isPlausible(reinterpret_cast<const bool*>(roles.data())); };
    roles.assign(roles.size(), 0);
    roles[0] = 1; roles[5] = 1; roles[8] = 1;      // spine + both wings
    EXPECT_TRUE(p());
    roles.assign(roles.size(), 0);
    roles[0] = 1; roles[5] = 1;                    // one wing only
    EXPECT_FALSE(p());
    roles.assign(roles.size(), 0);
    roles[5] = 1; roles[8] = 1;                    // wings but no spine anchor
    EXPECT_FALSE(p());
}

TEST(CreatureBodyPlan, DetectsQuadrupedAndWingedBipedFromRealBoneLists)
{
    // Horse.fbx (Quaternius farm pack)
    const QStringList horse{"Hips","Body","Back","Shoulders","Neck","Head",
        "FrontUpLeg.L","FrontLowLeg.L","FrontFoot.L","FrontUpLeg.R","FrontLowLeg.R",
        "FrontFoot.R","BackUpLeg.L","BackLowLeg.L","BackFoot.L","BackUpLeg.R",
        "BackLowLeg.R","BackFoot.R","Tail1"};
    bool ok = false;
    EXPECT_EQ(CreatureSkeleton::detectBodyPlan(horse, &ok),
              CreatureSkeleton::BodyPlan::Quadruped);
    EXPECT_TRUE(ok);

    // Dragon.fbx (Quaternius monster pack) — has legs too, so this pins the
    // ordering: wings must win, or a dragon would retarget as a quadruped.
    const QStringList dragon{"Root","BodyRoot","Body","Neck","Head","Shoulder.L",
        "Wing1.L","Wing2.L","Wing3.L","Shoulder.R","Wing1.R","Wing2.R","Wing3.R",
        "UpperLeg.L","LowerLeg.L","UpperLeg.R","LowerLeg.R","Tail1"};
    ok = false;
    EXPECT_EQ(CreatureSkeleton::detectBodyPlan(dragon, &ok),
              CreatureSkeleton::BodyPlan::WingedBiped);
    EXPECT_TRUE(ok);
}

TEST(CreatureBodyPlan, RejectsRigsItCannotRead)
{
    // A humanoid must NOT be claimed by a creature plan...
    const QStringList human{"Hips","Spine","Spine1","Neck","Head","LeftArm",
        "LeftForeArm","LeftHand","RightArm","RightForeArm","RightHand",
        "LeftUpLeg","LeftLeg","LeftFoot","RightUpLeg","RightLeg","RightFoot"};
    bool ok = true;
    CreatureSkeleton::detectBodyPlan(human, &ok);
    EXPECT_FALSE(ok) << "a biped humanoid must not be detected as a creature";

    // ...nor must a rig with no semantic names at all (Eagle.fbx is literally
    // Bone.001, Bone.002, … — 14 of 25 surveyed packs look like this, which
    // is why birds/fish/insects are out of scope for a NAME-based mapper).
    const QStringList generic{"Bone","Bone.001","Bone.002","Bone.003","Bone.004",
        "Bone.005","Bone.006","Bone.007","Bone.008"};
    ok = true;
    CreatureSkeleton::detectBodyPlan(generic, &ok);
    EXPECT_FALSE(ok) << "unnamed rigs must be refused, not guessed at";
}

TEST(CreatureBodyPlan, MixamoShoulderDoesNotFakeAWing)
{
    // Regression, found by auditing the mapper against every rig in the
    // corpus rather than by the fixtures: WingedBiped maps "Shoulder.L" to
    // the wing root (correct for the dragon/bat rigs), but a Mixamo humanoid
    // spells its clavicle LeftShoulder/RightShoulder — so Animated_Woman,
    // KnightCharacter and Alien were all detected as winged bipeds and would
    // have had their arms retargeted as wings.
    const QStringList mixamo{"Hips","Spine","Spine1","Spine2","Neck","Head",
        "LeftShoulder","LeftArm","LeftForeArm","LeftHand",
        "RightShoulder","RightArm","RightForeArm","RightHand",
        "LeftUpLeg","LeftLeg","LeftFoot","RightUpLeg","RightLeg","RightFoot"};
    bool ok = true;
    CreatureSkeleton::detectBodyPlan(mixamo, &ok);
    EXPECT_FALSE(ok) << "a Mixamo humanoid must not be read as a winged biped";
}
