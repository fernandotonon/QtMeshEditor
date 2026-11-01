#include <gtest/gtest.h>

#include "SomaSkeleton.h"
#include "SomaRetarget.h"

#include <map>

using namespace AIMotion;

namespace {

// somaskel77, in Kimodo's order (kimodo/skeleton/definitions.py, SOMASkeleton77).
std::vector<std::string> soma77()
{
    std::vector<std::string> n = {"Hips", "Spine1", "Spine2", "Chest", "Neck1", "Neck2", "Head", "HeadEnd", "Jaw", "LeftEye", "RightEye"};
    for (const char* side : {"Left", "Right"}) {
        const std::string s = side;
        for (const char* p : {"Shoulder", "Arm", "ForeArm", "Hand"}) n.push_back(s + p);
        for (const char* t : {"Thumb1", "Thumb2", "Thumb3", "ThumbEnd"}) n.push_back(s + "Hand" + t);
        for (const char* f : {"Index", "Middle", "Ring", "Pinky"})
            for (const char* k : {"1", "2", "3", "4", "End"}) n.push_back(s + "Hand" + f + k);
    }
    for (const char* side : {"Left", "Right"})
        for (const char* p : {"Leg", "Shin", "Foot", "ToeBase", "ToeEnd"}) n.push_back(std::string(side) + p);
    return n;
}

} // namespace

TEST(SomaSkeleton, TheFullSkeletonIsClassifiedWithNoUnknowns)
{
    const auto names = soma77();
    ASSERT_EQ(names.size(), 77u);
    std::map<SomaRole, int> count;
    for (const auto& n : names) {
        const SomaRole r = somaJointRole(n);
        EXPECT_NE(r, SomaRole::Unknown) << n;
        ++count[r];
    }
    // 7 spine/head + 2*(4 arm + 3 thumb + 4*3 finger + 4 leg) = 7 + 46 = 53 mapped.
    EXPECT_EQ(count[SomaRole::Mapped], 53);
    EXPECT_EQ(count[SomaRole::Metacarpal], 8);
    EXPECT_EQ(count[SomaRole::Facial], 3);
    EXPECT_EQ(count[SomaRole::EndEffector], 13);   // HeadEnd + 2*(5 fingers + toe)
    EXPECT_TRUE(looksLikeSoma(names));
}

TEST(SomaSkeleton, LegAndFingerNamesAreShiftedNotMatchedLiterally)
{
    // The two traps: SOMA LeftLeg is the THIGH, and SOMA's Index1 is the metacarpal.
    EXPECT_EQ(somaStandardName("LeftLeg"), "LeftUpLeg");
    EXPECT_EQ(somaStandardName("LeftShin"), "LeftLeg");
    EXPECT_EQ(somaStandardName("RightHandIndex2"), "RightHandIndex1");
    EXPECT_EQ(somaStandardName("RightHandPinky4"), "RightHandPinky3");
    EXPECT_EQ(somaStandardName("LeftHandIndex1"), "");
    EXPECT_EQ(somaStandardName("LeftHandThumb1"), "LeftHandThumb1");
    EXPECT_EQ(somaStandardName("Chest"), "Spine2");
    EXPECT_EQ(somaStandardName("Spine1"), "Spine");
    EXPECT_EQ(somaStandardName("Neck2"), "Neck1");
    EXPECT_EQ(somaJointRole("LeftLegg"), SomaRole::Unknown) << "near misses are not guessed";
}

TEST(SomaSkeleton, MapsOntoAMixamoRigWithTheRightLegBones)
{
    Motion m;
    m.names = soma77();
    const std::vector<std::string> mixamo = {
        "mixamorig:Hips", "mixamorig:Spine", "mixamorig:Spine1", "mixamorig:Spine2", "mixamorig:Neck", "mixamorig:Head",
        "mixamorig:LeftShoulder", "mixamorig:LeftArm", "mixamorig:LeftForeArm", "mixamorig:LeftHand",
        "mixamorig:LeftHandIndex1", "mixamorig:LeftHandIndex2", "mixamorig:LeftHandIndex3",
        "mixamorig:LeftUpLeg", "mixamorig:LeftLeg", "mixamorig:LeftFoot", "mixamorig:LeftToeBase",
        "mixamorig:RightUpLeg", "mixamorig:RightLeg", "mixamorig:RightFoot"};
    QStringList unmapped, excluded;
    const Retarget::BoneMap map = somaBoneMap(m, mixamo, &unmapped, &excluded);
    EXPECT_EQ(map.targetFor("LeftLeg"), "mixamorig:LeftUpLeg");
    EXPECT_EQ(map.targetFor("LeftShin"), "mixamorig:LeftLeg");
    EXPECT_EQ(map.targetFor("RightShin"), "mixamorig:RightLeg");
    EXPECT_EQ(map.targetFor("LeftHandIndex2"), "mixamorig:LeftHandIndex1");
    EXPECT_EQ(map.targetFor("LeftHandIndex1"), "") << "metacarpal stays out of the map";
    EXPECT_EQ(map.targetFor("Chest"), "mixamorig:Spine2");
    EXPECT_EQ(map.targetFor("Head"), "mixamorig:Head");
    EXPECT_EQ(excluded.size(), 24);
    EXPECT_TRUE(unmapped.contains(QStringLiteral("RightHandIndex2"))) << "a joint the rig lacks is reported";
    EXPECT_EQ(map.name, QString::fromLatin1(kSomaMapVersion));
}

namespace {

// A UniRig-style arm: named body bones, then an unnamed palm joint and five unnamed 3-joint fingers under the hand
// (bind offsets copied from a real UniRig officer rig's left hand). `order` lists the finger chains as they appear in
// the hierarchy (0 = thumb .. 4 = pinky), so a test can prove the fallback does not depend on child order.
Retarget::RigDesc uniRigArm(const std::vector<int>& order, int fingers = 5)
{
    Retarget::RigDesc r;
    auto add = [&](const std::string& n, int parent, Ogre::Vector3 t) {
        r.names.push_back(n);
        r.parent.push_back(parent);
        r.bindPos.push_back(t);
        r.bindRot.push_back(Ogre::Quaternion::IDENTITY);
        return int(r.names.size()) - 1;
    };
    const int hips = add("Hips", -1, {0, 1, 0});
    const int spine = add("Spine", hips, {0, 0.2f, 0});
    add("Head", spine, {0, 0.4f, 0});
    const int arm = add("LeftArm", spine, {-0.2f, 0.2f, 0});
    const int fore = add("LeftForeArm", arm, {-0.25f, 0, 0});
    const int hand = add("LeftHand", fore, {-0.106f, -0.012f, 0.008f});
    const int palm = add("joint_9", hand, {-0.137f, 0.004f, -0.004f});
    const Ogre::Vector3 roots[5] = {{-0.016f, 0.016f, -0.012f}, {-0.059f, 0.024f, -0.008f}, {-0.059f, 0.008f, 0},
                                    {-0.063f, -0.012f, 0.004f}, {-0.059f, -0.027f, 0.004f}};
    for (int k = 0; k < fingers; ++k) {
        const int f = order[size_t(k)];
        int j = add("f" + std::to_string(f) + "_a", palm, roots[f]);
        j = add("f" + std::to_string(f) + "_b", j, {-0.016f, 0, -0.008f});
        add("f" + std::to_string(f) + "_c", j, {-0.016f, 0, -0.008f});
    }
    return r;
}

} // namespace

TEST(SomaSkeleton, UnnamedFingerChainsAreFoundFromTheBindPose)
{
    Motion m;
    m.names = soma77();
    const Retarget::RigDesc rig = uniRigArm({3, 0, 4, 1, 2});
    Retarget::BoneMap map = somaBoneMap(m, rig.names);
    ASSERT_EQ(map.targetFor("LeftHand"), "LeftHand");
    ASSERT_EQ(map.targetFor("LeftHandThumb1"), "") << "nothing in the names says finger";

    QStringList notes;
    const QStringList added = addUnnamedFingerChains(m, rig, &map, &notes);
    EXPECT_EQ(added.size(), 15) << notes.join(QLatin1Char('\n')).toStdString();
    EXPECT_EQ(map.targetFor("LeftHandThumb1"), "f0_a");
    EXPECT_EQ(map.targetFor("LeftHandThumb3"), "f0_c");
    EXPECT_EQ(map.targetFor("LeftHandIndex2"), "f1_a") << "SOMA Index2 is the proximal phalanx";
    EXPECT_EQ(map.targetFor("LeftHandMiddle3"), "f2_b");
    EXPECT_EQ(map.targetFor("LeftHandRing4"), "f3_c");
    EXPECT_EQ(map.targetFor("LeftHandPinky2"), "f4_a");
    EXPECT_EQ(map.targetFor("LeftHandIndex1"), "") << "the metacarpal stays out";
    EXPECT_EQ(map.targetFor("RightHandThumb1"), "") << "a hand the rig lacks is skipped";
}

TEST(SomaSkeleton, AHandThatIsNotFiveChainsIsLeftAloneWithAReason)
{
    Motion m;
    m.names = soma77();
    const Retarget::RigDesc rig = uniRigArm({0, 1, 2, 3}, 4);
    Retarget::BoneMap map = somaBoneMap(m, rig.names);
    const size_t before = map.pairs.size();
    QStringList notes;
    EXPECT_TRUE(addUnnamedFingerChains(m, rig, &map, &notes).isEmpty());
    EXPECT_EQ(map.pairs.size(), before);
    ASSERT_EQ(notes.size(), 1);
    EXPECT_TRUE(notes.front().contains(QLatin1String("expected 5")));
}

TEST(SomaSkeleton, NamedFingersAreNeverReinferred)
{
    Motion m;
    m.names = soma77();
    Retarget::RigDesc rig = uniRigArm({0, 1, 2, 3, 4});
    for (std::string& n : rig.names)
        if (n == "f1_a") n = "LeftHandIndex1";
    Retarget::BoneMap map = somaBoneMap(m, rig.names);
    ASSERT_EQ(map.targetFor("LeftHandIndex2"), "LeftHandIndex1");
    EXPECT_TRUE(addUnnamedFingerChains(m, rig, &map).isEmpty());
}

TEST(SomaSkeleton, InPlaceClipsKeepTheRootHeight)
{
    Motion m;
    m.rootPos = {{0.1f, 0.9f, 0.2f}, {0.6f, 1.3f, 1.2f}, {1.1f, 0.5f, 2.2f}};
    m.restJoints = {{0.05f, 0.0f, -0.1f}};
    freezeHorizontalRoot(&m);
    for (const Ogre::Vector3& p : m.rootPos) {
        // The REST root's x/z, not the first frame's: the retargeter measures the root delta from the rest, so a
        // first-frame pin left a constant horizontal offset on the whole in-place clip.
        EXPECT_FLOAT_EQ(p.x, 0.05f);
        EXPECT_FLOAT_EQ(p.z, -0.1f);
    }
    EXPECT_FLOAT_EQ(m.rootPos[1].y, 1.3f) << "a jump still lifts";
    EXPECT_FLOAT_EQ(m.rootPos[2].y, 0.5f) << "a crouch still lowers";
}
