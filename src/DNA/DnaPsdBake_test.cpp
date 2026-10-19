#include <gtest/gtest.h>

#include "DnaPsdBake.h"

#include <cmath>
#include <random>

using namespace Dna;

namespace {
// A 5x5x5 lattice of points, in metres.
std::vector<std::array<double, 3>> lattice()
{
    std::vector<std::array<double, 3>> p;
    for (int x = 0; x < 5; ++x) for (int y = 0; y < 5; ++y) for (int z = 0; z < 5; ++z)
        p.push_back({0.1 * x - 0.2, 0.1 * y - 0.2, 0.1 * z - 0.2});
    return p;
}
}  // namespace

TEST(DnaPsdTransfer, AUniformScaleScalesEveryOffsetByExactlyThatFactor)
{
    const auto src = lattice();
    std::vector<std::array<double, 3>> dst;
    for (const auto& p : src) dst.push_back({1.25 * p[0], 1.25 * p[1], 1.25 * p[2]});
    QString err;
    const Fit::Warp w = Fit::buildWarp(src, dst, Fit::Options(), &err);
    ASSERT_TRUE(w.valid) << err.toStdString();

    Rig rig;
    rig.meshes.resize(1);
    for (const auto& p : src) for (double c : p) rig.meshes[0].positions.push_back(float(c));
    PoseSpace::Set set;
    set.drivers = {QStringLiteral("elbow")};
    PoseSpace::Sample sm;
    sm.rotations = {PoseSpace::Quat{0.92, 0, 0, 0.38}};
    sm.vertices = {3, 40};
    sm.deltas = {0.004f, -0.002f, 0.001f, 0.0f, 0.003f, -0.001f};
    set.samples.push_back(sm);
    std::vector<PoseSpace::Set> sets{set};
    Psd::transferThroughWarp(sets, rig, w);
    for (size_t i = 0; i < sm.deltas.size(); ++i) EXPECT_NEAR(sets[0].samples[0].deltas[i], 1.25f * sm.deltas[i], 2e-5f) << i;
}

TEST(DnaPsdTransfer, AnInvalidWarpLeavesTheSetsUntouched)
{
    Rig rig;
    rig.meshes.resize(1);
    rig.meshes[0].positions = {0, 0, 0};
    PoseSpace::Set set;
    set.drivers = {QStringLiteral("a")};
    PoseSpace::Sample sm;
    sm.rotations = {PoseSpace::Quat{1, 0, 0, 0}};
    sm.vertices = {0};
    sm.deltas = {1.f, 2.f, 3.f};
    set.samples.push_back(sm);
    std::vector<PoseSpace::Set> sets{set};
    Psd::transferThroughWarp(sets, rig, Fit::Warp());
    EXPECT_EQ(sets[0].samples[0].deltas, sm.deltas);
}

namespace {
PoseSpace::Set oneSample(uint32_t v, float x, float y, float z)
{
    PoseSpace::Set set;
    set.name = QStringLiteral("elbow");
    set.drivers = {QStringLiteral("elbow")};
    PoseSpace::Sample sm;
    sm.rotations = {PoseSpace::Quat{0.92, 0, 0, 0.38}};
    sm.vertices = {v};
    sm.deltas = {x, y, z};
    set.samples.push_back(sm);
    return set;
}
}  // namespace

TEST(DnaPsdLinear, EveryOffsetIsCarriedThroughItsVerticesOwnMatrix)
{
    // two meshes of 2 vertices each; global vertex 3 is mesh 1's second vertex: a 90-degree turn about Y
    // maps (1,0,0) to (0,0,-1)
    std::vector<std::vector<std::array<float, 9>>> lin(2, std::vector<std::array<float, 9>>(2, {1, 0, 0, 0, 1, 0, 0, 0, 1}));
    lin[1][1] = {0, 0, 1, 0, 1, 0, -1, 0, 0};
    std::vector<PoseSpace::Set> sets{oneSample(3, 1.f, 0.f, 0.f), oneSample(0, 1.f, 2.f, 3.f)};
    Psd::applyVertexLinear(sets, lin);
    EXPECT_NEAR(sets[0].samples[0].deltas[0], 0.f, 1e-6f);
    EXPECT_NEAR(sets[0].samples[0].deltas[2], -1.f, 1e-6f);
    EXPECT_NEAR(sets[1].samples[0].deltas[1], 2.f, 1e-6f);   // mesh 0, vertex 0: identity
}

TEST(DnaPsdChannels, SamplesBecomeNamedBlendShapesSplitPerMesh)
{
    Rig rig;
    rig.meshes.resize(2);
    rig.meshes[0].positions.assign(6, 0.f);   // 2 vertices
    rig.meshes[1].positions.assign(6, 0.f);   // 2 vertices
    std::vector<PoseSpace::Set> sets{oneSample(3, 1.f, 2.f, 3.f)};   // mesh 1, vertex 1
    EXPECT_EQ(Psd::appendAsBlendShapes(rig, sets), 1);
    ASSERT_EQ(rig.blendShapeChannels.size(), 1);
    EXPECT_TRUE(PoseSpace::isChannelName(rig.blendShapeChannels[0]));
    EXPECT_TRUE(rig.meshes[0].blendShapeTargets.empty());
    ASSERT_EQ(rig.meshes[1].blendShapeTargets.size(), 1u);
    EXPECT_EQ(rig.meshes[1].blendShapeTargets[0].vertexIndices, std::vector<uint32_t>{1});
    EXPECT_FLOAT_EQ(rig.meshes[1].blendShapeTargets[0].deltas[2], 3.f);
    EXPECT_EQ(Psd::appendAsBlendShapes(rig, sets), 0);   // idempotent: the channel exists
}
