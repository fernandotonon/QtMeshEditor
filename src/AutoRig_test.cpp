// Unit tests for AutoRig (#407). The pure-data core (templateJoints /
// fitTemplate) needs no Ogre/GL context, so these run everywhere — unlike
// rigEntity() which needs a loaded mesh (covered by the CLI coverage test
// under Xvfb on CI).

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "AutoRig.h"

namespace {

// Build a synthetic upright "humanoid-ish" point cloud: 2 units tall (y),
// ~1 wide at the shoulders, narrow elsewhere, centred on x/z=0.
std::vector<float> uprightCloud()
{
    std::vector<float> v;
    for (int i = 0; i < 2000; ++i) {
        const float y = (static_cast<float>(i) / 2000.0f) * 2.0f;
        const float w = (y > 1.4f && y < 1.7f) ? 0.9f : 0.35f;  // shoulders bulge
        for (int s = -1; s <= 1; s += 2) {
            v.push_back(s * w * 0.5f);
            v.push_back(y);
            v.push_back(0.0f);
        }
    }
    return v;
}

} // namespace

TEST(AutoRigCore, TemplatesAreNonEmptyAndWellParented)
{
    for (auto t : {AutoRig::Template::Humanoid, AutoRig::Template::Biped,
                   AutoRig::Template::Quadruped, AutoRig::Template::Generic}) {
        const auto js = AutoRig::templateJoints(t);
        ASSERT_FALSE(js.empty());
        // Exactly one root; every non-root parent index is a valid earlier joint.
        int roots = 0;
        for (size_t i = 0; i < js.size(); ++i) {
            if (js[i].parent < 0) { ++roots; continue; }
            EXPECT_GE(js[i].parent, 0);
            EXPECT_LT(static_cast<size_t>(js[i].parent), js.size());
            EXPECT_LT(static_cast<size_t>(js[i].parent), i)
                << "parent must precede child for single-pass bone build";
            // Normalised template coords stay in [0,1].
            for (int a = 0; a < 3; ++a) {
                EXPECT_GE(js[i].pos[a], 0.0);
                EXPECT_LE(js[i].pos[a], 1.0);
            }
        }
        EXPECT_EQ(roots, 1) << "template must have exactly one root";
    }
}

TEST(AutoRigCore, HumanoidHasExpectedBoneCount)
{
    EXPECT_EQ(AutoRig::templateJoints(AutoRig::Template::Humanoid).size(), 19u);
    EXPECT_EQ(AutoRig::templateJoints(AutoRig::Template::Generic).size(), 3u);
}

TEST(AutoRigCore, FitPlacesAllJointsInsideAABB)
{
    const auto cloud = uprightCloud();
    const int n = static_cast<int>(cloud.size() / 3);
    const auto tmpl = AutoRig::templateJoints(AutoRig::Template::Humanoid);

    AutoRig::Options o;
    o.tmpl = AutoRig::Template::Humanoid;
    o.upAxis = 1;
    int recentered = 0;
    const auto placed = AutoRig::fitTemplate(tmpl, cloud.data(), n, o, &recentered);

    ASSERT_EQ(placed.size(), tmpl.size());
    EXPECT_GT(recentered, 0) << "spine/limb-root joints should recentre on a real cloud";

    // Cloud AABB: x in [-0.45, 0.45], y in [0, 2], z == 0.
    for (const auto& j : placed) {
        EXPECT_GE(j.pos[1], -1e-3);
        EXPECT_LE(j.pos[1], 2.0 + 1e-3) << j.name.toStdString() << " y out of AABB";
        EXPECT_GE(j.pos[0], -0.45 - 1e-3);
        EXPECT_LE(j.pos[0],  0.45 + 1e-3) << j.name.toStdString() << " x out of AABB";
    }
}

TEST(AutoRigCore, FitRespectsVerticalOrdering)
{
    const auto cloud = uprightCloud();
    const int n = static_cast<int>(cloud.size() / 3);
    const auto tmpl = AutoRig::templateJoints(AutoRig::Template::Humanoid);
    AutoRig::Options o;
    const auto placed = AutoRig::fitTemplate(tmpl, cloud.data(), n, o, nullptr);

    auto yOf = [&](const QString& name) -> double {
        for (const auto& j : placed) if (j.name == name) return j.pos[1];
        return -1e9;
    };
    // Head above hips above feet.
    EXPECT_GT(yOf("Head"), yOf("Hips"));
    EXPECT_GT(yOf("Hips"), yOf("LeftFoot"));
    EXPECT_GT(yOf("Hips"), yOf("RightFoot"));
    // Symmetric feet stay on opposite sides of centre (x sign preserved).
    EXPECT_GT(yOf("Head"), 1.5);   // head lands in the upper portion
}

TEST(AutoRigCore, FitIsRobustToDegenerateInput)
{
    const auto tmpl = AutoRig::templateJoints(AutoRig::Template::Generic);
    AutoRig::Options o;
    int rc = -1;
    // Null / zero-count → returns the template unchanged, no crash, rc=0.
    const auto p0 = AutoRig::fitTemplate(tmpl, nullptr, 0, o, &rc);
    EXPECT_EQ(p0.size(), tmpl.size());
    EXPECT_EQ(rc, 0);

    // Single degenerate vertex (all same point) → no division blow-up.
    std::vector<float> one = {0.5f, 0.5f, 0.5f};
    const auto p1 = AutoRig::fitTemplate(tmpl, one.data(), 1, o, &rc);
    EXPECT_EQ(p1.size(), tmpl.size());
    for (const auto& j : p1)
        for (int a = 0; a < 3; ++a)
            EXPECT_TRUE(std::isfinite(j.pos[a]));
}

TEST(AutoRigCore, TemplateStringRoundTrip)
{
    using T = AutoRig::Template;
    for (auto t : {T::Humanoid, T::Biped, T::Quadruped, T::Generic})
        EXPECT_EQ(AutoRig::templateFromString(AutoRig::templateToString(t)), t);
    // Unknown → humanoid default; alias "quad".
    EXPECT_EQ(AutoRig::templateFromString("nonsense"), T::Humanoid);
    EXPECT_EQ(AutoRig::templateFromString("quad"), T::Quadruped);
    EXPECT_EQ(AutoRig::templateFromString("HUMANOID"), T::Humanoid);
}

TEST(AutoRigCore, ReportSerialization)
{
    AutoRig::Report r;
    r.applied = true;
    r.meshName = "robot";
    r.templateName = "humanoid";
    r.boneCount = 19;
    r.verticesSampled = 1234;
    r.jointsRecentered = 11;
    const auto j = AutoRig::reportToJson(r);
    EXPECT_TRUE(j["applied"].toBool());
    EXPECT_EQ(j["boneCount"].toInt(), 19);
    EXPECT_EQ(j["template"].toString(), "humanoid");
    EXPECT_FALSE(AutoRig::reportToText(r).isEmpty());

    AutoRig::Report fail;
    fail.applied = false;
    fail.error = "boom";
    EXPECT_TRUE(AutoRig::reportToText(fail).contains("boom"));
}
