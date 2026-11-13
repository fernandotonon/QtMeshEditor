#include "ShapeGuide.h"
#include "Trellis2Bake.h"

#include <gtest/gtest.h>

#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {

void bounds(const ShapeGuide::Mesh& m, float mn[3], float mx[3])
{
    for (int k = 0; k < 3; ++k) { mn[k] = 1e30f; mx[k] = -1e30f; }
    for (size_t v = 0; v + 2 < m.positions.size(); v += 3)
        for (int k = 0; k < 3; ++k) {
            mn[k] = std::min(mn[k], m.positions[v + k]);
            mx[k] = std::max(mx[k], m.positions[v + k]);
        }
}

// Signed volume: positive for a closed, outward-wound surface.
double signedVolume(const ShapeGuide::Mesh& m)
{
    double vol = 0.0;
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
        const float* a = &m.positions[m.indices[t] * 3];
        const float* b = &m.positions[m.indices[t + 1] * 3];
        const float* c = &m.positions[m.indices[t + 2] * 3];
        vol += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0])
                + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
    }
    return vol;
}

} // namespace

TEST(ShapeGuide, EveryPresetIsAnOutwardSolidStandingOnTheGround)
{
    ASSERT_GE(ShapeGuide::presets().size(), 10u);
    for (const ShapeGuide::PresetInfo& p : ShapeGuide::presets()) {
        const ShapeGuide::Mesh m = ShapeGuide::preset(p.id);
        ASSERT_FALSE(m.empty()) << p.id.toStdString();
        float mn[3], mx[3];
        bounds(m, mn, mx);
        EXPECT_NEAR(mn[1], 0.0f, 1e-4f) << p.id.toStdString() << " must stand on y = 0";
        // Outward winding: every part is a closed outward solid, so the sum of
        // their volumes is positive (an inside-out box would subtract).
        EXPECT_GT(signedVolume(m), 0.0) << p.id.toStdString();
        for (uint32_t i : m.indices)
            ASSERT_LT(i, m.positions.size() / 3);
    }
    EXPECT_TRUE(ShapeGuide::preset(QStringLiteral("no-such-preset")).empty());
    EXPECT_FALSE(ShapeGuide::isPreset(QStringLiteral("no-such-preset")));
}

TEST(ShapeGuide, VehiclesFaceForwardAlongZ)
{
    for (const char* id : {"car", "van", "bus", "truck"}) {
        float mn[3], mx[3];
        bounds(ShapeGuide::preset(QString::fromLatin1(id)), mn, mx);
        EXPECT_GT(mx[2] - mn[2], 1.5f * (mx[0] - mn[0])) << id << " length runs along Z";
    }
}

TEST(ShapeGuide, GridFrameIsZUpAndFillsTheExtent)
{
    // A 2 x 4 x 1 box standing at x = 10: its height (editor Y) must become
    // grid Z, and the 4 m side must span exactly the 0.95 extent.
    ShapeGuide::Mesh box;
    box.positions = {10, 0, 0, 12, 0, 0, 12, 4, 0, 10, 4, 1};
    box.indices = {0, 1, 2, 0, 2, 3};
    const ShapeGuide::GridPlacement p = ShapeGuide::placementFor(box);
    EXPECT_NEAR(p.scale, 0.95f / 4.0f, 1e-6f);
    EXPECT_NEAR(p.centre[0], 11.0f, 1e-6f);
    const ShapeGuide::Mesh g = ShapeGuide::toGrid(box, p);
    float mn[3], mx[3];
    bounds(g, mn, mx);
    EXPECT_NEAR(mx[2] - mn[2], 0.95f, 1e-5f);              // height -> grid z
    EXPECT_NEAR(mx[0] - mn[0], 2.0f * p.scale, 1e-5f);      // width  -> grid x
    EXPECT_NEAR(mx[1] - mn[1], 1.0f * p.scale, 1e-5f);      // depth  -> grid y
    // Editor +X is grid -X.
    EXPECT_LT(g.positions[3], g.positions[0]);
    // The direction helpers invert each other.
    const float e[3] = {0.3f, -0.2f, 0.7f};
    float gg[3], back[3];
    ShapeGuide::editorToGridDir(e, gg);
    ShapeGuide::gridToEditorDir(gg, back);
    for (int k = 0; k < 3; ++k) EXPECT_FLOAT_EQ(back[k], e[k]);
}

TEST(ShapeGuide, VoxelizedSurfaceIsHollowAndCoversTheFaces)
{
    const ShapeGuide::Mesh m = ShapeGuide::preset(QStringLiteral("bus"));
    const ShapeGuide::Mesh g = ShapeGuide::toGrid(m, ShapeGuide::placementFor(m));
    const std::vector<uint8_t> occ = ShapeGuide::voxelizeSurface(g, 64);
    ASSERT_EQ(occ.size(), 64u * 64u * 64u);
    const size_t n = std::accumulate(occ.begin(), occ.end(), size_t(0));
    EXPECT_GT(n, 2000u);                 // a bus side alone is ~60 x 15 voxels
    EXPECT_LT(n, occ.size() / 4);        // surface, not a filled solid
    // The bus is centred: the centre voxel is inside the body, not on a face.
    EXPECT_EQ(occ[(32 * 64 + 32) * 64 + 32], 0);
}

TEST(ShapeGuide, ObjRoundTrip)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("house.obj"));
    const ShapeGuide::Mesh m = ShapeGuide::preset(QStringLiteral("house"));
    ASSERT_TRUE(ShapeGuide::writeObj(path, m, QStringLiteral("house")));
    ShapeGuide::Mesh back;
    QString err;
    ASSERT_TRUE(ShapeGuide::readObj(path, back, &err)) << err.toStdString();
    EXPECT_EQ(back.indices, m.indices);
    ASSERT_EQ(back.positions.size(), m.positions.size());
    for (size_t i = 0; i < m.positions.size(); ++i)
        EXPECT_NEAR(back.positions[i], m.positions[i], 1e-5f);
    EXPECT_FALSE(ShapeGuide::readObj(dir.filePath(QStringLiteral("missing.obj")), back));
}

TEST(ShapeGuide, FitRecoversScaleAndOffsetOfTheGeneratedSurface)
{
    // The "generated" surface is the guide shrunk by 0.9 and shifted: the
    // trimmed ICP must find that placement.
    const ShapeGuide::Mesh m = ShapeGuide::preset(QStringLiteral("house"));
    ShapeGuide::Mesh src = m;
    for (size_t v = 0; v + 2 < src.positions.size(); v += 3) {
        src.positions[v] = 0.9f * src.positions[v] + 0.3f;
        src.positions[v + 1] = 0.9f * src.positions[v + 1] - 0.2f;
        src.positions[v + 2] = 0.9f * src.positions[v + 2] + 0.1f;
    }
    const Trellis2Bake::TargetFit fit = Trellis2Bake::fitTargetToSource(
        m.positions, m.indices, src.positions, src.indices, 80, 0.1f);
    ASSERT_TRUE(fit.ok);
    EXPECT_NEAR(fit.scale, 0.9f, 0.02f);
    EXPECT_NEAR(fit.offset[0], 0.3f, 0.08f);
    EXPECT_NEAR(fit.offset[1], -0.2f, 0.08f);
    EXPECT_NEAR(fit.offset[2], 0.1f, 0.08f);
    EXPECT_LT(fit.meanDistanceAfter, fit.meanDistanceBefore);
}

TEST(ShapeGuide, EncoderReportsMissingModelInsteadOfCrashing)
{
    std::vector<float> latent;
    QString err;
    EXPECT_FALSE(ShapeGuide::encode(std::vector<uint8_t>(64 * 64 * 64, 0),
                                    QStringLiteral("/nonexistent/ss_encoder.onnx"), latent, &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(ShapeGuide::encode(std::vector<uint8_t>(10, 0), QString(), latent, &err));
}

TEST(ShapeGuide, FitFindsTheTurnOfAThreeQuarterGeneration)
{
    // The generation of a three-quarter photo comes back turned about the up
    // axis (grid +Z). The fit must find that turn, not squash the guide.
    const ShapeGuide::Mesh ed = ShapeGuide::preset(QStringLiteral("van"));
    const ShapeGuide::Mesh m = ShapeGuide::toGrid(ed, ShapeGuide::placementFor(ed));
    const float yaw = 35.0f * 3.14159265f / 180.0f;
    const float c = std::cos(yaw), sn = std::sin(yaw);
    ShapeGuide::Mesh src = m;
    for (size_t v = 0; v + 2 < src.positions.size(); v += 3) {
        const float x = src.positions[v], y = src.positions[v + 1];
        src.positions[v] = 0.95f * (c * x - sn * y) + 0.02f;
        src.positions[v + 1] = 0.95f * (sn * x + c * y);
        src.positions[v + 2] = 0.95f * src.positions[v + 2];
    }
    const Trellis2Bake::TargetFit fit = Trellis2Bake::fitTargetToSource(
        m.positions, m.indices, src.positions, src.indices);
    ASSERT_TRUE(fit.ok);
    EXPECT_NEAR(fit.yaw * 180.0f / 3.14159265f, 35.0f, 3.0f);
    EXPECT_NEAR(fit.scale, 0.95f, 0.03f);
    // An unturned generation keeps the authored orientation.
    const Trellis2Bake::TargetFit still = Trellis2Bake::fitTargetToSource(
        m.positions, m.indices, m.positions, m.indices);
    EXPECT_NEAR(still.yaw, 0.0f, 1e-6f);
}
