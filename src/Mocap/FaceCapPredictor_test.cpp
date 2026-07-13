#ifdef ENABLE_MOCAP

#include <gtest/gtest.h>

#include <QDir>
#include <QImage>

#include "Mocap/FaceCapCanonicalData.h"
#include "Mocap/FaceCapPredictor.h"

// Without models on disk the predictor must degrade gracefully (the
// UniRig/MeshSegmenter pattern): load() false, isAvailable() false, a clear
// lastError, and predict() returning confidence 0 without crashing.
TEST(FaceCapPredictor, GracefulWithoutModels)
{
    FaceCapPredictor p;
    EXPECT_FALSE(p.isAvailable());
    const bool loaded = p.load(QStringLiteral("/nonexistent/model/dir"));
    EXPECT_FALSE(loaded);
    EXPECT_FALSE(p.isAvailable());
    EXPECT_FALSE(p.lastError().isEmpty());

    QImage img(64, 64, QImage::Format_RGB888);
    img.fill(Qt::gray);
    const FaceSample s = p.predict(img, 1.5);
    EXPECT_DOUBLE_EQ(s.timeSec, 1.5);
    EXPECT_FLOAT_EQ(s.confidence, 0.f);
}

TEST(FaceCapPredictor, BlendshapeVocabularyIsStable)
{
    // the shared vocabulary contract: 52 names, _neutral first, jawOpen at 25
    EXPECT_EQ(FaceCap::kBlendshapeCount, 52);
    EXPECT_STREQ(FaceCap::kBlendshapeNames[0], "_neutral");
    EXPECT_STREQ(FaceCap::kBlendshapeNames[25], "jawOpen");
    EXPECT_STREQ(FaceCap::kBlendshapeNames[9], "eyeBlinkLeft");
    EXPECT_STREQ(FaceCap::kBlendshapeNames[44], "mouthSmileLeft");
    EXPECT_EQ(static_cast<int>(FaceCap::kBlendshapeLandmarkSubset.size()), 146);
}

// Real-inference test, env-gated (models are not in CI): set
// QTMESH_MOCAP_MODELS_DIR to a dir containing the three face graphs and
// QTMESH_MOCAP_TEST_IMAGE to a clear frontal-face photo.
TEST(FaceCapPredictor, EnvGatedRealInference)
{
    const QByteArray dir = qgetenv("QTMESH_MOCAP_MODELS_DIR");
    const QByteArray imagePath = qgetenv("QTMESH_MOCAP_TEST_IMAGE");
    if (dir.isEmpty() || imagePath.isEmpty())
        GTEST_SKIP() << "set QTMESH_MOCAP_MODELS_DIR + QTMESH_MOCAP_TEST_IMAGE";

    FaceCapPredictor p;
    ASSERT_TRUE(p.load(QString::fromUtf8(dir))) << p.lastError().toStdString();
    ASSERT_TRUE(p.isAvailable());

    QImage img(QString::fromUtf8(imagePath));
    ASSERT_FALSE(img.isNull());

    const FaceSample s = p.predict(img, 0.0);
    EXPECT_GT(s.confidence, 0.9f);
    for (float w : s.weights) {
        EXPECT_GE(w, 0.f);
        EXPECT_LE(w, 1.f);
    }
    // a unit quaternion came back
    const auto& q = s.headRotation;
    EXPECT_NEAR(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3], 1.f, 1e-3);

    // detector-skip: a second predict on the same frame must NOT re-run the
    // detector (tracking ROI from the first frame's landmarks)
    const int runsAfterFirst = p.detectorRuns();
    const FaceSample s2 = p.predict(img, 1.0 / 30.0);
    EXPECT_GT(s2.confidence, 0.9f);
    EXPECT_EQ(p.detectorRuns(), runsAfterFirst);

    // spot-check values against docs/MOCAP_SPIKE.md's parity run
    for (int i : {9, 25, 44, 45}) {
        printf("  %s = %.4f (first) / %.4f (tracked)\n",
               FaceCap::kBlendshapeNames[i], s.weights[i], s2.weights[i]);
    }
    printf("  headRotation = (%.3f, %.3f, %.3f, %.3f) confidence=%.3f\n",
           q[0], q[1], q[2], q[3], s.confidence);
}

#endif  // ENABLE_MOCAP
