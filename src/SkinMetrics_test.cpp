#include "SkinMetrics.h"
#include "GeodesicVoxelBind.h"
#include "SkinWeights.h"
#include "SkinWeightsPost.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

// Slice E (#819): skin-quality metrics + the procedural acceptance
// fixtures. Pure-data — no Ogre / GL. The fixtures mirror the issue's
// acceptance criteria: volume preservation at a 90° elbow bend with
// the Slice-B post-passes ON, zero bleed on the two-limb proximity
// pair (with the inverse-distance contrast), and smoothing reducing
// the Laplacian energy.

namespace {

// M_PI is not portable (MinGW wants _USE_MATH_DEFINES).
constexpr double kPi = 3.14159265358979323846;

// Box soup helper (same construction as GeodesicVoxelBind_test.cpp).
struct Soup {
    std::vector<float>         positions;
    std::vector<std::uint32_t> indices;
    int vertexCount() const { return int(positions.size() / 3); }
    void addBox(float mnx, float mny, float mnz,
                float mxx, float mxy, float mxz)
    {
        const std::uint32_t base = std::uint32_t(vertexCount());
        const float xs[2] = { mnx, mxx };
        const float ys[2] = { mny, mxy };
        const float zs[2] = { mnz, mxz };
        for (int zi = 0; zi < 2; ++zi)
            for (int yi = 0; yi < 2; ++yi)
                for (int xi = 0; xi < 2; ++xi) {
                    positions.push_back(xs[xi]);
                    positions.push_back(ys[yi]);
                    positions.push_back(zs[zi]);
                }
        // Outward winding matters for meshVolume: the divergence-
        // theorem sum needs consistent orientation. Vertex layout:
        // bit0 = x, bit1 = y, bit2 = z.
        auto quad = [&](int a, int b, int c, int d) {
            indices.push_back(base + a); indices.push_back(base + b);
            indices.push_back(base + c);
            indices.push_back(base + a); indices.push_back(base + c);
            indices.push_back(base + d);
        };
        quad(0, 2, 3, 1);   // z = mn (normal -z)
        quad(4, 5, 7, 6);   // z = mx (normal +z)
        quad(0, 1, 5, 4);   // y = mn (normal -y)
        quad(2, 6, 7, 3);   // y = mx (normal +y)
        quad(0, 4, 6, 2);   // x = mn (normal -x)
        quad(1, 3, 7, 5);   // x = mx (normal +x)
    }

    // Welded capsule-like arm tube along y: `segments`+1 octagonal
    // rings (circumradius `radius` about (cx, y, cz)) with shared
    // side walls and fan-capped ends, consistently outward wound.
    // (A stacked-box soup would put coincident interior walls in
    // the volume sum, and a square cross-section pinches harder at
    // its corners than the capsule the acceptance criterion models.)
    void addCapsuleTube(float cx, float cz, float radius,
                        float y0, float y1, int segments)
    {
        constexpr int kRing = 8;
        const std::uint32_t base = std::uint32_t(vertexCount());
        const float step = (y1 - y0) / float(segments);
        for (int r = 0; r <= segments; ++r) {
            const float y = y0 + r * step;
            for (int k = 0; k < kRing; ++k) {
                const double a = 2.0 * kPi * k / kRing;
                positions.insert(positions.end(), {
                    float(cx + radius * std::cos(a)), y,
                    float(cz + radius * std::sin(a)) });
            }
        }
        auto tri = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            indices.insert(indices.end(), { a, b, c });
        };
        // Caps (fans): bottom outward −y, top outward +y.
        const std::uint32_t top = base + std::uint32_t(segments) * kRing;
        for (int k = 1; k + 1 < kRing; ++k) {
            tri(base + 0, base + std::uint32_t(k), base + std::uint32_t(k + 1));
            tri(top + 0, top + std::uint32_t(k + 1), top + std::uint32_t(k));
        }
        // Side walls between consecutive rings.
        for (int r = 0; r < segments; ++r) {
            const std::uint32_t lo = base + std::uint32_t(r) * kRing;
            const std::uint32_t hi = lo + kRing;
            for (int e = 0; e < kRing; ++e) {
                const std::uint32_t i = std::uint32_t(e);
                const std::uint32_t j = std::uint32_t((e + 1) % kRing);
                tri(lo + j, lo + i, hi + i);
                tri(lo + j, hi + i, hi + j);
            }
        }
    }
};

SkinWeights::BoneSegment vBone(double cx, double y0, double y1, double cz)
{
    return { cx, y0, cz, cx, y1, cz };
}

} // namespace

// ─── Metric primitives ──────────────────────────────────────────────────────

TEST(SkinMetricsTest, InfluenceHistogramCountsAndAverage)
{
    std::vector<SkinWeights::VertexWeights> w(4);
    w[0].count = 1; w[1].count = 4; w[2].count = 4; w[3].count = 0;
    const auto h = SkinMetrics::influenceHistogram(w);
    EXPECT_EQ(h.counts[0], 1);
    EXPECT_EQ(h.counts[1], 1);
    EXPECT_EQ(h.counts[4], 2);
    EXPECT_EQ(h.maxInfluences, 4);
    EXPECT_NEAR(h.averageInfluences, 9.0 / 4.0, 1e-12);
}

TEST(SkinMetricsTest, LaplacianEnergyZeroForUniformAndTwoForDisjoint)
{
    // Edge 0-1 with identical weights → 0; edge with disjoint
    // one-bone binds → ||(1,0)-(0,1)||² = 2.
    std::vector<SkinWeights::VertexWeights> same(2);
    same[0].boneIndices[0] = 0; same[0].weights[0] = 1.0; same[0].count = 1;
    same[1] = same[0];
    const std::vector<std::vector<int>> adj = { { 1 }, { 0 } };
    EXPECT_NEAR(SkinMetrics::laplacianEnergy(same, adj), 0.0, 1e-12);

    std::vector<SkinWeights::VertexWeights> disjoint(2);
    disjoint[0].boneIndices[0] = 0; disjoint[0].weights[0] = 1.0;
    disjoint[0].count = 1;
    disjoint[1].boneIndices[0] = 1; disjoint[1].weights[0] = 1.0;
    disjoint[1].count = 1;
    EXPECT_NEAR(SkinMetrics::laplacianEnergy(disjoint, adj), 2.0, 1e-12);

    EXPECT_DOUBLE_EQ(SkinMetrics::laplacianEnergy({}, {}), -1.0);
}

TEST(SkinMetricsTest, RotationAboutKeepsPivotAndRotates)
{
    const double axis[3]  = { 0, 0, 1 };
    const double pivot[3] = { 1, 2, 3 };
    const auto m = SkinMetrics::rotationAbout(axis, pivot, kPi / 2.0);
    auto apply = [&](double x, double y, double z, double out[3]) {
        out[0] = m[0] * x + m[1] * y + m[2]  * z + m[3];
        out[1] = m[4] * x + m[5] * y + m[6]  * z + m[7];
        out[2] = m[8] * x + m[9] * y + m[10] * z + m[11];
    };
    double p[3];
    apply(pivot[0], pivot[1], pivot[2], p);
    EXPECT_NEAR(p[0], pivot[0], 1e-12);
    EXPECT_NEAR(p[1], pivot[1], 1e-12);
    EXPECT_NEAR(p[2], pivot[2], 1e-12);
    // (2,2,3) is +x of the pivot; a +90° z-rotation sends it to +y.
    apply(2, 2, 3, p);
    EXPECT_NEAR(p[0], 1.0, 1e-12);
    EXPECT_NEAR(p[1], 3.0, 1e-12);
    EXPECT_NEAR(p[2], 3.0, 1e-12);
}

TEST(SkinMetricsTest, DeformLbsIdentityAndTranslation)
{
    const std::vector<float> pos = { 0, 0, 0, 1, 1, 1 };
    std::vector<SkinWeights::VertexWeights> w(2);
    w[0].boneIndices[0] = 0; w[0].weights[0] = 1.0; w[0].count = 1;
    // Vertex 1 unweighted → stays put.

    std::vector<SkinMetrics::Transform> bones(1,
        SkinMetrics::identityTransform());
    auto out = SkinMetrics::deformLBS(pos.data(), 2, w, bones);
    ASSERT_EQ(out.size(), 6u);
    for (int i = 0; i < 6; ++i) EXPECT_FLOAT_EQ(out[i], pos[i]);

    bones[0][3] = 5.0;   // translate x by 5
    out = SkinMetrics::deformLBS(pos.data(), 2, w, bones);
    EXPECT_FLOAT_EQ(out[0], 5.0f);
    EXPECT_FLOAT_EQ(out[3], 1.0f);   // unweighted vertex unchanged
}

TEST(SkinMetricsTest, MeshVolumeOfUnitBox)
{
    Soup soup;
    soup.addBox(0, 0, 0, 1, 1, 1);
    EXPECT_NEAR(SkinMetrics::meshVolume(soup.positions.data(),
                                        soup.vertexCount(),
                                        soup.indices.data(),
                                        soup.indices.size()),
                1.0, 1e-9);
}

// ─── Acceptance fixture: 90° elbow bend volume preservation ────────────────

TEST(SkinMetricsTest, ElbowBendPreservesVolumeWithPostPasses)
{
    // Capsule arm (radius 0.5, length 4, 24 rings), two bones
    // (lower/upper), geodesic-voxel weights + Slice-B post-passes,
    // then the upper bone bent 90° about the elbow. Acceptance
    // threshold (issue #819 Slice E): volume ≥ 0.9 of rest with
    // smoothing ON. Verified 0.911 on the reference run (see
    // docs/SKINNING_QUALITY.md).
    Soup soup;
    soup.addCapsuleTube(0.5f, 0.5f, 0.5f, 0.0f, 4.0f, 24);

    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(0.5, 0.0, 2.0, 0.5),   // lower
        vBone(0.5, 2.0, 4.0, 0.5),   // upper
    };
    SkinWeightsOptions opts;
    opts.maxInfluenceDistance = 0;
    opts.voxelResolution      = 32;
    opts.smoothIterations     = 8;   // wide blend zone at the joint

    std::vector<SkinWeights::VertexWeights> weights;
    SkinWeights::ComputeInfo info;
    ASSERT_TRUE(SkinWeights::computeWeights(
        soup.positions.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size(),
        bones, opts, SkinWeights::Algorithm::GeodesicVoxel, weights, &info));
    ASSERT_EQ(info.algorithmUsed, QStringLiteral("geodesic-voxel"));

    // Slice-B post-passes (the acceptance criterion is "with Slice B
    // ON").
    const auto adjacency = SkinWeightsPost::buildAdjacency(
        soup.vertexCount(), soup.indices.data(), soup.indices.size());
    SkinWeightsPost::laplacianSmooth(weights, adjacency,
                                     opts.smoothIterations);
    SkinWeightsPost::pruneAndRenormalize(weights,
                                         opts.maxInfluencesPerVertex);

    // Bend the upper bone 90° about the elbow (pivot y=2), rotating
    // about the z axis.
    const double axis[3]  = { 0, 0, 1 };
    const double pivot[3] = { 0.5, 2.0, 0.5 };
    std::vector<SkinMetrics::Transform> boneTransforms = {
        SkinMetrics::identityTransform(),
        SkinMetrics::rotationAbout(axis, pivot, kPi / 2.0),
    };
    const auto deformed = SkinMetrics::deformLBS(
        soup.positions.data(), soup.vertexCount(), weights, boneTransforms);

    const double restVol = SkinMetrics::meshVolume(
        soup.positions.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size());
    const double bentVol = SkinMetrics::meshVolume(
        deformed.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size());
    ASSERT_GT(restVol, 0.0);

    const double ratio = bentVol / restVol;
    EXPECT_GE(ratio, 0.9)
        << "volume collapsed at the 90° elbow: ratio=" << ratio;
    EXPECT_LE(ratio, 1.1)
        << "volume inflated at the 90° elbow: ratio=" << ratio;
}

// ─── Acceptance fixture: two-limb proximity bleed ───────────────────────────

TEST(SkinMetricsTest, ProximityFixtureBleedZeroForGeodesicPositiveForInverse)
{
    // The Slice-E bleed thresholds: geodesic-voxel weights must show
    // ZERO bleed on the proximity pair; the inverse-distance
    // contrast must show measurable bleed against the same geodesic
    // field.
    Soup soup;
    soup.addBox(0.0f, 0.0f, 0.0f, 1.0f, 4.0f, 1.0f);
    soup.addBox(1.5f, 0.0f, 0.0f, 2.5f, 4.0f, 1.0f);
    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(0.5, 0.5, 3.5, 0.5),
        vBone(2.0, 0.5, 3.5, 0.5),
    };
    SkinWeightsOptions opts;
    opts.maxInfluenceDistance = 0;
    opts.voxelResolution      = 32;

    std::vector<SkinWeights::VertexWeights> gvb;
    std::vector<std::vector<int>> allowed;
    const auto res = GeodesicVoxelBind::compute(
        soup.positions.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size(),
        bones, opts, gvb, &allowed);
    ASSERT_TRUE(res.ok) << res.error.toStdString();

    EXPECT_DOUBLE_EQ(SkinWeightsPost::bleedFraction(gvb, allowed), 0.0);

    std::vector<SkinWeights::VertexWeights> id;
    ASSERT_TRUE(SkinWeights::computeWeights(
        soup.positions.data(), soup.vertexCount(), bones, opts, id));
    EXPECT_GT(SkinWeightsPost::bleedFraction(id, allowed), 0.1)
        << "inverse-distance no longer bleeds — the contrast fixture "
           "lost its meaning";
}

// ─── Post-pass property: smoothing reduces Laplacian energy ─────────────────

TEST(SkinMetricsTest, SmoothingReducesLaplacianEnergy)
{
    Soup soup;
    soup.addCapsuleTube(0.5f, 0.5f, 0.5f, 0.0f, 4.0f, 24);
    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(0.5, 0.0, 2.0, 0.5),
        vBone(0.5, 2.0, 4.0, 0.5),
    };
    SkinWeightsOptions opts;
    opts.maxInfluenceDistance = 0;
    opts.falloff              = 8.0;   // sharp bind → visible banding
    opts.voxelResolution      = 32;

    std::vector<SkinWeights::VertexWeights> weights;
    SkinWeights::ComputeInfo info;
    ASSERT_TRUE(SkinWeights::computeWeights(
        soup.positions.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size(),
        bones, opts, SkinWeights::Algorithm::GeodesicVoxel, weights, &info));

    const auto adjacency = SkinWeightsPost::buildAdjacency(
        soup.vertexCount(), soup.indices.data(), soup.indices.size());
    const double before = SkinMetrics::laplacianEnergy(weights, adjacency);

    auto smoothed = weights;
    SkinWeightsPost::laplacianSmooth(smoothed, adjacency, 3);
    const double after = SkinMetrics::laplacianEnergy(smoothed, adjacency);

    ASSERT_GE(before, 0.0);
    ASSERT_GE(after, 0.0);
    EXPECT_LT(after, before)
        << "Laplacian relaxation failed to reduce the weight-field "
           "energy (before=" << before << " after=" << after << ")";
}
