#include <gtest/gtest.h>

#include "FaceRig/DeformationTransfer.h"

#include <cmath>
#include <vector>

namespace {

struct Grid {
    std::vector<float> V;
    std::vector<int> F;
};

// A bumpy plane grid — real triangles for the per-face deformation gradients.
Grid makeGrid(int n, float extent, float bump = 0.0f)
{
    Grid g;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float fx = (float(x)/(n-1) - 0.5f) * extent;
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

double diag(const std::vector<float>& v)
{
    float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
    for (size_t i = 0; i < v.size(); i += 3)
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], v[i+a]);
            hi[a] = std::max(hi[a], v[i+a]);
        }
    double s = 0;
    for (int a = 0; a < 3; ++a) s += double(hi[a]-lo[a])*double(hi[a]-lo[a]);
    return std::sqrt(s);
}

double maxAbs(const std::vector<float>& v)
{
    double m = 0;
    for (float x : v) m = std::max(m, double(std::abs(x)));
    return m;
}

}  // namespace

TEST(DeformationTransfer, RejectsBadInput)
{
    FaceRig::DeformationTransfer dt;
    const Grid g = makeGrid(6, 2.0f);
    EXPECT_FALSE(dt.init({}, {}, {}));                 // empty
    EXPECT_FALSE(dt.init(g.V, g.F, {}));               // fitted count mismatch
    EXPECT_FALSE(dt.valid());
    // valid init
    EXPECT_TRUE(dt.init(g.V, g.F, g.V));
    EXPECT_TRUE(dt.valid());
    EXPECT_EQ(dt.vertexCount(), int(g.V.size()/3));
}

// Identity: fitted == template neutral. Transferring a shape's delta should
// reproduce (approximately) that same delta — the source and target rest
// frames are identical, so the deformation gradients map back to the input.
TEST(DeformationTransfer, IdentityFitReproducesShapeDelta)
{
    const Grid g = makeGrid(10, 2.0f, 0.1f);
    FaceRig::DeformationTransfer dt;
    ASSERT_TRUE(dt.init(g.V, g.F, g.V));

    // a shape: push the centre region up in +Z (like a smile bump)
    const int n = int(g.V.size()/3);
    std::vector<float> delta(g.V.size(), 0.0f);
    for (int i = 0; i < n; ++i) {
        const float x = g.V[size_t(i)*3], y = g.V[size_t(i)*3+1];
        const float bump = 0.15f * std::exp(-(x*x + y*y) * 4.0f);
        delta[size_t(i)*3+2] = bump;
    }
    const auto out = dt.transfer(delta);
    ASSERT_EQ(out.size(), delta.size());
    for (float v : out) ASSERT_TRUE(std::isfinite(v));

    // out should track delta closely (identity transfer). Compare RMS error.
    double num = 0, den = 0;
    for (size_t i = 0; i < delta.size(); ++i) {
        const double d = double(out[i]) - double(delta[i]);
        num += d*d;
        den += double(delta[i])*double(delta[i]);
    }
    const double relRms = std::sqrt(num / std::max(den, 1e-12));
    EXPECT_LT(relRms, 0.15);   // within 15% of the original shape
}

// Winding / orientation preserved: transferring onto a UNIFORMLY SCALED user
// identity should scale the shape delta by the same factor (deformation
// gradients are scale-covariant), and keep signs (no inside-out flip).
TEST(DeformationTransfer, ScaledIdentityScalesTheDelta)
{
    const Grid tmpl = makeGrid(10, 2.0f, 0.1f);
    // fitted = template scaled 2x (a "bigger head")
    std::vector<float> fitted(tmpl.V.size());
    for (size_t i = 0; i < fitted.size(); ++i) fitted[i] = tmpl.V[i] * 2.0f;

    FaceRig::DeformationTransfer dt;
    ASSERT_TRUE(dt.init(tmpl.V, tmpl.F, fitted));

    const int n = int(tmpl.V.size()/3);
    std::vector<float> delta(tmpl.V.size(), 0.0f);
    for (int i = 0; i < n; ++i) {
        const float x = tmpl.V[size_t(i)*3], y = tmpl.V[size_t(i)*3+1];
        delta[size_t(i)*3+2] = 0.12f * std::exp(-(x*x + y*y) * 4.0f);
    }
    const auto out = dt.transfer(delta);
    ASSERT_EQ(out.size(), delta.size());
    for (float v : out) ASSERT_TRUE(std::isfinite(v));

    // on a 2x mesh the same relative deformation should be ~2x the amplitude
    const double dIn = maxAbs(delta);
    const double dOut = maxAbs(out);
    EXPECT_GT(dOut, dIn * 1.3);   // clearly scaled up
    EXPECT_LT(dOut, dIn * 3.0);   // but bounded

    // sign preserved: peak stays +Z (no flip)
    double peak = 0;
    for (int i = 0; i < n; ++i)
        if (std::abs(out[size_t(i)*3+2]) > std::abs(peak))
            peak = out[size_t(i)*3+2];
    EXPECT_GT(peak, 0.0);
}

// Zero shape → zero delta (the neutral maps to the neutral).
TEST(DeformationTransfer, ZeroDeltaProducesZero)
{
    const Grid g = makeGrid(8, 2.0f, 0.1f);
    FaceRig::DeformationTransfer dt;
    ASSERT_TRUE(dt.init(g.V, g.F, g.V));
    std::vector<float> zero(g.V.size(), 0.0f);
    const auto out = dt.transfer(zero);
    ASSERT_EQ(out.size(), zero.size());
    EXPECT_LT(maxAbs(out) / diag(g.V), 1e-3);
}
