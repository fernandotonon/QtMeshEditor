#include <gtest/gtest.h>

#include "MouthFit.h"

using namespace CharacterGen;

namespace {

// a closed cube [-1,1]^3 as the skin, wound outwards
Dna::Mesh cube()
{
    Dna::Mesh m;
    m.name = "body";
    for (int i = 0; i < 8; ++i) { m.positions.push_back((i & 1) ? 1.0f : -1.0f); m.positions.push_back((i & 2) ? 1.0f : -1.0f); m.positions.push_back((i & 4) ? 1.0f : -1.0f); }
    const uint32_t quads[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
    for (uint32_t i = 0; i < 8; ++i) m.layouts.push_back({i, 0, i});
    for (const auto& q : quads) m.faces.push_back({q[0], q[1], q[2], q[3]});
    return m;
}

Dna::Mesh points(const char* name, std::vector<float> p)
{
    Dna::Mesh m;
    m.name = name;
    m.positions = std::move(p);
    return m;
}

}  // namespace

TEST(MouthFit, RayParityTellsInsideFromOutside)
{
    const Dna::Mesh c = cube();
    std::vector<uint32_t> tris;
    for (const auto& f : c.faces) for (size_t i = 1; i + 1 < f.size(); ++i) tris.insert(tris.end(), {f[0], f[i], f[i + 1]});
    const float dir[3] = {0, 0, 1};
    const float inside[3] = {0.1f, 0.2f, 0.0f};
    const float outside[3] = {3.0f, 0.2f, 0.0f};
    EXPECT_EQ(MouthFit::rayCrossings(c.positions, tris, inside, dir) & 1, 1);
    EXPECT_EQ(MouthFit::rayCrossings(c.positions, tris, outside, dir) & 1, 0);
}

TEST(MouthFit, CountsTheInteriorVerticesThatAShapePushesThroughTheSkin)
{
    Dna::Rig rig;
    rig.meshes.push_back(cube());
    Dna::Mesh teeth = points("head_teeth", {0, 0, 0,  0.5f, 0.5f, 0.0f});
    Dna::BlendShapeTarget t;
    t.channel = 0;
    t.vertexIndices = {1};
    t.deltas = {0.0f, 4.0f, 0.0f};   // throws the second vertex 4 units up: far outside the cube
    teeth.blendShapeTargets.push_back(t);
    rig.meshes.push_back(teeth);
    rig.blendShapeChannels = QStringList{QStringLiteral("jawOpen")};
    const auto samples = MouthFit::measure(rig, {{QStringLiteral("jawOpen"), 1.0f}});
    ASSERT_EQ(samples.size(), 2u);
    EXPECT_EQ(samples[0].outside, 0);            // rest: both inside
    EXPECT_EQ(samples[1].interiorVertices, 2);
    EXPECT_EQ(samples[1].outside, 1);
    EXPECT_NEAR(samples[1].worst, 3.5f, 0.6f);   // nearest skin vertex of (0.5, 4.5, 0)
}

TEST(MouthFit, NoSkinMeshGivesNoSamples)
{
    Dna::Rig rig;
    EXPECT_TRUE(MouthFit::measure(rig, {}).empty());
}

TEST(MouthFit, ClearanceShiftBringsAnOutsideInteriorBackInsideTheSkin)
{
    const Dna::Mesh skin = cube();   // [-1,1]^3
    // a block of points that straddles the +x wall: some inside, some 0.3 outside
    std::vector<float> interior;
    for (int i = 0; i < 20; ++i) { interior.push_back(0.5f + 0.04f * float(i)); interior.push_back(0.1f * float(i % 5) - 0.2f); interior.push_back(0.07f * float(i % 3)); }
    int before = 0, after = 0;
    const auto shift = MouthFit::clearanceShift(skin, interior, 0.01f, 0.5f, 12, 1, &before, &after, 5.0f);
    EXPECT_GT(before, 0);
    EXPECT_EQ(after, 0);
    EXPECT_LT(shift[0], -0.05f);              // moved toward the head's inside along -x
    // (y and z follow the nearest skin VERTICES, which on a cube are its corners: only the direction through the wall is asserted)
}

TEST(MouthFit, ClearanceShiftLeavesAContainedInteriorAlone)
{
    const Dna::Mesh skin = cube();
    const std::vector<float> interior = {0.1f, 0.1f, 0.2f,  -0.3f, 0.2f, 0.1f,  0.0f, -0.4f, 0.0f};
    int before = -1;
    const auto shift = MouthFit::clearanceShift(skin, interior, 0.01f, 0.5f, 12, 1, &before, nullptr, 5.0f);
    EXPECT_EQ(before, 0);
    EXPECT_EQ(shift[0], 0.0f);
    EXPECT_EQ(shift[1], 0.0f);
    EXPECT_EQ(shift[2], 0.0f);
}

TEST(MouthFit, ClearanceShiftIsCappedPerAxis)
{
    const Dna::Mesh skin = cube();
    const std::vector<float> interior = {3.0f, 0.0f, 0.0f};   // 2 units outside
    const auto shift = MouthFit::clearanceShift(skin, interior, 0.01f, 0.1f, 12, 1, nullptr, nullptr, 5.0f);
    EXPECT_GE(shift[0], -0.1f - 1e-6f);
}

TEST(MouthFit, JawFollowPicksTheFactorThatKeepsTheInteriorInside)
{
    const Dna::Mesh skin = cube();
    const std::vector<float> skinDelta(skin.positions.size(), 0.0f);   // the skin does not move
    // points near the top that the jaw delta pushes UP: a factor of 1 keeps them in (0.6 + 0.3 < 1), 3 throws them out
    std::vector<float> interior, delta;
    for (int i = 0; i < 12; ++i) { interior.insert(interior.end(), {0.0f, 0.6f, 0.1f * float(i % 3)}); delta.insert(delta.end(), {0.0f, 0.3f, 0.0f}); }
    std::vector<int> counts;
    const float chosen = MouthFit::chooseJawFollow(skin, skinDelta, interior, delta, {3.0f, 1.0f, 2.0f}, 3.0f, &counts, 5.0f);
    ASSERT_EQ(counts.size(), 3u);
    EXPECT_GT(counts[0], 0);
    EXPECT_EQ(counts[1], 0);
    EXPECT_EQ(chosen, 1.0f);
}

TEST(MouthFit, JawFollowFallsBackWhenItCannotMeasure)
{
    Dna::Mesh empty;
    EXPECT_EQ(MouthFit::chooseJawFollow(empty, {}, {}, {}, {1.0f, 2.0f}, 1.4f), 1.4f);
    const Dna::Mesh skin = cube();
    EXPECT_EQ(MouthFit::chooseJawFollow(skin, std::vector<float>(skin.positions.size(), 0.0f), {0, 0, 0}, {0, 0, 0}, {}, 1.4f), 1.4f);   // no candidates
}
