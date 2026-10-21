#include <gtest/gtest.h>

#include "DnaPsdBake.h"

#include <cmath>
#include <random>

using namespace Dna;

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
