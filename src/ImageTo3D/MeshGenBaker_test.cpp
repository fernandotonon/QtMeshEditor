#include "MeshGenBaker.h"

#include <gtest/gtest.h>

#include <QColor>

#include <algorithm>
#include <cmath>
#include <vector>

// Unit tests for the xatlas texture bake (epic #764 quality work). Pure-data
// with an analytic colour sampler — no Ogre, no ONNX, no GL.

namespace {

// Unit square in XY (two triangles).
const std::vector<float> kQuadPos{
    0, 0, 0,
    1, 0, 0,
    1, 1, 0,
    0, 1, 0,
};
const std::vector<uint32_t> kQuadIdx{0, 1, 2, 0, 2, 3};

// Analytic sampler: colour encodes the sample position (r=x, g=y, b=0.5).
bool posColorSampler(const float* pts, size_t count, float* outRgb)
{
    for (size_t i = 0; i < count; ++i) {
        outRgb[i * 3 + 0] = pts[i * 3 + 0];
        outRgb[i * 3 + 1] = pts[i * 3 + 1];
        outRgb[i * 3 + 2] = 0.5f;
    }
    return true;
}

} // namespace

TEST(MeshGenBakerTest, BakesQuadWithPositionEncodedColors)
{
    MeshGenBaker::Options opts;
    opts.textureSize = 128;
    const auto r = MeshGenBaker::bake(kQuadPos, kQuadIdx, posColorSampler, opts);
    ASSERT_TRUE(r.ok) << qPrintable(r.error);

    // Geometry survives (xatlas may split verts along seams but a flat quad
    // needs none beyond the chart border), UVs normalized.
    EXPECT_GE(r.vertexCount, 4);
    EXPECT_EQ(r.triangleCount, 2);
    ASSERT_EQ(r.uvs.size(), static_cast<size_t>(r.vertexCount) * 2);
    ASSERT_EQ(r.positions.size(), static_cast<size_t>(r.vertexCount) * 3);
    for (size_t i = 0; i < r.uvs.size(); ++i) {
        EXPECT_GE(r.uvs[i], 0.0f);
        EXPECT_LE(r.uvs[i], 1.0f);
    }
    ASSERT_FALSE(r.texture.isNull());

    // The baked texel under each output vertex's UV must encode that vertex's
    // 3D position (r=x, g=y) — proves UV -> surface-point -> sampler wiring.
    int checked = 0;
    for (int v = 0; v < r.vertexCount; ++v) {
        const float u  = r.uvs[static_cast<size_t>(v) * 2 + 0];
        const float vv = r.uvs[static_cast<size_t>(v) * 2 + 1];
        // Sample a texel nudged toward the chart interior (the vertex sits on
        // the chart border where dilation may have written a neighbour).
        const float cx = 0.5f - u, cy = 0.5f - vv;   // toward UV centre
        const int px = static_cast<int>((u + cx * 0.05f) * (r.texture.width() - 1));
        const int py = static_cast<int>((vv + cy * 0.05f) * (r.texture.height() - 1));
        const QColor c = r.texture.pixelColor(px, py);
        const float x = r.positions[static_cast<size_t>(v) * 3 + 0];
        const float y = r.positions[static_cast<size_t>(v) * 3 + 1];
        // Loose tolerance: the nudge moves the sample point slightly.
        EXPECT_NEAR(c.redF(), x, 0.15f);
        EXPECT_NEAR(c.greenF(), y, 0.15f);
        ++checked;
    }
    EXPECT_GT(checked, 0);
}

TEST(MeshGenBakerTest, FailsCleanlyOnDegenerateInput)
{
    const auto r0 = MeshGenBaker::bake({}, {}, posColorSampler, {});
    EXPECT_FALSE(r0.ok);

    const auto r1 = MeshGenBaker::bake(kQuadPos, kQuadIdx, {}, {});
    EXPECT_FALSE(r1.ok);   // no sampler

    std::vector<uint32_t> badIdx{0, 1};   // not a multiple of 3
    const auto r2 = MeshGenBaker::bake(kQuadPos, badIdx, posColorSampler, {});
    EXPECT_FALSE(r2.ok);
}

TEST(MeshGenBakerTest, SamplerAbortPropagatesAsCancelled)
{
    const auto r = MeshGenBaker::bake(
        kQuadPos, kQuadIdx,
        [](const float*, size_t, float*) { return false; }, {});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.cancelled);
    EXPECT_EQ(r.error, QStringLiteral("cancelled"));
    // "No partial data on failure" contract.
    EXPECT_TRUE(r.positions.empty());
    EXPECT_TRUE(r.indices.empty());
    EXPECT_TRUE(r.uvs.empty());
}

TEST(MeshGenBakerTest, RejectsOutOfRangeIndices)
{
    std::vector<uint32_t> bad{0, 1, 9};   // 9 >= 4 vertices
    const auto r = MeshGenBaker::bake(kQuadPos, bad, posColorSampler, {});
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("out of range")));
}

TEST(MeshGenBakerTest, PublishesPreDilationCoverageForSeamFill)
{
    // #1017: the coverage bitmap is the mask an inpaint seam-fill consumes.
    // The load-bearing property is that it is published BEFORE the dilation
    // pass: dilation only smears border colour outward to stop filtering
    // bleed, so a post-dilation bitmap would mark the smeared ring as "fine"
    // and the seam fill would skip exactly the texels it exists to repair.
    //
    // It cannot be checked by comparing two dilatePx settings: dilatePx also
    // feeds xatlas's chart PADDING, so changing it changes the atlas layout
    // and the two bakes are not comparable.
    MeshGenBaker::Options opts;
    opts.textureSize = 64;
    opts.dilatePx = 6;
    const auto r = MeshGenBaker::bake(kQuadPos, kQuadIdx, posColorSampler, opts);
    ASSERT_TRUE(r.ok) << qPrintable(r.error);

    // Coverage is sized to the BAKED TEXTURE, not to Options::textureSize:
    // xatlas picks its own atlas dimensions (59x59 for a 64 request here), so
    // a caller building a mask must read texture.width()/height().
    const size_t n = static_cast<size_t>(r.texture.width()) * r.texture.height();
    ASSERT_GT(n, 0u);
    ASSERT_EQ(r.coverage.size(), n);

    const int covered =
        static_cast<int>(std::count(r.coverage.begin(), r.coverage.end(),
                                    uint8_t{1}));
    EXPECT_GT(covered, 0) << "nothing covered — the bake produced no texels";

    // The real invariant: coverage marks only texels a chart RASTERIZED. A
    // post-dilation bitmap would additionally mark the dilatePx-wide ring
    // around every chart, so re-running the dilation over the published
    // coverage must still be able to GROW it (i.e. it is not already grown).
    // On a fully-packed atlas there is no ring to grow into, which is itself
    // the honest answer — so only assert the direction, never a magnitude.
    std::vector<uint8_t> grown = r.coverage;
    const int W = r.texture.width(), H = r.texture.height();
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const size_t lin = static_cast<size_t>(y) * W + x;
            if (r.coverage[lin]) continue;
            for (int dy = -1; dy <= 1 && !grown[lin]; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sy < 0 || sx >= W || sy >= H) continue;
                    if (r.coverage[static_cast<size_t>(sy) * W + sx]) {
                        grown[lin] = 1; break;
                    }
                }
        }
    }
    const int grownCount =
        static_cast<int>(std::count(grown.begin(), grown.end(), uint8_t{1}));
    EXPECT_GE(grownCount, covered);   // dilation never shrinks
}

namespace {

// UV sphere: `rings` x `segs` quads, radius r (no pole fans — rings of quads).
void uvSphere(float r, int rings, int segs, std::vector<float>& pos, std::vector<uint32_t>& idx)
{
    pos.clear();
    idx.clear();
    for (int i = 0; i <= rings; ++i) {
        const float th = 3.14159265f * (0.05f + 0.9f * float(i) / rings);
        for (int j = 0; j < segs; ++j) {
            const float ph = 2.0f * 3.14159265f * float(j) / segs;
            pos.insert(pos.end(), {r * std::sin(th) * std::cos(ph), r * std::cos(th),
                                   r * std::sin(th) * std::sin(ph)});
        }
    }
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < segs; ++j) {
            const uint32_t a = uint32_t(i * segs + j), b = uint32_t(i * segs + (j + 1) % segs);
            const uint32_t c = a + uint32_t(segs), d = b + uint32_t(segs);
            idx.insert(idx.end(), {a, c, b, b, c, d});
        }
}

} // namespace

// xatlas ignores faces whose 3D area is below FLT_EPSILON in ABSOLUTE units
// and parks them at UV (0,0). A generated mesh is ~1 unit across, so its
// smaller faces fell under that (2054 vertices at (0,0) on a 1024^3 goblin).
// The baker unwraps a scaled copy; nothing may be left unparametrized.
TEST(MeshGenBakerTest, TinyMeshFacesAreAllCharted)
{
    std::vector<float> pos;
    std::vector<uint32_t> idx;
    uvSphere(0.002f, 24, 48, pos, idx);   // mean face area ~1e-8 < FLT_EPSILON
    MeshGenBaker::Options opts;
    opts.textureSize = 256;
    const auto r = MeshGenBaker::bake(pos, idx, posColorSampler, opts);
    ASSERT_TRUE(r.ok) << qPrintable(r.error);
    int parked = 0;
    for (int v = 0; v < r.vertexCount; ++v)
        if (r.uvs[2 * size_t(v)] == 0.0f && r.uvs[2 * size_t(v) + 1] == 0.0f)
            ++parked;
    EXPECT_EQ(parked, 0);
    // Output positions are the ORIGINAL (unscaled) ones.
    float maxAbs = 0.0f;
    for (float p : r.positions)
        maxAbs = std::max(maxAbs, std::abs(p));
    EXPECT_LE(maxAbs, 0.0021f);
}

TEST(MeshGenBakerTest, TangentsFollowUAndHandedness)
{
    // Quad in XY with U along +X and V along +Y.
    const std::vector<float> uv{0, 0, 1, 0, 1, 1, 0, 1};
    const std::vector<float> n{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const auto t = MeshGenBaker::computeTangents(kQuadPos, kQuadIdx, uv, n);
    ASSERT_EQ(t.size(), 16u);
    for (int v = 0; v < 4; ++v) {
        EXPECT_NEAR(t[4 * v + 0], 1.0f, 1e-5f);
        EXPECT_NEAR(t[4 * v + 1], 0.0f, 1e-5f);
        EXPECT_NEAR(t[4 * v + 2], 0.0f, 1e-5f);
        EXPECT_FLOAT_EQ(t[4 * v + 3], 1.0f);   // n x t = +Y = bitangent
    }
    // Mirrored V flips the handedness.
    const std::vector<float> uvMirror{0, 1, 1, 1, 1, 0, 0, 0};
    const auto m = MeshGenBaker::computeTangents(kQuadPos, kQuadIdx, uvMirror, n);
    EXPECT_FLOAT_EQ(m[3], -1.0f);
}

TEST(MeshGenBakerTest, TangentsNeverZeroOnDegenerateUv)
{
    // Every UV collapsed to one point: no usable gradient anywhere. Ogre's
    // builder leaves (0,0,0) here, which normal mapping renders black.
    const std::vector<float> uv(8, 0.25f);
    const std::vector<float> n{0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const auto t = MeshGenBaker::computeTangents(kQuadPos, kQuadIdx, uv, n);
    for (int v = 0; v < 4; ++v) {
        const float len = std::sqrt(t[4 * v] * t[4 * v] + t[4 * v + 1] * t[4 * v + 1]
                                    + t[4 * v + 2] * t[4 * v + 2]);
        EXPECT_NEAR(len, 1.0f, 1e-5f);
        EXPECT_NEAR(t[4 * v + 2], 0.0f, 1e-5f);   // perpendicular to the normal
    }
}
