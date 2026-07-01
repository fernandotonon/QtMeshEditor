#include "MeshGenPredictor.h"

#include <gtest/gtest.h>

#include <QFileInfo>
#include <QImage>

#include <cmath>
#include <set>

// Unit tests for MeshGenPredictor (epic #764, slice B #766). The pure-data grid
// builder is tested unconditionally; the ONNX inference path is tested only when
// built with ENABLE_ONNX and the model is present, else GTEST_SKIP (the
// UniRig/PBR convention).

TEST(MeshGenPredictorTest, IsAvailableReflectsOnnxBuild)
{
#ifdef ENABLE_ONNX
    EXPECT_TRUE(MeshGenPredictor::isAvailable());
#else
    EXPECT_FALSE(MeshGenPredictor::isAvailable());
#endif
}

TEST(MeshGenPredictorTest, PredictWithoutModelFailsCleanly)
{
    // No throw, ok=false, informative error — whether or not ONNX is compiled.
    QImage img(64, 64, QImage::Format_RGB888);
    img.fill(Qt::white);
    auto r = MeshGenPredictor::predict(img, "/no/such/encoder.onnx",
                                       "/no/such/decoder.onnx");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
#ifndef ENABLE_ONNX
    EXPECT_TRUE(r.error.contains("ENABLE_ONNX"));
#endif
}

TEST(MeshGenPredictorTest, BuildGridPointsHasCorrectCountAndRange)
{
    const int res = 8;
    const float radius = 0.87f;
    auto pts = MeshGenPredictor::buildGridPoints(res, radius);
    ASSERT_EQ(pts.size(), static_cast<size_t>(res) * res * res * 3);

    // First point is the min corner; last is the max corner.
    EXPECT_NEAR(pts[0], -radius, 1e-5f);
    EXPECT_NEAR(pts[1], -radius, 1e-5f);
    EXPECT_NEAR(pts[2], -radius, 1e-5f);
    const size_t last = pts.size() - 3;
    EXPECT_NEAR(pts[last + 0], radius, 1e-5f);
    EXPECT_NEAR(pts[last + 1], radius, 1e-5f);
    EXPECT_NEAR(pts[last + 2], radius, 1e-5f);

    // No coordinate escapes the box.
    for (float c : pts) {
        EXPECT_GE(c, -radius - 1e-4f);
        EXPECT_LE(c, radius + 1e-4f);
    }
}

TEST(MeshGenPredictorTest, BuildGridPointsIsXFastest)
{
    // Layout must match MarchingCubes' field[z*n*n + y*n + x] (x fastest). So the
    // second point differs from the first ONLY in x.
    const int res = 4;
    const float radius = 1.0f;
    auto pts = MeshGenPredictor::buildGridPoints(res, radius);
    ASSERT_GE(pts.size(), 6u);
    EXPECT_GT(pts[3], pts[0]);            // x advanced
    EXPECT_NEAR(pts[4], pts[1], 1e-6f);   // y unchanged
    EXPECT_NEAR(pts[5], pts[2], 1e-6f);   // z unchanged
    // The (res)th point (index res) starts a new y row: x resets, y advances.
    const size_t row1 = static_cast<size_t>(res) * 3;
    EXPECT_NEAR(pts[row1 + 0], pts[0], 1e-6f);  // x reset to min
    EXPECT_GT(pts[row1 + 1], pts[1]);           // y advanced
}

TEST(MeshGenPredictorTest, BuildGridPointsDegenerateIsSafe)
{
    EXPECT_TRUE(MeshGenPredictor::buildGridPoints(1, 1.0f).empty());
    EXPECT_TRUE(MeshGenPredictor::buildGridPoints(0, 1.0f).empty());
}

// Full inference: only when ONNX + both models are present in the cache.
TEST(MeshGenPredictorTest, InferenceProducesMeshWhenModelPresent)
{
#ifndef ENABLE_ONNX
    GTEST_SKIP() << "built without ENABLE_ONNX";
#else
    const QString enc = MeshGenPredictor::encoderModelPath();
    const QString dec = MeshGenPredictor::decoderModelPath();
    if (!QFileInfo::exists(enc) || !QFileInfo::exists(dec))
        GTEST_SKIP() << "TripoSR models not present in the AppData cache";

    QImage img(256, 256, QImage::Format_RGB888);
    img.fill(Qt::gray);
    MeshGenPredictor::Options o;
    o.sdfResolution = 64;   // small = fast for the test
    auto r = MeshGenPredictor::predict(img, enc, dec, o);
    // A blank/flat image may legitimately produce an EMPTY surface, but any OTHER
    // failure (session load, contract mismatch, decode error) is a regression — so
    // don't let the test pass on an arbitrary error. Accept ok, or ok=false ONLY
    // when it's the documented empty-surface case.
    if (r.ok) {
        EXPECT_GT(r.vertexCount, 0);
        EXPECT_GT(r.triangleCount, 0);
        EXPECT_EQ(static_cast<int>(r.positions.size()), r.vertexCount * 3);
        EXPECT_TRUE(r.usedModel);
    } else {
        EXPECT_TRUE(r.error.contains("empty surface", Qt::CaseInsensitive))
            << "unexpected predict() failure: " << r.error.toStdString();
    }
#endif
}
