/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

// Skel slice E (#559): left/right bone-naming convention detection.
// Pure-data — no Ogre, no display, safe under Xvfb / headless CI.

#include <gtest/gtest.h>

#include "BoneNamingMap.h"

using BoneNaming::Convention;
using BoneNaming::CustomRule;
using BoneNaming::Map;
using BoneNaming::Side;

namespace {

// ---------------------------------------------------------------------------
// Built-in convention 1: generic _l / _r  (and _L / _R)
// ---------------------------------------------------------------------------
TEST(BoneNamingMap, UnderscoreLowercaseSuffix)
{
    Map m;
    EXPECT_EQ(m.counterpart("Hand_l"), "Hand_r");
    EXPECT_EQ(m.counterpart("Hand_r"), "Hand_l");
    EXPECT_EQ(m.sideOf("Hand_l"), Side::Left);
    EXPECT_EQ(m.sideOf("Hand_r"), Side::Right);
    EXPECT_EQ(m.conventionOf("Hand_l"), Convention::UnderscoreLR);
}

TEST(BoneNamingMap, UnderscoreUppercaseSuffixPreservesCase)
{
    Map m;
    EXPECT_EQ(m.counterpart("Hand_L"), "Hand_R");
    EXPECT_EQ(m.counterpart("Hand_R"), "Hand_L");
}

TEST(BoneNamingMap, UnderscorePrefix)
{
    Map m;
    EXPECT_EQ(m.counterpart("L_UpperArm"), "R_UpperArm");
    EXPECT_EQ(m.counterpart("R_UpperArm"), "L_UpperArm");
    EXPECT_EQ(m.sideOf("L_UpperArm"), Side::Left);
}

// ---------------------------------------------------------------------------
// Built-in convention 2: Blender .l / .r  (and .L / .R)
// ---------------------------------------------------------------------------
TEST(BoneNamingMap, BlenderDotSuffix)
{
    Map m;
    EXPECT_EQ(m.counterpart("hand.l"), "hand.r");
    EXPECT_EQ(m.counterpart("hand.r"), "hand.l");
    EXPECT_EQ(m.counterpart("forearm.L"), "forearm.R");
    EXPECT_EQ(m.counterpart("forearm.R"), "forearm.L");
    EXPECT_EQ(m.conventionOf("hand.l"), Convention::DotLR);
}

// ---------------------------------------------------------------------------
// Built-in convention 3: Left* / Right* words (Mixamo, Unity HumanIK)
// ---------------------------------------------------------------------------
TEST(BoneNamingMap, LeftRightWordPrefix)
{
    Map m;
    EXPECT_EQ(m.counterpart("LeftHand"), "RightHand");
    EXPECT_EQ(m.counterpart("RightHand"), "LeftHand");
    EXPECT_EQ(m.sideOf("LeftShoulder"), Side::Left);
    EXPECT_EQ(m.conventionOf("LeftHand"), Convention::LeftRightWord);
}

TEST(BoneNamingMap, MixamoNamespacedPrefixIsHandled)
{
    // The old PoseLibrary::flipBoneName required the name to START with
    // "Left", so every `mixamorig:`-namespaced bone silently failed to pair.
    Map m;
    EXPECT_EQ(m.counterpart("mixamorig:LeftArm"), "mixamorig:RightArm");
    EXPECT_EQ(m.counterpart("mixamorig:RightArm"), "mixamorig:LeftArm");
    EXPECT_EQ(m.sideOf("mixamorig:LeftArm"), Side::Left);
}

TEST(BoneNamingMap, ExplicitAffixWinsOverAnEmbeddedSideWord)
{
    // Real Mixamo-derived rigs produce names carrying BOTH signals, where
    // "Left" is part of the inherited bone name and "_l" is the actual side
    // marker. Flipping the word instead would give "mixamorig_RightUpLeg_l":
    // a name whose halves disagree about the side, matching no real bone.
    Map m;
    EXPECT_EQ(m.counterpart("mixamorig_LeftUpLeg_l"), "mixamorig_LeftUpLeg_r");
    EXPECT_EQ(m.counterpart("mixamorig_LeftUpLeg_r"), "mixamorig_LeftUpLeg_l");
    EXPECT_EQ(m.conventionOf("mixamorig_LeftUpLeg_l"), Convention::UnderscoreLR);
    // Without a competing affix the word rule still applies.
    EXPECT_EQ(m.counterpart("mixamorig:LeftUpLeg"), "mixamorig:RightUpLeg");
}

TEST(BoneNamingMap, LowercaseAndAllCapsSideWords)
{
    Map m;
    EXPECT_EQ(m.counterpart("left_hand"), "right_hand");
    EXPECT_EQ(m.counterpart("right_hand"), "left_hand");
    EXPECT_EQ(m.counterpart("LEFT_HAND"), "RIGHT_HAND");
    EXPECT_EQ(m.counterpart("RIGHT_HAND"), "LEFT_HAND");
}

// ---------------------------------------------------------------------------
// Built-in convention 4: *_left / *_right
// ---------------------------------------------------------------------------
TEST(BoneNamingMap, SuffixLeftRightWord)
{
    Map m;
    EXPECT_EQ(m.counterpart("hand_left"), "hand_right");
    EXPECT_EQ(m.counterpart("hand_right"), "hand_left");
    EXPECT_EQ(m.conventionOf("hand_left"), Convention::SuffixLeftWord);
    EXPECT_EQ(m.sideOf("hand_right"), Side::Right);
}

// ---------------------------------------------------------------------------
// Bonus conventions
// ---------------------------------------------------------------------------
TEST(BoneNamingMap, BipedStandaloneSideToken)
{
    // 3ds Max Biped: the side is a whole space-delimited token.
    Map m;
    EXPECT_EQ(m.counterpart("Bip001 L UpperArm"), "Bip001 R UpperArm");
    EXPECT_EQ(m.counterpart("Bip001 R Thigh"), "Bip001 L Thigh");
    EXPECT_EQ(m.conventionOf("Bip001 L UpperArm"), Convention::BipedToken);
}

TEST(BoneNamingMap, CamelCaseGluedLetter)
{
    Map m;
    EXPECT_EQ(m.counterpart("lHand"), "rHand");
    EXPECT_EQ(m.counterpart("rHand"), "lHand");
    EXPECT_EQ(m.counterpart("handL"), "handR");
    EXPECT_EQ(m.counterpart("handR"), "handL");
}

// ---------------------------------------------------------------------------
// Centre bones and false positives
// ---------------------------------------------------------------------------
TEST(BoneNamingMap, CentreBonesHaveNoCounterpart)
{
    Map m;
    for (const char* n : {"Spine", "Hips", "Head", "Neck", "Chest", "Root"}) {
        EXPECT_EQ(m.sideOf(n), Side::Center) << n;
        // Empty, NOT the input echoed back: "no counterpart" must be
        // distinguishable from "maps to itself", or a mirror op would write a
        // centre bone onto itself through the pair path.
        EXPECT_TRUE(m.counterpart(n).isEmpty()) << n << " -> " << m.counterpart(n).toStdString();
    }
}

TEST(BoneNamingMap, WordsMerelyContainingSideLettersAreNotFlipped)
{
    Map m;
    // "Shoulder" ends in 'r', "Heel" ends in 'l' — a naive trailing-letter
    // rule would rename them and quietly corrupt a mirror.
    EXPECT_TRUE(m.counterpart("Shoulder").isEmpty());
    EXPECT_TRUE(m.counterpart("Heel").isEmpty());
    EXPECT_TRUE(m.counterpart("Pelvis").isEmpty());
    // "Lefty" is not the word "left".
    EXPECT_TRUE(m.counterpart("Lefty").isEmpty());
    EXPECT_TRUE(m.counterpart("Rightmost").isEmpty());
}

TEST(BoneNamingMap, EmptyNameIsSafe)
{
    Map m;
    EXPECT_TRUE(m.counterpart(QString()).isEmpty());
    EXPECT_EQ(m.sideOf(QString()), Side::Center);
}

TEST(BoneNamingMap, RoundTripIsInvolutive)
{
    // Flipping twice must return the original for every convention, or
    // "mirror twice" would not be a no-op.
    Map m;
    for (const char* n : {"Hand_l", "hand.R", "LeftArm", "hand_right",
                          "L_Foot", "lHand", "handR", "Bip001 L Thigh",
                          "mixamorig:RightLeg"}) {
        const QString once = m.counterpart(n);
        ASSERT_FALSE(once.isEmpty()) << n;
        EXPECT_EQ(m.counterpart(once), QString(n)) << n;
    }
}

// ---------------------------------------------------------------------------
// Custom conventions
// ---------------------------------------------------------------------------
TEST(BoneNamingMap, CustomRuleMatchesUserConvention)
{
    // A rig using "__lft" / "__rgt", which no built-in rule knows.
    CustomRule rule;
    rule.leftPattern      = "__lft$";
    rule.leftReplacement  = "__rgt";
    rule.rightPattern     = "__rgt$";
    rule.rightReplacement = "__lft";
    ASSERT_TRUE(rule.isValid());

    Map m({rule});
    EXPECT_EQ(m.counterpart("arm__lft"), "arm__rgt");
    EXPECT_EQ(m.counterpart("arm__rgt"), "arm__lft");
    EXPECT_EQ(m.sideOf("arm__lft"), Side::Left);
    EXPECT_EQ(m.conventionOf("arm__lft"), Convention::Custom);
}

TEST(BoneNamingMap, CustomRuleWinsOverBuiltIn)
{
    // Custom rules are tried first so a user can correct a rig the defaults
    // read wrongly. Here the built-ins would pair "ctrl_l" as underscore-LR;
    // the custom rule redirects it elsewhere.
    CustomRule rule;
    rule.leftPattern      = "^ctrl_l$";
    rule.leftReplacement  = "ctrl_opposite";
    rule.rightPattern     = "^ctrl_opposite$";
    rule.rightReplacement = "ctrl_l";
    Map m({rule});
    EXPECT_EQ(m.counterpart("ctrl_l"), "ctrl_opposite");
    // Built-ins still apply to everything the custom rule doesn't claim.
    EXPECT_EQ(m.counterpart("Hand_l"), "Hand_r");
}

TEST(BoneNamingMap, InvalidCustomRuleIsIgnoredNotCrashing)
{
    CustomRule bad;
    bad.leftPattern      = "([unclosed";   // invalid regex
    bad.leftReplacement  = "x";
    bad.rightPattern     = "y";
    bad.rightReplacement = "z";
    EXPECT_FALSE(bad.isValid());

    Map m({bad});
    // Falls through to the built-ins rather than throwing or matching junk.
    EXPECT_EQ(m.counterpart("Hand_l"), "Hand_r");
}

// ---------------------------------------------------------------------------
// Pairing diagnostic (the inspector panel's data source)
// ---------------------------------------------------------------------------
TEST(BoneNamingMap, PairingReportListsPairsAndCentreBones)
{
    Map m;
    const QStringList bones{
        "Hips", "Spine", "Head",
        "LeftArm", "RightArm",
        "LeftLeg", "RightLeg",
    };
    const auto rep = m.pair(bones);
    EXPECT_EQ(rep.pairCount(), 2);
    // Each pair is emitted once, left first.
    ASSERT_EQ(rep.pairs.size(), 2);
    EXPECT_EQ(rep.pairs[0].left, "LeftArm");
    EXPECT_EQ(rep.pairs[0].right, "RightArm");
    EXPECT_EQ(rep.pairs[1].left, "LeftLeg");
    EXPECT_EQ(rep.pairs[1].right, "RightLeg");
    EXPECT_EQ(rep.centerBones, QStringList({"Hips", "Spine", "Head"}));
    EXPECT_TRUE(rep.unpairedSided.isEmpty());
    EXPECT_TRUE(rep.collisions.isEmpty());
}

TEST(BoneNamingMap, PairingReportFlagsSidedBoneWithMissingPartner)
{
    // A sided bone whose partner is absent is what makes a mirror silently
    // incomplete, so it must be reported apart from centre bones.
    Map m;
    const auto rep = m.pair({"Hips", "LeftArm"});
    EXPECT_EQ(rep.pairCount(), 0);
    EXPECT_EQ(rep.unpairedSided, QStringList({"LeftArm"}));
    EXPECT_EQ(rep.centerBones, QStringList({"Hips"}));
}

TEST(BoneNamingMap, PairingReportDetectsCollisions)
{
    // Two distinct bones flipping onto the same target would make a mirror
    // DROP data (the old PoseLibrary insert-into-hash did exactly that).
    CustomRule rule;
    rule.leftPattern      = "^(armA_l|armB_l)$";
    rule.leftReplacement  = "arm_r";
    rule.rightPattern     = "^arm_r$";
    rule.rightReplacement = "armA_l";
    Map m({rule});
    const auto rep = m.pair({"armA_l", "armB_l", "arm_r"});
    EXPECT_FALSE(rep.collisions.isEmpty());
}

TEST(BoneNamingMap, CollisionsAreReportedRegardlessOfBoneOrder)
{
    // The diagnostic must not depend on the order bones happen to appear in.
    // With the non-mutual source FIRST, it used to fail the mutuality check
    // before claiming the target, the mutual pair then succeeded, and the
    // report came back with no collision at all — the same rig reporting a
    // problem or not depending purely on skeleton bone order.
    CustomRule rule;
    rule.leftPattern      = "^(armA_l|armB_l)$";
    rule.leftReplacement  = "arm_r";
    rule.rightPattern     = "^arm_r$";
    rule.rightReplacement = "armA_l";
    Map m({rule});

    for (const QStringList& bones : {QStringList{"armA_l", "armB_l", "arm_r"},
                                     QStringList{"armB_l", "armA_l", "arm_r"}}) {
        const auto rep = m.pair(bones);
        EXPECT_FALSE(rep.collisions.isEmpty())
            << "no collision reported for order: " << bones.join(",").toStdString();
    }
}

TEST(BoneNamingMap, MixedConventionRigStillPairs)
{
    // Real assets are inconsistent — a rig can carry deform bones in one
    // convention and control bones in another.
    Map m;
    const auto rep = m.pair({"Hand_l", "Hand_r", "LeftFoot", "RightFoot", "Spine"});
    EXPECT_EQ(rep.pairCount(), 2);
    EXPECT_EQ(rep.centerBones, QStringList({"Spine"}));
}

// ---------------------------------------------------------------------------
// buildPairTable — the handle→handle bridge shared by the GUI and the CLI
// ---------------------------------------------------------------------------
TEST(BoneNamingMap, BuildPairTableMapsHandlesBothWays)
{
    Map m;
    // Handle-indexed name list, as SkinEvaluate::EvalData::boneNames is.
    const QStringList names{"Root", "Hand_l", "Hand_r", "Spine"};
    int pairs = 0;
    const std::vector<int> t = BoneNaming::buildPairTable(names, m, &pairs);
    ASSERT_EQ(t.size(), 4u);
    EXPECT_EQ(t[0], -1) << "Root is a centre bone";
    EXPECT_EQ(t[1], 2)  << "Hand_l -> Hand_r's handle";
    EXPECT_EQ(t[2], 1)  << "Hand_r -> Hand_l's handle";
    EXPECT_EQ(t[3], -1) << "Spine is a centre bone";
    EXPECT_EQ(pairs, 1);
}

TEST(BoneNamingMap, NonReciprocalCustomRuleIsNotReportedAsAPair)
{
    // A syntactically valid custom rule can map A -> B without B -> A. Treating
    // that as a pair is WORSE than reporting nothing: `buildPairTable` would
    // map A onto B while B maps onto itself, so a weight mirror sends BOTH
    // sides' weights to B instead of swapping them — corrupting the result
    // while reporting success. A pair must be mutual.
    CustomRule rule;
    rule.leftPattern      = "^armA_l$";
    rule.leftReplacement  = "armB_r";
    // The reverse direction deliberately points somewhere else entirely.
    rule.rightPattern     = "^armB_r$";
    rule.rightReplacement = "somewhere_else";
    Map m({rule});

    const QStringList bones{"armA_l", "armB_r"};
    const auto rep = m.pair(bones);
    EXPECT_EQ(rep.pairCount(), 0) << "a one-way mapping is not a pair";

    // And the handle table must not map it either, or the weight mirror
    // silently folds both sides onto one bone.
    int pairs = -1;
    const std::vector<int> t = BoneNaming::buildPairTable(bones, m, &pairs);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[0], -1) << "armA_l must not map onto a bone that does not map back";
    EXPECT_EQ(pairs, 0);
}

TEST(BoneNamingMap, BuildPairTableLeavesAMissingPartnerUnmapped)
{
    // A sided bone whose partner is absent must map to -1 ("keep your own
    // weights"), not to some other handle.
    Map m;
    const QStringList names{"Root", "Hand_l"};
    int pairs = 0;
    const std::vector<int> t = BoneNaming::buildPairTable(names, m, &pairs);
    ASSERT_EQ(t.size(), 2u);
    EXPECT_EQ(t[1], -1);
    EXPECT_EQ(pairs, 0);
}

TEST(BoneNamingMap, BuildPairTableOnAnEmptySkeletonIsSafe)
{
    Map m;
    int pairs = -1;
    const std::vector<int> t = BoneNaming::buildPairTable({}, m, &pairs);
    EXPECT_TRUE(t.empty());
    EXPECT_EQ(pairs, 0);
}

TEST(BoneNamingMap, HasAnyPairGatesMirrorUi)
{
    Map m;
    EXPECT_FALSE(m.hasAnyPair({"Bone1", "Bone2", "Bone3"}));
    EXPECT_TRUE(m.hasAnyPair({"Bone_l", "Bone_r"}));
}

// ---------------------------------------------------------------------------
// Fixture rigs — real bone names, not hand-invented ones
// ---------------------------------------------------------------------------
TEST(BoneNamingMap, FixtureRigMixamo)
{
    // Verbatim bone names from media/models/Rumba Dancing.fbx (the project's
    // canonical Mixamo asset), including the `L_Eye_Geo` / `R_Eye_Geo` helper
    // nodes that use a DIFFERENT convention from the skeleton proper.
    Map m;
    const QStringList bones{
        "mixamorig:Hips", "mixamorig:Spine", "mixamorig:Spine1", "mixamorig:Spine2",
        "mixamorig:Neck", "mixamorig:Neck1", "mixamorig:Head", "mixamorig:HeadTop_End",
        "mixamorig:LeftShoulder", "mixamorig:LeftArm", "mixamorig:LeftForeArm",
        "mixamorig:LeftHand", "mixamorig:LeftHandThumb1", "mixamorig:LeftHandIndex1",
        "mixamorig:RightShoulder", "mixamorig:RightArm", "mixamorig:RightForeArm",
        "mixamorig:RightHand", "mixamorig:RightHandThumb1", "mixamorig:RightHandIndex1",
        "mixamorig:LeftUpLeg", "mixamorig:LeftLeg", "mixamorig:LeftFoot",
        "mixamorig:LeftToeBase", "mixamorig:LeftToe_End",
        "mixamorig:RightUpLeg", "mixamorig:RightLeg", "mixamorig:RightFoot",
        "mixamorig:RightToeBase", "mixamorig:RightToe_End",
        "mixamorig:LeftEye", "mixamorig:RightEye",
        "mixamorig:L_Eye_Geo", "mixamorig:R_Eye_Geo",
    };
    const auto rep = m.pair(bones);
    // 12 anatomical pairs (shoulder/arm/forearm/hand/thumb/index, upleg/leg/
    // foot/toebase/toe-end, eye) + the L_/R_ eye-geo pair, which uses a
    // DIFFERENT convention from the skeleton proper — a mixed-convention rig
    // in real shipped data, not a contrived case.
    EXPECT_EQ(rep.pairCount(), 13);
    EXPECT_TRUE(rep.collisions.isEmpty());
    EXPECT_TRUE(rep.unpairedSided.isEmpty())
        << rep.unpairedSided.join(", ").toStdString();
    // The centre chain must NOT be paired.
    EXPECT_EQ(rep.centerBones, QStringList({
        "mixamorig:Hips", "mixamorig:Spine", "mixamorig:Spine1", "mixamorig:Spine2",
        "mixamorig:Neck", "mixamorig:Neck1", "mixamorig:Head", "mixamorig:HeadTop_End"}));
    // Spot-check a specific mapping rather than trusting only the count.
    EXPECT_EQ(m.counterpart("mixamorig:LeftHandThumb1"), "mixamorig:RightHandThumb1");
    EXPECT_EQ(m.counterpart("mixamorig:L_Eye_Geo"), "mixamorig:R_Eye_Geo");
}

TEST(BoneNamingMap, FixtureRigUnityHumanIK)
{
    // Unity's Mecanim / HumanIK avatar bone names.
    Map m;
    const QStringList bones{
        "Hips", "Spine", "Chest", "UpperChest", "Neck", "Head",
        "LeftUpperArm", "LeftLowerArm", "LeftHand",
        "RightUpperArm", "RightLowerArm", "RightHand",
        "LeftUpperLeg", "LeftLowerLeg", "LeftFoot", "LeftToes",
        "RightUpperLeg", "RightLowerLeg", "RightFoot", "RightToes",
    };
    const auto rep = m.pair(bones);
    EXPECT_EQ(rep.pairCount(), 7);
    EXPECT_TRUE(rep.unpairedSided.isEmpty());
    EXPECT_EQ(rep.centerBones.size(), 6);
}

TEST(BoneNamingMap, FixtureRigGenericUnderscore)
{
    Map m;
    const QStringList bones{
        "root", "pelvis", "spine_01", "spine_02", "neck_01", "head",
        "upperarm_l", "lowerarm_l", "hand_l",
        "upperarm_r", "lowerarm_r", "hand_r",
        "thigh_l", "calf_l", "foot_l",
        "thigh_r", "calf_r", "foot_r",
    };
    const auto rep = m.pair(bones);
    EXPECT_EQ(rep.pairCount(), 6);
    EXPECT_TRUE(rep.unpairedSided.isEmpty());
    // "spine_01" etc. must stay centre — a digit-suffixed name has no side.
    EXPECT_EQ(rep.centerBones.size(), 6);
}

TEST(BoneNamingMap, FixtureRigBlenderDot)
{
    Map m;
    const QStringList bones{
        "Hips", "Spine", "Head",
        "Shoulder.L", "UpperArm.L", "Hand.L",
        "Shoulder.R", "UpperArm.R", "Hand.R",
        "Thigh.l", "Shin.l", "Thigh.r", "Shin.r",
    };
    const auto rep = m.pair(bones);
    EXPECT_EQ(rep.pairCount(), 5);
    EXPECT_TRUE(rep.unpairedSided.isEmpty());
    EXPECT_EQ(rep.centerBones, QStringList({"Hips", "Spine", "Head"}));
}

} // namespace
