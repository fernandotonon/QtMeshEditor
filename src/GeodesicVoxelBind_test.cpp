#include "GeodesicVoxelBind.h"
#include "SkinWeights.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

// Unit tests for geodesic voxel binding (issue #819, Slice A).
// Pure-data — no Ogre / GL. Fixtures are triangle-soup boxes: the
// voxelization is winding- and connectivity-agnostic (that's the
// point of the method), so a box union stands in for capsules.

namespace {

struct Soup {
    std::vector<float>         positions;   // xyz per vertex
    std::vector<std::uint32_t> indices;     // triangle list

    int vertexCount() const { return int(positions.size() / 3); }

    // Append an axis-aligned box [mn, mx] as 8 verts + 12 tris.
    // `crackedTopFace` replaces the y = mx lid with a detached,
    // slightly inset quad — a genuine non-watertight mesh with a
    // ~0.05-unit rim crack all around (sub-voxel at the resolutions
    // the tests use, so the paper's voxel-scale hole closing must
    // seal it). Removing the whole lid instead would open an
    // 8-voxel hole the exterior flood correctly pours through —
    // that case is the plane/no-volume fallback, not hole closing.
    void addBox(float mnx, float mny, float mnz,
                float mxx, float mxy, float mxz,
                bool crackedTopFace = false)
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
        // Vertex layout: bit0 = x, bit1 = y, bit2 = z.
        auto quad = [&](int a, int b, int c, int d) {
            indices.push_back(base + a); indices.push_back(base + b);
            indices.push_back(base + c);
            indices.push_back(base + a); indices.push_back(base + c);
            indices.push_back(base + d);
        };
        quad(0, 1, 3, 2);   // z = mn
        quad(4, 5, 7, 6);   // z = mx
        quad(0, 1, 5, 4);   // y = mn
        if (!crackedTopFace) {
            quad(2, 3, 7, 6);   // y = mx
        } else {
            const float inset = 0.05f;
            const std::uint32_t b2 = std::uint32_t(vertexCount());
            const float ix0 = mnx + inset, ix1 = mxx - inset;
            const float iz0 = mnz + inset, iz1 = mxz - inset;
            positions.insert(positions.end(), {
                ix0, mxy, iz0,
                ix1, mxy, iz0,
                ix1, mxy, iz1,
                ix0, mxy, iz1,
            });
            indices.insert(indices.end(), { b2, b2 + 1, b2 + 2,
                                            b2, b2 + 2, b2 + 3 });
        }
        quad(0, 2, 6, 4);   // x = mn
        quad(1, 3, 7, 5);   // x = mx
    }
};

// Vertical bone segment centred in x/z at `cx`/`cz`, spanning y.
SkinWeights::BoneSegment vBone(double cx, double y0, double y1, double cz)
{
    return { cx, y0, cz, cx, y1, cz };
}

double weightOnBone(const SkinWeights::VertexWeights& vw, int bone)
{
    for (int i = 0; i < vw.count; ++i)
        if (vw.boneIndices[i] == bone) return vw.weights[i];
    return 0.0;
}

} // namespace

// ─── Two parallel limbs: zero weight crossover ──────────────────────────────

TEST(GeodesicVoxelBindTest, ParallelLimbsHaveZeroCrossover)
{
    // Two vertical 1×4×1 boxes separated by a 0.5-unit gap (≈ 4
    // voxels at resolution 32 — cleanly resolvable). One bone
    // inside each. This is the canonical inner-thighs / hand-near-
    // leg case.
    Soup soup;
    soup.addBox(0.0f, 0.0f, 0.0f, 1.0f, 4.0f, 1.0f);
    soup.addBox(1.5f, 0.0f, 0.0f, 2.5f, 4.0f, 1.0f);

    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(0.5, 0.5, 3.5, 0.5),   // bone 0 in the left box
        vBone(2.0, 0.5, 3.5, 0.5),   // bone 1 in the right box
    };

    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 4;
    opts.maxInfluenceDistance   = 0;   // no cap — isolation must come
                                       // from the geodesic field alone
    opts.voxelResolution        = 32;

    std::vector<SkinWeights::VertexWeights> w;
    const auto res = GeodesicVoxelBind::compute(
        soup.positions.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size(), bones, opts, w);
    ASSERT_TRUE(res.ok) << res.error.toStdString();
    ASSERT_EQ(int(w.size()), soup.vertexCount());
    EXPECT_GT(res.interiorVoxels, 0);

    // Left-box vertices (first 8) must carry ZERO weight on bone 1,
    // right-box vertices (last 8) zero on bone 0 — the boxes are
    // separate solid components, so the distance field cannot cross.
    for (int v = 0; v < 8; ++v) {
        EXPECT_GT(weightOnBone(w[v], 0), 0.99) << "left vertex " << v;
        EXPECT_DOUBLE_EQ(weightOnBone(w[v], 1), 0.0) << "left vertex " << v;
    }
    for (int v = 8; v < 16; ++v) {
        EXPECT_GT(weightOnBone(w[v], 1), 0.99) << "right vertex " << v;
        EXPECT_DOUBLE_EQ(weightOnBone(w[v], 0), 0.0) << "right vertex " << v;
    }
}

TEST(GeodesicVoxelBindTest, InverseDistanceContrastShowsCrossover)
{
    // The SAME fixture through the legacy inverse-distance path DOES
    // bleed across the gap — the contrast assert that motivates the
    // geodesic default (#819).
    Soup soup;
    soup.addBox(0.0f, 0.0f, 0.0f, 1.0f, 4.0f, 1.0f);
    soup.addBox(1.5f, 0.0f, 0.0f, 2.5f, 4.0f, 1.0f);

    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(0.5, 0.5, 3.5, 0.5),
        vBone(2.0, 0.5, 3.5, 0.5),
    };

    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 4;
    opts.maxInfluenceDistance   = 0;

    std::vector<SkinWeights::VertexWeights> w;
    ASSERT_TRUE(SkinWeights::computeWeights(
        soup.positions.data(), soup.vertexCount(), bones, opts, w));

    double maxCross = 0.0;
    for (int v = 0; v < 8; ++v)
        maxCross = std::max(maxCross, weightOnBone(w[v], 1));
    EXPECT_GT(maxCross, 0.01)
        << "inverse-distance unexpectedly produced no crossover — the "
           "contrast fixture no longer demonstrates the geodesic win";
}

// ─── Weights follow the bend, not the chord ─────────────────────────────────

TEST(GeodesicVoxelBindTest, WeightsFollowTheBendNotTheChord)
{
    // U-shape: two vertical arms joined by a base. A vertex at the
    // top of the left arm is Euclidean-close to the right arm's
    // bone, but geodesically far (down the left arm, across the
    // base, up the right arm). With a distance cap that admits the
    // chord but not the path, inverse-distance bleeds and geodesic
    // must not.
    Soup soup;
    soup.addBox(0.0f, 0.0f, 0.0f, 1.0f, 4.0f, 1.0f);   // left arm
    soup.addBox(2.0f, 0.0f, 0.0f, 3.0f, 4.0f, 1.0f);   // right arm
    soup.addBox(0.0f, 0.0f, 0.0f, 3.0f, 1.0f, 1.0f);   // base (overlaps arms)

    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(0.5, 2.5, 3.5, 0.5),   // bone 0: upper left arm
        vBone(2.5, 2.5, 3.5, 0.5),   // bone 1: upper right arm
    };

    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 4;
    // Cap at 0.6 × diagonal (~3.1): the ~2.6-unit chord between the
    // arm tops fits, the ~7-unit geodesic path does not.
    opts.maxInfluenceDistance = 0.6;
    opts.voxelResolution      = 32;

    std::vector<SkinWeights::VertexWeights> w;
    const auto res = GeodesicVoxelBind::compute(
        soup.positions.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size(), bones, opts, w);
    ASSERT_TRUE(res.ok) << res.error.toStdString();

    // Vertex 2 of the first box is (0, 4, 0) — top of the left arm.
    EXPECT_DOUBLE_EQ(weightOnBone(w[2], 1), 0.0)
        << "top-of-left-arm vertex picked up right-arm weight through "
           "the chord";
    EXPECT_GT(weightOnBone(w[2], 0), 0.99);

    // Contrast: inverse-distance with the same cap DOES take the chord.
    std::vector<SkinWeights::VertexWeights> idw;
    ASSERT_TRUE(SkinWeights::computeWeights(
        soup.positions.data(), soup.vertexCount(), bones, opts, idw));
    EXPECT_GT(weightOnBone(idw[2], 1), 0.01);
}

// ─── Non-watertight input matches the closed result ─────────────────────────

TEST(GeodesicVoxelBindTest, NonWatertightMatchesClosedResult)
{
    // Same box, closed vs a cracked (detached, inset) top lid — a
    // genuine non-watertight mesh whose sub-voxel rim gap the
    // voxel-resolution hole closing must seal, classifying the same
    // interior and producing near-identical weights (the paper's
    // headline robustness property).
    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(0.5, 0.0, 2.0, 0.5),   // lower bone
        vBone(0.5, 2.0, 4.0, 0.5),   // upper bone
    };
    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 4;
    opts.maxInfluenceDistance   = 0;
    opts.voxelResolution        = 32;

    Soup closed;
    closed.addBox(0.0f, 0.0f, 0.0f, 1.0f, 4.0f, 1.0f, /*crackedTopFace*/false);
    Soup open;
    open.addBox(0.0f, 0.0f, 0.0f, 1.0f, 4.0f, 1.0f, /*crackedTopFace*/true);

    std::vector<SkinWeights::VertexWeights> wc, wo;
    const auto rc = GeodesicVoxelBind::compute(
        closed.positions.data(), closed.vertexCount(),
        closed.indices.data(), closed.indices.size(), bones, opts, wc);
    const auto ro = GeodesicVoxelBind::compute(
        open.positions.data(), open.vertexCount(),
        open.indices.data(), open.indices.size(), bones, opts, wo);
    ASSERT_TRUE(rc.ok) << rc.error.toStdString();
    ASSERT_TRUE(ro.ok) << ro.error.toStdString();
    EXPECT_GT(ro.interiorVoxels, 0)
        << "hole closing failed — cracked box classified no interior";

    // Compare the 8 shared box-corner vertices (the cracked variant
    // has 4 extra lid verts).
    for (size_t v = 0; v < 8; ++v) {
        for (int b = 0; b < 2; ++b) {
            EXPECT_NEAR(weightOnBone(wc[v], b), weightOnBone(wo[v], b), 0.15)
                << "vertex " << v << " bone " << b;
        }
    }
}

// ─── Degenerate input: planes fall back cleanly ─────────────────────────────

TEST(GeodesicVoxelBindTest, PlaneHasNoInteriorAndFailsGracefully)
{
    // A flat quad encloses no volume — compute must refuse (ok=false)
    // so the caller can fall back to inverse-distance.
    Soup soup;
    soup.positions = {
        0.0f, 0.0f, 0.5f,
        4.0f, 0.0f, 0.5f,
        4.0f, 4.0f, 0.5f,
        0.0f, 4.0f, 0.5f,
    };
    soup.indices = { 0, 1, 2, 0, 2, 3 };

    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(2.0, 0.0, 4.0, 0.5),
    };
    SkinWeightsOptions opts;

    std::vector<SkinWeights::VertexWeights> w;
    const auto res = GeodesicVoxelBind::compute(
        soup.positions.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size(), bones, opts, w);
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.isEmpty());
}

TEST(GeodesicVoxelBindTest, MissingIndicesFailsGracefully)
{
    Soup soup;
    soup.addBox(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(0.5, 0.0, 1.0, 0.5),
    };
    std::vector<SkinWeights::VertexWeights> w;
    const auto res = GeodesicVoxelBind::compute(
        soup.positions.data(), soup.vertexCount(),
        nullptr, 0, bones, {}, w);
    EXPECT_FALSE(res.ok);
}

// ─── The dispatching overload: fallback + info plumbing ─────────────────────

TEST(GeodesicVoxelBindTest, DispatchFallsBackToInverseDistanceOnPlane)
{
    // Through SkinWeights::computeWeights(algo=GeodesicVoxel) the
    // plane must still produce valid weights via the fallback, and
    // the info block must say so.
    std::vector<float> positions = {
        0.0f, 0.0f, 0.5f,
        4.0f, 0.0f, 0.5f,
        4.0f, 4.0f, 0.5f,
        0.0f, 4.0f, 0.5f,
    };
    std::vector<std::uint32_t> indices = { 0, 1, 2, 0, 2, 3 };
    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(2.0, 0.0, 4.0, 0.5),
    };
    SkinWeightsOptions opts;
    opts.maxInfluenceDistance = 0;

    std::vector<SkinWeights::VertexWeights> w;
    SkinWeights::ComputeInfo info;
    ASSERT_TRUE(SkinWeights::computeWeights(
        positions.data(), 4, indices.data(), indices.size(),
        bones, opts, SkinWeights::Algorithm::GeodesicVoxel, w, &info));
    EXPECT_EQ(info.algorithmUsed, QStringLiteral("inverse-distance"));
    EXPECT_FALSE(info.fallbackReason.isEmpty());
    ASSERT_EQ(w.size(), 4u);
    for (const auto& vw : w) {
        EXPECT_GE(vw.count, 1);
        double sum = 0.0;
        for (int i = 0; i < vw.count; ++i) sum += vw.weights[i];
        EXPECT_NEAR(sum, 1.0, 1e-6);
    }
}

TEST(GeodesicVoxelBindTest, DispatchRunsGeodesicOnSolidAndReportsIt)
{
    Soup soup;
    soup.addBox(0.0f, 0.0f, 0.0f, 1.0f, 4.0f, 1.0f);
    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(0.5, 0.5, 3.5, 0.5),
    };
    SkinWeightsOptions opts;
    opts.maxInfluenceDistance = 0;

    std::vector<SkinWeights::VertexWeights> w;
    SkinWeights::ComputeInfo info;
    ASSERT_TRUE(SkinWeights::computeWeights(
        soup.positions.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size(),
        bones, opts, SkinWeights::Algorithm::GeodesicVoxel, w, &info));
    EXPECT_EQ(info.algorithmUsed, QStringLiteral("geodesic-voxel"));
    EXPECT_TRUE(info.fallbackReason.isEmpty());
    EXPECT_EQ(int(info.allowedBones.size()), soup.vertexCount());
}

TEST(GeodesicVoxelBindTest, UniRigRequestFallsBackToGeodesic)
{
    // Slice-C plumbing: until the UniRig skin model is exported and
    // hosted, requesting it must transparently run geodesic-voxel
    // and say so.
    Soup soup;
    soup.addBox(0.0f, 0.0f, 0.0f, 1.0f, 4.0f, 1.0f);
    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(0.5, 0.5, 3.5, 0.5),
    };
    SkinWeightsOptions opts;
    opts.maxInfluenceDistance = 0;

    std::vector<SkinWeights::VertexWeights> w;
    SkinWeights::ComputeInfo info;
    ASSERT_TRUE(SkinWeights::computeWeights(
        soup.positions.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size(),
        bones, opts, SkinWeights::Algorithm::UniRigML, w, &info));
    EXPECT_EQ(info.algorithmUsed, QStringLiteral("geodesic-voxel"));
    EXPECT_TRUE(info.fallbackReason.contains(QStringLiteral("UniRig")));
}

// ─── Bones outside the solid ────────────────────────────────────────────────

TEST(GeodesicVoxelBindTest, FarBoneGetsNoSeedsAndIsReported)
{
    Soup soup;
    soup.addBox(0.0f, 0.0f, 0.0f, 1.0f, 4.0f, 1.0f);
    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(0.5, 0.5, 3.5, 0.5),     // inside
        vBone(50.0, 0.0, 1.0, 0.5),    // far outside any solid…
    };
    // …but inside the padded grid? No — 50 is way past the AABB, the
    // DDA clamps out and the snap radius won't reach. It must land in
    // bonesWithoutSeeds and receive zero weight everywhere.
    SkinWeightsOptions opts;
    opts.maxInfluenceDistance = 0;

    std::vector<SkinWeights::VertexWeights> w;
    const auto res = GeodesicVoxelBind::compute(
        soup.positions.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size(), bones, opts, w);
    ASSERT_TRUE(res.ok) << res.error.toStdString();
    ASSERT_EQ(res.bonesWithoutSeeds.size(), 1u);
    EXPECT_EQ(res.bonesWithoutSeeds[0], 1);
    for (const auto& vw : w)
        EXPECT_DOUBLE_EQ(weightOnBone(vw, 1), 0.0);
}

TEST(GeodesicVoxelBindTest, DistantEndpointBoneStillSeedsThroughTheMesh)
{
    // A bone whose endpoints lie far outside the grid but whose
    // segment passes straight through the box must still seed —
    // the DDA clips the segment to the grid instead of exhausting
    // its step budget marching in from a distant start cell.
    Soup soup;
    soup.addBox(0.0f, 0.0f, 0.0f, 1.0f, 4.0f, 1.0f);
    const std::vector<SkinWeights::BoneSegment> bones = {
        { 0.5, -1000.0, 0.5, 0.5, 1000.0, 0.5 },   // through the core
    };
    SkinWeightsOptions opts;
    opts.maxInfluenceDistance = 0;

    std::vector<SkinWeights::VertexWeights> w;
    const auto res = GeodesicVoxelBind::compute(
        soup.positions.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size(), bones, opts, w);
    ASSERT_TRUE(res.ok) << res.error.toStdString();
    EXPECT_TRUE(res.bonesWithoutSeeds.empty())
        << "distant-endpoint bone lost its seeds — DDA grid clipping broke";
    for (const auto& vw : w)
        EXPECT_NEAR(weightOnBone(vw, 0), 1.0, 1e-9);
}

TEST(GeodesicVoxelBindTest, NonFiniteInputFailsGracefully)
{
    // NaN vertex positions must be rejected before any grid
    // arithmetic (int conversion of non-finite doubles is UB).
    Soup soup;
    soup.addBox(0.0f, 0.0f, 0.0f, 1.0f, 4.0f, 1.0f);
    soup.positions[4] = std::numeric_limits<float>::quiet_NaN();
    const std::vector<SkinWeights::BoneSegment> bones = {
        vBone(0.5, 0.5, 3.5, 0.5),
    };
    std::vector<SkinWeights::VertexWeights> w;
    const auto res = GeodesicVoxelBind::compute(
        soup.positions.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size(), bones, {}, w);
    EXPECT_FALSE(res.ok);
    EXPECT_FALSE(res.error.isEmpty());

    // A non-finite BONE is skipped (reported seedless), not fatal.
    Soup clean;
    clean.addBox(0.0f, 0.0f, 0.0f, 1.0f, 4.0f, 1.0f);
    const std::vector<SkinWeights::BoneSegment> mixedBones = {
        vBone(0.5, 0.5, 3.5, 0.5),
        { std::numeric_limits<double>::quiet_NaN(), 0, 0, 0, 1, 0 },
    };
    SkinWeightsOptions opts;
    opts.maxInfluenceDistance = 0;
    const auto res2 = GeodesicVoxelBind::compute(
        clean.positions.data(), clean.vertexCount(),
        clean.indices.data(), clean.indices.size(), mixedBones, opts, w);
    ASSERT_TRUE(res2.ok) << res2.error.toStdString();
    ASSERT_EQ(res2.bonesWithoutSeeds.size(), 1u);
    EXPECT_EQ(res2.bonesWithoutSeeds[0], 1);
}

// ─── Partition of unity + influence cap ─────────────────────────────────────

TEST(GeodesicVoxelBindTest, WeightsSumToOneAndRespectMaxInfluences)
{
    Soup soup;
    soup.addBox(0.0f, 0.0f, 0.0f, 1.0f, 4.0f, 1.0f);
    std::vector<SkinWeights::BoneSegment> bones;
    for (int i = 0; i < 6; ++i)
        bones.push_back(vBone(0.5, i * 0.6, i * 0.6 + 0.5, 0.5));

    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 2;
    opts.maxInfluenceDistance   = 0;

    std::vector<SkinWeights::VertexWeights> w;
    const auto res = GeodesicVoxelBind::compute(
        soup.positions.data(), soup.vertexCount(),
        soup.indices.data(), soup.indices.size(), bones, opts, w);
    ASSERT_TRUE(res.ok) << res.error.toStdString();
    for (const auto& vw : w) {
        EXPECT_GE(vw.count, 1);
        EXPECT_LE(vw.count, 2);
        double sum = 0.0;
        for (int i = 0; i < vw.count; ++i) {
            EXPECT_GE(vw.weights[i], 0.0);
            sum += vw.weights[i];
        }
        EXPECT_NEAR(sum, 1.0, 1e-6);
    }
}
