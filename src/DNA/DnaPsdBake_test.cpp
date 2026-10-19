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
