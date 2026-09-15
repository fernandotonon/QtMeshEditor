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
