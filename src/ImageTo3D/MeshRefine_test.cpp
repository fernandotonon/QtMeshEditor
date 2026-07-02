#include "MeshRefine.h"
#include "MarchingCubes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

// Unit tests for the post-marching-cubes refinement pass (epic #764 quality
// work). Pure-data — no Ogre, no GL — runs unconditionally on Linux CI.

namespace {

// Deterministic pseudo-random float in [-1,1] (no <random> to keep runs
// bit-identical across platforms).
float prand(uint32_t& state)
{
    state = state * 1664525u + 1013904223u;
    return (float((state >> 8) & 0xffffff) / float(0xffffff)) * 2.0f - 1.0f;
}

// Marching-cubes sphere as a realistic connected test mesh.
MarchingCubes::Mesh sphereMesh(int n = 24, float R = 0.6f)
{
    std::vector<float> g(static_cast<size_t>(n) * n * n);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const float fx = -1.0f + 2.0f * x / (n - 1);
                const float fy = -1.0f + 2.0f * y / (n - 1);
                const float fz = -1.0f + 2.0f * z / (n - 1);
                g[static_cast<size_t>(z) * n * n + static_cast<size_t>(y) * n + x] =
                    R - std::sqrt(fx * fx + fy * fy + fz * fz);
            }
    return MarchingCubes::extract(g.data(), n, n, n, 0.0f, {-1, -1, -1}, {1, 1, 1},
                                  g.size());
}

// Mean + max deviation of |p| from R over all vertices.
void radiusStats(const std::vector<float>& pos, float R, float& meanDev, float& maxDev)
{
    meanDev = 0; maxDev = 0;
    const size_t nv = pos.size() / 3;
    for (size_t v = 0; v < nv; ++v) {
        const float r = std::sqrt(pos[v * 3] * pos[v * 3] +
                                  pos[v * 3 + 1] * pos[v * 3 + 1] +
                                  pos[v * 3 + 2] * pos[v * 3 + 2]);
        const float d = std::fabs(r - R);
        meanDev += d;
        maxDev = std::max(maxDev, d);
    }
    if (nv) meanDev /= float(nv);
}

} // namespace

TEST(MeshRefineTest, TaubinSmoothingReducesNoiseWithoutCollapse)
{
    auto m = sphereMesh();
    ASSERT_GT(m.vertexCount, 100);

    // Add radial noise to every vertex, then smooth it back out.
    const float R = 0.6f, noise = 0.03f;
    std::vector<float> noisy = m.positions;
    uint32_t seed = 42;
    for (size_t v = 0; v < noisy.size() / 3; ++v) {
        const float k = 1.0f + noise * prand(seed) / R;
        noisy[v * 3 + 0] *= k;
        noisy[v * 3 + 1] *= k;
        noisy[v * 3 + 2] *= k;
    }
    float meanBefore, maxBefore;
    radiusStats(noisy, R, meanBefore, maxBefore);

    MeshRefine::taubinSmooth(noisy, m.indices, 6);

    float meanAfter, maxAfter;
    radiusStats(noisy, R, meanAfter, maxAfter);

    // Noise must come down substantially…
    EXPECT_LT(meanAfter, meanBefore * 0.6f)
        << "smoothing should remove high-frequency noise";
    // …and Taubin's λ|μ pair must not shrink the sphere (plain Laplacian
    // smoothing at these iteration counts loses several percent of radius).
    float meanR = 0;
    const size_t nv = noisy.size() / 3;
    for (size_t v = 0; v < nv; ++v)
        meanR += std::sqrt(noisy[v * 3] * noisy[v * 3] +
                           noisy[v * 3 + 1] * noisy[v * 3 + 1] +
                           noisy[v * 3 + 2] * noisy[v * 3 + 2]);
    meanR /= float(nv);
    EXPECT_NEAR(meanR, R, R * 0.03f) << "Taubin must roughly preserve volume";
}

TEST(MeshRefineTest, TaubinIgnoresDegenerateInput)
{
    std::vector<float> pos;                       // empty
    std::vector<uint32_t> idx{0, 1, 2};
    MeshRefine::taubinSmooth(pos, idx, 3);        // must not crash
    EXPECT_TRUE(pos.empty());

    std::vector<float> tri{0, 0, 0, 1, 0, 0, 0, 1, 0};
    std::vector<uint32_t> bad{0, 1, 9};           // out-of-range index
    std::vector<float> copy = tri;
    MeshRefine::taubinSmooth(tri, bad, 3);
    EXPECT_EQ(tri, copy) << "corrupt indices must leave the mesh untouched";
}

TEST(MeshRefineTest, IsoProjectSnapsVerticesOntoAnalyticSphere)
{
    // Field f = R - |p| (inside-positive), gradient ∇f = -p/|p|.
    const float R = 0.6f;
    auto m = sphereMesh();
    ASSERT_GT(m.vertexCount, 100);

    // Push vertices off the surface, then project back with the analytic field.
    std::vector<float> pos = m.positions;
    uint32_t seed = 7;
    for (size_t v = 0; v < pos.size() / 3; ++v) {
        const float k = 1.0f + 0.04f * prand(seed);
        pos[v * 3 + 0] *= k; pos[v * 3 + 1] *= k; pos[v * 3 + 2] *= k;
    }

    const size_t nv = pos.size() / 3;
    std::vector<float> f(nv), grad(nv * 3);
    for (size_t v = 0; v < nv; ++v) {
        const float x = pos[v * 3], y = pos[v * 3 + 1], z = pos[v * 3 + 2];
        const float r = std::sqrt(x * x + y * y + z * z);
        f[v] = R - r;
        grad[v * 3 + 0] = -x / r;
        grad[v * 3 + 1] = -y / r;
        grad[v * 3 + 2] = -z / r;
    }
    float meanBefore, maxBefore;
    radiusStats(pos, R, meanBefore, maxBefore);
    ASSERT_GT(meanBefore, 0.005f);

    MeshRefine::isoProjectStep(pos, f, grad, 0.1f);

    float meanAfter, maxAfter;
    radiusStats(pos, R, meanAfter, maxAfter);
    EXPECT_LT(meanAfter, 1e-4f) << "one Newton step on an exact field lands on it";
    EXPECT_LT(maxAfter, 1e-3f);
}

TEST(MeshRefineTest, IsoProjectClampsStepAndSkipsFlatGradient)
{
    std::vector<float> pos{1, 0, 0};
    std::vector<float> f{10.0f};                  // huge residual
    std::vector<float> grad{1, 0, 0};
    MeshRefine::isoProjectStep(pos, f, grad, 0.05f);
    EXPECT_NEAR(pos[0], 1.0f - 0.05f, 1e-6f) << "step must clamp to maxStep";

    std::vector<float> pos2{1, 2, 3};
    std::vector<float> f2{1.0f};
    std::vector<float> zero{0, 0, 0};             // vanishing gradient
    MeshRefine::isoProjectStep(pos2, f2, zero, 0.05f);
    EXPECT_FLOAT_EQ(pos2[0], 1.0f);
    EXPECT_FLOAT_EQ(pos2[1], 2.0f);
    EXPECT_FLOAT_EQ(pos2[2], 3.0f);

    // Size mismatch → no-op.
    std::vector<float> pos3{1, 0, 0};
    std::vector<float> fBad{1.0f, 2.0f};
    MeshRefine::isoProjectStep(pos3, fBad, grad, 0.05f);
    EXPECT_FLOAT_EQ(pos3[0], 1.0f);
}
