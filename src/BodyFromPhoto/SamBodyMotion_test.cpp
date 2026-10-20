#include <gtest/gtest.h>

#include "SamBodyMotion.h"

#include <set>
#include <string>

using namespace SamBody;

TEST(SamBodyMotion, JointTableIsTheMhrSkeleton)
{
    const auto& n = mhrJointNames();
    EXPECT_EQ(n.size(), 127u);
    std::set<std::string> unique;
    for (const char* s : n) { ASSERT_NE(s, nullptr); unique.insert(s); }
    EXPECT_EQ(unique.size(), 127u);                                   // no duplicates, no gaps
    // indices that the neutral-skeleton orientation check and the role table depend on
    EXPECT_EQ(mhrJointIndex("body_world"), 0);
    EXPECT_EQ(mhrJointIndex("root"), 1);
    EXPECT_EQ(mhrJointIndex("l_upleg"), 2);
    EXPECT_EQ(mhrJointIndex("l_foot"), 4);
    EXPECT_EQ(mhrJointIndex("r_upleg"), 18);
    EXPECT_EQ(mhrJointIndex("c_head"), 113);
    EXPECT_EQ(mhrJointIndex("l_pinky0"), 79);
    EXPECT_EQ(mhrJointIndex("l_thumb3"), 99);
    EXPECT_EQ(mhrJointIndex("nope"), -1);
    EXPECT_EQ(mhrJointIndex(nullptr), -1);
}

TEST(SamBodyMotion, EveryRoleSourceExists)
{
    int resolved = 0;
    for (const char* j : roleSourceJoints())
        if (j) { EXPECT_GE(mhrJointIndex(j), 0) << j; ++resolved; }
    EXPECT_EQ(resolved, 19);                                          // Neck1 and both buttocks stay identity by design
}

TEST(SamBodyMotion, RolesTakeTheirJointsRotation)
{
    std::vector<float> q(127 * 4, 0.0f);
    for (int j = 0; j < 127; ++j) q[j * 4 + 3] = 1.0f;
    const int lu = mhrJointIndex("l_uparm");
    q[lu * 4] = 0.0f; q[lu * 4 + 1] = 0.0f; q[lu * 4 + 2] = 0.7071068f; q[lu * 4 + 3] = 0.7071068f;   // 90 deg about Z
    const RoleFrame f = canonicalRoles(q);
    const int LShoulder = 11;
    EXPECT_TRUE(f.resolvedMask & (1u << LShoulder));
    EXPECT_NEAR(f.quats[LShoulder][2], 0.7071068f, 1e-6);
    EXPECT_NEAR(f.quats[LShoulder][3], 0.7071068f, 1e-6);
    const int RShoulder = 7;
    EXPECT_NEAR(f.quats[RShoulder][3], 1.0f, 1e-6);                    // untouched arm
    const int Neck1 = 4, RButtock = 14, LButtock = 18;
    for (int r : {Neck1, RButtock, LButtock}) {
        EXPECT_FALSE(f.resolvedMask & (1u << r));
        EXPECT_EQ(f.quats[r][3], 1.0f);
    }
}

TEST(SamBodyMotion, MalformedInputGivesIdentityRoles)
{
    const RoleFrame f = canonicalRoles(std::vector<float>(10, 0.5f));
    EXPECT_EQ(f.resolvedMask, 0u);
    for (const auto& r : f.quats) { EXPECT_EQ(r[3], 1.0f); EXPECT_EQ(r[0], 0.0f); }
}
