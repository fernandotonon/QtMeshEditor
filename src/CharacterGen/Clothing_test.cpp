#include <gtest/gtest.h>

#include "Clothing.h"

#include <cmath>

using namespace CharacterGen;

namespace {
// A crude standing figure's joints (metres): the character faces +Z, left is +X, arms hang slightly out.
JointPositions joints()
{
    JointPositions j;
    j[QStringLiteral("Hips")] = {0.0, 0.92, 0.0};
    j[QStringLiteral("Neck")] = {0.0, 1.42, 0.0};
    j[QStringLiteral("LeftArm")] = {0.19, 1.38, 0.0};
    j[QStringLiteral("LeftForeArm")] = {0.38, 1.12, 0.0};
    j[QStringLiteral("LeftHand")] = {0.55, 0.90, 0.0};
    j[QStringLiteral("RightArm")] = {-0.19, 1.38, 0.0};
    j[QStringLiteral("RightForeArm")] = {-0.38, 1.12, 0.0};
    j[QStringLiteral("RightHand")] = {-0.55, 0.90, 0.0};
    j[QStringLiteral("LeftUpLeg")] = {0.09, 0.90, 0.0};
    j[QStringLiteral("LeftLeg")] = {0.10, 0.48, 0.0};
    j[QStringLiteral("LeftFoot")] = {0.10, 0.08, 0.0};
    j[QStringLiteral("RightUpLeg")] = {-0.09, 0.90, 0.0};
    j[QStringLiteral("RightLeg")] = {-0.10, 0.48, 0.0};
    j[QStringLiteral("RightFoot")] = {-0.10, 0.08, 0.0};
    return j;
}

// A flat sheet in the xy plane with UVs and one joint's skin, to cut.
Dna::Mesh sheet()
{
    Dna::Mesh m;
    const int n = 11;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            m.positions.push_back(float(i) / float(n - 1));
            m.positions.push_back(float(j) / float(n - 1));
            m.positions.push_back(0.0f);
            m.uvs.push_back(float(i) / float(n - 1));
            m.uvs.push_back(float(j) / float(n - 1));
            m.skin.push_back({{0, 1.0f}});
        }
    for (uint32_t v = 0; v < uint32_t(n * n); ++v) m.layouts.push_back({v, v, v});
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            const uint32_t a = uint32_t(j * n + i), b = a + 1, c = a + uint32_t(n), d = c + 1;
            m.faces.push_back({a, b, d});
            m.faces.push_back({a, d, c});
        }
    return m;
}
}  // namespace

TEST(Clothing, StyleAndColourParsing)
{
    TopKind t{};
    BottomKind b{};
    EXPECT_TRUE(parseTopStyle("TShirt", &t));
    EXPECT_EQ(t, TopKind::TShirt);
    EXPECT_TRUE(parseTopStyle("longsleeve", &t));
    EXPECT_FALSE(parseTopStyle("cape", &t));
    EXPECT_TRUE(parseBottomStyle("pants", &b));
    EXPECT_EQ(b, BottomKind::Pants);
    EXPECT_FALSE(parseBottomStyle("skirt", &b));
    Rgb c{};
    EXPECT_TRUE(parseClothColour("Navy", &c));
    EXPECT_NEAR(c[2], 90.0f / 255.0f, 1e-4f);
    EXPECT_TRUE(parseClothColour("#ff8000", &c));
    EXPECT_NEAR(c[0], 1.0f, 1e-4f);
    EXPECT_FALSE(parseClothColour("chartreuse-ish", &c));
    EXPECT_FALSE(parseClothColour("#12", &c));
    EXPECT_EQ(topStyleIds().first(), QStringLiteral("none"));
    EXPECT_EQ(bottomStyleIds().size(), 3);
}

TEST(Clothing, ClipCutsExactlyAlongTheZeroContourAndKeepsUvsAndSkin)
{
    const Dna::Mesh body = sheet();
    std::vector<float> field(body.positions.size() / 3);
    for (size_t v = 0; v < field.size(); ++v) field[v] = body.positions[v * 3] - 0.45f;   // keep x >= 0.45 (between grid lines)
    const Dna::Mesh g = clipBodyByField(body, field, 0.01f);
    ASSERT_FALSE(g.faces.empty());
    ASSERT_EQ(g.skin.size(), g.positions.size() / 3);
    ASSERT_EQ(g.normals.size(), g.positions.size());
    float minX = 1e9f;
    for (size_t v = 0; v < g.positions.size() / 3; ++v) minX = std::min(minX, g.positions[v * 3]);
    EXPECT_NEAR(minX, 0.45f, 1e-4f) << "the hem sits exactly on the contour, not on a grid line";
    for (size_t v = 0; v < g.positions.size() / 3; ++v) EXPECT_NEAR(g.positions[v * 3 + 2], 0.01f, 1e-5f) << "pushed along the body normal";
    // UVs follow the (flat) positions: u == x
    for (const auto& l : g.layouts) EXPECT_NEAR(g.uvs[size_t(l.uv) * 2], g.positions[size_t(l.position) * 3], 1e-4f);
    for (const auto& s : g.skin) { ASSERT_FALSE(s.empty()); float w = 0; for (auto& si : s) w += si.weight; EXPECT_NEAR(w, 1.0f, 1e-4f); }
}

TEST(Clothing, AFieldShorterThanThePositionsIsRefused)
{
    const Dna::Mesh body = sheet();
    EXPECT_TRUE(clipBodyByField(body, std::vector<float>(body.positions.size() / 3 - 1, 1.0f), 0.01f).faces.empty());
    EXPECT_FALSE(clipBodyByField(body, std::vector<float>(body.positions.size() / 3, 1.0f), 0.01f).faces.empty());
}

TEST(Clothing, ClipOfNothingIsEmpty)
{
    const Dna::Mesh body = sheet();
    EXPECT_TRUE(clipBodyByField(body, std::vector<float>(body.positions.size() / 3, -1.0f), 0.01f).faces.empty());
    EXPECT_TRUE(clipBodyByField(Dna::Mesh(), {}, 0.01f).faces.empty());
}

TEST(Clothing, TopFieldCoversTheTorsoAndSleevesButNotHeadHandsOrLegs)
{
    const JointPositions j = joints();
    EXPECT_GT(topField(TopKind::TShirt, j, {0.0, 1.2, 0.1}), 0.0f) << "chest";
    EXPECT_GT(topField(TopKind::TShirt, j, {0.0, 1.0, 0.1}), 0.0f) << "belly";
    EXPECT_LT(topField(TopKind::TShirt, j, {0.0, 0.8, 0.1}), 0.0f) << "below the hem";
    EXPECT_LT(topField(TopKind::TShirt, j, {0.0, 1.62, 0.0}), 0.0f) << "head";
    EXPECT_LT(topField(TopKind::TShirt, j, {0.0, 1.52, 0.0}), 0.0f) << "neck";
    EXPECT_LT(topField(TopKind::TShirt, j, {0.55, 0.90, 0.0}), 0.0f) << "hand";
    EXPECT_LT(topField(TopKind::TShirt, j, {0.10, 0.50, 0.0}), 0.0f) << "leg";
    // sleeves: the upper arm is covered by a short sleeve, the forearm only by a long one
    EXPECT_GT(topField(TopKind::TShirt, j, {0.24, 1.30, 0.0}), 0.0f) << "upper arm";
    EXPECT_LT(topField(TopKind::TShirt, j, {0.46, 1.01, 0.0}), 0.0f) << "forearm, short sleeve";
    EXPECT_GT(topField(TopKind::LongSleeve, j, {0.46, 1.01, 0.0}), 0.0f) << "forearm, long sleeve";
    EXPECT_LT(topField(TopKind::Tank, j, {0.24, 1.30, 0.0}), 0.0f) << "no sleeves on a tank top";
    EXPECT_GT(topField(TopKind::Tank, j, {0.0, 1.2, 0.1}), 0.0f);
    EXPECT_LT(topField(TopKind::None, j, {0.0, 1.2, 0.1}), 0.0f);
}

TEST(Clothing, TopCoversHipsWiderThanTheShouldersButNotTheHands)
{
    const JointPositions j = joints();   // shoulders at |x| = 0.19, so the old fixed width limit was 0.235
    for (const TopKind k : {TopKind::TShirt, TopKind::LongSleeve, TopKind::Tank}) {
        EXPECT_GT(topField(k, j, {0.30, 0.95, 0.0}), 0.0f) << "side of a wide hip just above the hem";
        EXPECT_GT(topField(k, j, {-0.30, 0.95, 0.0}), 0.0f) << "other side";
        EXPECT_LT(topField(k, j, {0.55, 0.90, 0.11}), 0.0f) << "splayed thumb beside the wrist";
    }
    EXPECT_LT(topField(TopKind::Tank, j, {0.30, 1.30, 0.0}), 0.0f) << "above the armhole the shoulder limit still applies";

    // fingertips 18 cm past a horizontal forearm's wrist, above the hem: only the extension past the wrist keeps them bare
    JointPositions level = joints();
    level[QStringLiteral("LeftHand")] = {0.60, 1.12, 0.0};
    EXPECT_LT(topField(TopKind::TShirt, level, {0.78, 1.12, 0.0}), 0.0f) << "fingertips past the wrist";

    // an incomplete arm chain on a side keeps the shoulder-width limit there instead of covering everything beside the body
    JointPositions noLeftHand = joints();
    noLeftHand.erase(QStringLiteral("LeftHand"));
    EXPECT_LT(topField(TopKind::TShirt, noLeftHand, {0.30, 0.95, 0.0}), 0.0f) << "missing left wrist";
    EXPECT_GT(topField(TopKind::TShirt, noLeftHand, {-0.30, 0.95, 0.0}), 0.0f) << "complete right arm still relaxes its side";
}

TEST(Clothing, BottomFieldCoversPelvisAndLegsButNotTheTorsoOrArms)
{
    const JointPositions j = joints();
    EXPECT_GT(bottomField(BottomKind::Pants, j, {0.0, 0.92, 0.1}), 0.0f) << "pelvis";
    EXPECT_GT(bottomField(BottomKind::Pants, j, {0.10, 0.30, 0.0}), 0.0f) << "shin";
    EXPECT_LT(bottomField(BottomKind::Pants, j, {0.0, 1.25, 0.1}), 0.0f) << "chest";
    EXPECT_LT(bottomField(BottomKind::Pants, j, {0.0, 1.05, 0.1}), 0.0f) << "above the waistband";
    EXPECT_LT(bottomField(BottomKind::Pants, j, {0.55, 0.90, 0.0}), 0.0f) << "a hand hanging beside the thigh";
    EXPECT_LT(bottomField(BottomKind::Pants, j, {0.10, 0.02, 0.1}), 0.0f) << "below the ankle";
    EXPECT_GT(bottomField(BottomKind::Shorts, j, {0.10, 0.70, 0.0}), 0.0f) << "thigh";
    EXPECT_LT(bottomField(BottomKind::Shorts, j, {0.10, 0.30, 0.0}), 0.0f) << "shin, shorts";
    EXPECT_LT(bottomField(BottomKind::None, j, {0.0, 0.92, 0.1}), 0.0f);
}

TEST(Clothing, MissingJointsMeanNoGarment)
{
    JointPositions j = joints();
    j.erase(QStringLiteral("Neck"));
    EXPECT_LT(topField(TopKind::TShirt, j, {0.0, 1.2, 0.1}), 0.0f);
    Dna::Mesh body = sheet();
    EXPECT_TRUE(buildTop(TopKind::TShirt, body, j).faces.empty());
}

TEST(Clothing, JointWorldPositionsAddUpTheChain)
{
    Dna::Rig rig;
    Dna::Joint root; root.name = QStringLiteral("Hips"); root.parent = -1; root.translation = {0.0, 0.9, 0.0};
    Dna::Joint child; child.name = QStringLiteral("Spine"); child.parent = 0; child.translation = {0.0, 0.1, 0.02};
    Dna::Joint grand; grand.name = QStringLiteral("Neck"); grand.parent = 1; grand.translation = {0.0, 0.3, 0.0};
    rig.joints = {grand, child, root};   // children listed before their parents: parent indices below refer to the new order
    rig.joints[0].parent = 1;
    rig.joints[1].parent = 2;
    rig.joints[2].parent = -1;
    const JointPositions w = jointWorldPositions(rig);
    EXPECT_NEAR(w.at(QStringLiteral("Neck"))[1], 1.3, 1e-9);
    EXPECT_NEAR(w.at(QStringLiteral("Spine"))[2], 0.02, 1e-9);
    EXPECT_NEAR(w.at(QStringLiteral("Hips"))[1], 0.9, 1e-9);
}
