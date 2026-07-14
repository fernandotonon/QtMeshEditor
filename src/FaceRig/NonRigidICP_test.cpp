#include <gtest/gtest.h>

#include "FaceRig/NonRigidICP.h"

#include <array>
#include <cmath>
#include <random>
#include <vector>

namespace {

// A small closed-ish grid surface (a bumpy plane) as a stand-in mesh: a
// (n x n) vertex grid triangulated, so it has real edges/faces for NRICP.
struct Grid {
    std::vector<float> V;
    std::vector<int> F;
};

Grid makeGrid(int n, float extent, float bump = 0.0f, float dx = 0.0f)
{
    Grid g;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float fx = (float(x)/(n-1) - 0.5f) * extent + dx;
            const float fy = (float(y)/(n-1) - 0.5f) * extent;
            const float fz = bump * std::sin(float(x)) * std::cos(float(y));
            g.V.insert(g.V.end(), {fx, fy, fz});
        }
    for (int y = 0; y < n-1; ++y)
        for (int x = 0; x < n-1; ++x) {
            const int a = y*n+x, b = y*n+x+1, c = (y+1)*n+x, d = (y+1)*n+x+1;
            g.F.insert(g.F.end(), {a, b, c});
            g.F.insert(g.F.end(), {b, d, c});
        }
    return g;
}

double maxAbs(const std::vector<float>& v)
{
    double m = 0;
    for (float x : v) m = std::max(m, double(std::abs(x)));
    return m;
}

}  // namespace

TEST(NonRigidICP, FitToIdenticalMeshIsNearIdentity)
{
    const Grid g = makeGrid(10, 2.0f);
    const auto res = FaceRig::fit(g.V, g.F, g.V, g.F);
    ASSERT_TRUE(res.ok);
    ASSERT_EQ(res.fitted.size(), g.V.size());
    // fitting a mesh onto ITSELF: fitted verts land on the surface (residual ~0)
    EXPECT_LT(res.meanResidual / res.diag, 0.02);   // < 2% of diag
    // and stay close to their original positions
    double maxMove = 0;
    for (size_t i = 0; i < g.V.size(); ++i)
        maxMove = std::max(maxMove, double(std::abs(res.fitted[i] - g.V[i])));
    EXPECT_LT(maxMove / res.diag, 0.1);
}

TEST(NonRigidICP, FitsOntoTranslatedTarget)
{
    const Grid tmpl = makeGrid(10, 2.0f);
    // user = same surface shifted +0.5 in X (a different "identity")
    Grid user = makeGrid(10, 2.0f, 0.0f, 0.5f);
    const auto res = FaceRig::fit(tmpl.V, tmpl.F, user.V, user.F);
    ASSERT_TRUE(res.ok);
    // the fitted template should end up on the shifted plane (low residual)
    EXPECT_LT(res.meanResidual / res.diag, 0.05);
    // and its mean X should have moved toward the user's (+0.5)
    double mx = 0; int n = int(res.fitted.size()/3);
    for (int i = 0; i < n; ++i) mx += res.fitted[i*3];
    mx /= n;
    EXPECT_GT(mx, 0.2);   // shifted from ~0 toward +0.5
}

TEST(NonRigidICP, FitsOntoDifferentTopologyTarget)
{
    // template 10x10, user 14x14 of the same surface — different vert counts
    const Grid tmpl = makeGrid(10, 2.0f, 0.15f);
    const Grid user = makeGrid(14, 2.0f, 0.15f);
    const auto res = FaceRig::fit(tmpl.V, tmpl.F, user.V, user.F);
    ASSERT_TRUE(res.ok);
    EXPECT_EQ(int(res.fitted.size()/3), int(tmpl.V.size()/3));
    EXPECT_LT(res.meanResidual / res.diag, 0.05);   // fits despite topology gap
    for (float x : res.fitted) EXPECT_TRUE(std::isfinite(x));
}

TEST(NonRigidICP, BoundedOnPerturbedTarget)
{
    const Grid tmpl = makeGrid(10, 2.0f);
    Grid user = makeGrid(10, 2.0f);
    std::mt19937 rng(7);
    std::normal_distribution<float> jitter(0.f, 0.03f);
    for (auto& v : user.V) v += jitter(rng);
    const auto res = FaceRig::fit(tmpl.V, tmpl.F, user.V, user.F);
    ASSERT_TRUE(res.ok);
    // stiffness keeps the fit from chasing every jitter spike
    EXPECT_LT(res.maxResidual / res.diag, 0.2);
    for (float x : res.fitted) EXPECT_TRUE(std::isfinite(x));
}

TEST(NonRigidICP, DegenerateInputDoesNotCrashOrNaN)
{
    const Grid g = makeGrid(6, 1.0f);
    // empty template
    EXPECT_FALSE(FaceRig::fit({}, {}, g.V, g.F).ok);
    // empty user
    EXPECT_FALSE(FaceRig::fit(g.V, g.F, {}, {}).ok);
    // user with a single degenerate (zero-area) triangle
    std::vector<float> uv = {0,0,0, 0,0,0, 0,0,0};
    std::vector<int> uf = {0,1,2};
    const auto res = FaceRig::fit(g.V, g.F, uv, uf);
    // may or may not be "ok" but must never produce NaN
    for (float x : res.fitted) EXPECT_TRUE(std::isfinite(x));
}

TEST(NonRigidICP, ReportsResidualAndDiag)
{
    const Grid g = makeGrid(8, 3.0f);
    const auto res = FaceRig::fit(g.V, g.F, g.V, g.F);
    ASSERT_TRUE(res.ok);
    EXPECT_GT(res.diag, 0.0);
    EXPECT_EQ(int(res.residual.size()), int(g.V.size()/3));
    EXPECT_GE(res.maxResidual, res.meanResidual);
}
