// Pure-helper tests for TripoSplatPredictor. The ONNX pipeline itself is
// covered by C++/Python parity (scripts/verify_triposplat_native.py
// --fixture + QTMESH_TRIPOSPLAT_FIXTURE), which needs the ~6 GB models.

#include <gtest/gtest.h>

#include "TripoSplatPredictor.h"

#include <QImage>

#include <numeric>

TEST(TripoSplatPredictor, TimeScheduleMatchesUpstreamShift)
{
    // t = s*l / (1 + (s-1)*l), l = linspace(1, 0, steps+1), s = 3.
    const auto t = TripoSplatPredictor::timeSchedule(2, 3.0);
    ASSERT_EQ(t.size(), 3u);
    EXPECT_DOUBLE_EQ(t[0], 1.0);
    EXPECT_DOUBLE_EQ(t[1], 0.75);
    EXPECT_DOUBLE_EQ(t[2], 0.0);
    const auto u = TripoSplatPredictor::timeSchedule(4, 1.0);   // shift 1 = uniform
    EXPECT_DOUBLE_EQ(u[1], 0.75);
    EXPECT_DOUBLE_EQ(u[3], 0.25);
}

TEST(TripoSplatPredictor, SystematicCountsSplitTheBudgetExactly)
{
    const float probs[8] = {0.5f, 0.25f, 0.25f, 0, 0, 0, 0, 0};
    int out[8];
    TripoSplatPredictor::systematicCounts(probs, 8, 0.5, out);
    EXPECT_EQ(std::accumulate(out, out + 8, 0), 8);
    // Systematic sampling hits the expectation exactly when it is integral.
    EXPECT_EQ(out[0], 4);
    EXPECT_EQ(out[1], 2);
    EXPECT_EQ(out[2], 2);
    for (int c = 3; c < 8; ++c)
        EXPECT_EQ(out[c], 0);

    // Values cross-checked against upstream sample_probs (positions u/n + k/n
    // searched in the clamped CDF).
    const float p2[8] = {0.1f, 0.2f, 0.3f, 0.4f, 0, 0, 0, 0};
    TripoSplatPredictor::systematicCounts(p2, 3, 0.75, out);
    // positions 0.25, 0.583, 0.917 (clear of the 0.1/0.3/0.6 CDF steps)
    // -> children 1, 2, 3
    EXPECT_EQ(out[0], 0);
    EXPECT_EQ(out[1], 1);
    EXPECT_EQ(out[2], 1);
    EXPECT_EQ(out[3], 1);

    // All-zero probabilities fall back to uniform instead of dropping points.
    const float zero[8] = {};
    TripoSplatPredictor::systematicCounts(zero, 16, 0.0, out);
    EXPECT_EQ(std::accumulate(out, out + 8, 0), 16);
    TripoSplatPredictor::systematicCounts(probs, 0, 0.3, out);
    EXPECT_EQ(std::accumulate(out, out + 8, 0), 0);
}

TEST(TripoSplatPredictor, PrepareImageCentresTheSubjectOnBlack)
{
    // A 400x200 transparent canvas with an opaque white square off-centre.
    QImage img(400, 200, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    for (int y = 40; y < 100; ++y)
        for (int x = 300; x < 360; ++x)
            img.setPixel(x, y, qRgba(255, 255, 255, 255));
    const QImage out = TripoSplatPredictor::prepareImage(img);
    ASSERT_EQ(out.width(), 1024);
    ASSERT_EQ(out.height(), 1024);
    EXPECT_FALSE(out.hasAlphaChannel());
    // Centre is the subject, corners are black background.
    EXPECT_GT(qRed(out.pixel(512, 512)), 200);
    EXPECT_EQ(qRed(out.pixel(5, 5)), 0);
    EXPECT_EQ(qRed(out.pixel(1018, 1018)), 0);
    // The square fills ~1/1.2 of the frame (upstream's 1.2x crop margin).
    int lit = 0;
    for (int x = 0; x < 1024; ++x)
        lit += qRed(out.pixel(x, 512)) > 128 ? 1 : 0;
    EXPECT_NEAR(lit, 1024 / 1.2, 40);
}

TEST(TripoSplatPredictor, ModelFilesListEveryGraphAndItsWeights)
{
    const QStringList files = TripoSplatPredictor::modelFiles();
    EXPECT_EQ(files.size(), 10);
    for (const char* g : {"dinov3", "vae", "flow", "octree", "gs"}) {
        EXPECT_TRUE(files.contains(QStringLiteral("triposplat_%1.onnx").arg(QLatin1String(g))));
        EXPECT_TRUE(files.contains(QStringLiteral("triposplat_%1.onnx.data").arg(QLatin1String(g))));
    }
}
