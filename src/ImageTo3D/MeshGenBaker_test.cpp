#include "MeshGenBaker.h"

#include <gtest/gtest.h>

#include <QColor>

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
    EXPECT_EQ(r.error, QStringLiteral("cancelled"));
}
