#ifdef ENABLE_MOCAP

#include <gtest/gtest.h>

#include <QImage>

#include <cmath>
#include <vector>

#include "Mocap/FaceCapGeom.h"

using namespace FaceCapGeom;

TEST(FaceCapGeom, AnchorCountsMatchTheModels)
{
    EXPECT_EQ(genSsdAnchors(128, {8, 16, 16, 16}).size(), 896u);   // BlazeFace
    EXPECT_EQ(genSsdAnchors(224, {8, 16, 32, 32, 32}).size(), 2254u);  // BlazePose
}

TEST(FaceCapGeom, LetterboxPadsTheShortSideAndUnmapsBack)
{
    QImage img(200, 100, QImage::Format_RGB888);
    img.fill(Qt::white);
    std::vector<float> tensor(64 * 64 * 3);
    const Letterbox lb = letterboxToTensor(img, 64, 0.f, 1.f, tensor.data());

    EXPECT_NEAR(lb.padX, 0.f, 1e-5);
    EXPECT_NEAR(lb.padY, 0.25f, 1e-5);  // 100/200 -> half height, centred
    EXPECT_NEAR(lb.fracX, 1.f, 1e-5);
    EXPECT_NEAR(lb.fracY, 0.5f, 1e-5);

    // the letterbox bands are zero, the content is white
    auto at = [&](int x, int y) { return tensor[(y * 64 + x) * 3]; };
    EXPECT_FLOAT_EQ(at(32, 2), 0.f);    // top band
    EXPECT_FLOAT_EQ(at(32, 61), 0.f);   // bottom band
    EXPECT_NEAR(at(32, 32), 1.f, 0.02f);  // centre content

    // centre of the letterboxed square unmaps to the image centre
    float x = 0.5f, y = 0.5f;
    unletterbox(lb, x, y);
    EXPECT_NEAR(x, 0.5f, 1e-5);
    EXPECT_NEAR(y, 0.5f, 1e-5);
    // top of the content band unmaps to y = 0
    x = 0.5f;
    y = 0.25f;
    unletterbox(lb, x, y);
    EXPECT_NEAR(y, 0.f, 1e-5);
}

TEST(FaceCapGeom, LetterboxNormalizationRange)
{
    QImage img(10, 10, QImage::Format_RGB888);
    img.fill(Qt::white);
    std::vector<float> tensor(16 * 16 * 3);
    letterboxToTensor(img, 16, -1.f, 1.f, tensor.data());
    float mx = -2.f;
    for (float v : tensor)
        mx = std::max(mx, v);
    EXPECT_NEAR(mx, 1.f, 0.02f);  // white -> +1 in [-1,1]
}

TEST(FaceCapGeom, DecodeDetectionsFindsTheSyntheticBox)
{
    const auto anchors = genSsdAnchors(128, {8, 16, 16, 16});
    std::vector<float> boxes(anchors.size() * 16, 0.f);
    std::vector<float> scores(anchors.size(), -10.f);  // sigmoid ~ 0

    // plant one detection on anchor 100: centred on the anchor, 32px box,
    // keypoints 4px right of centre
    const size_t i = 100;
    scores[i] = 10.f;  // sigmoid ~ 1
    boxes[i * 16 + 2] = 32.f;
    boxes[i * 16 + 3] = 32.f;
    for (int k = 0; k < 6; ++k)
        boxes[i * 16 + 4 + 2 * k] = 4.f;

    const auto dets =
        decodeDetections(boxes.data(), scores.data(), anchors, 128, 6);
    ASSERT_EQ(dets.size(), 1u);
    const auto& d = dets.front();
    EXPECT_NEAR(d.score, 1.f, 1e-3);
    EXPECT_NEAR(d.box[0] + d.box[2] / 2, anchors[i][0], 1e-5);  // centre = anchor
    EXPECT_NEAR(d.box[2], 0.25f, 1e-5);                          // 32/128
    ASSERT_EQ(d.keypoints.size(), 6u);
    EXPECT_NEAR(d.keypoints[0][0], anchors[i][0] + 4.f / 128.f, 1e-5);
}

TEST(FaceCapGeom, WeightedNmsMergesOverlappingCluster)
{
    const auto anchors = genSsdAnchors(128, {8, 16, 16, 16});
    std::vector<float> boxes(anchors.size() * 16, 0.f);
    std::vector<float> scores(anchors.size(), -10.f);
    // two strongly overlapping detections on nearby anchors -> ONE merged out
    for (size_t i : {200u, 201u}) {
        scores[i] = 5.f;
        boxes[i * 16 + 2] = 64.f;
        boxes[i * 16 + 3] = 64.f;
    }
    const auto dets =
        decodeDetections(boxes.data(), scores.data(), anchors, 128, 6);
    EXPECT_EQ(dets.size(), 1u);
}

TEST(FaceCapGeom, FaceRectIsSquareLongAndScaled)
{
    Detection det;
    det.score = 1.f;
    det.box = {0.4f, 0.4f, 0.2f, 0.1f};  // in a 100x100 image: 20 x 10 box
    det.keypoints = {{0.45f, 0.45f}, {0.55f, 0.45f}};  // level eyes
    const RotatedRect r = rectFromFaceDetection(det, 100, 100);
    EXPECT_NEAR(r.cx, 50.f, 1e-3);
    EXPECT_NEAR(r.cy, 45.f, 1e-3);
    EXPECT_NEAR(r.w, 30.f, 1e-3);  // max(20,10) * 1.5
    EXPECT_NEAR(r.h, 30.f, 1e-3);
    EXPECT_NEAR(r.angle, 0.f, 1e-5);  // level eyes -> no rotation
}

TEST(FaceCapGeom, FaceRectRotationFollowsTheEyeLine)
{
    Detection det;
    det.score = 1.f;
    det.box = {0.4f, 0.4f, 0.2f, 0.2f};
    // left eye 45 degrees BELOW the right eye in image coords (y down)
    det.keypoints = {{0.45f, 0.45f}, {0.55f, 0.55f}};
    const RotatedRect r = rectFromFaceDetection(det, 100, 100);
    EXPECT_NEAR(r.angle, static_cast<float>(M_PI) / 4.f, 1e-4);
}

TEST(FaceCapGeom, CropIdentityRoundTrip)
{
    // a gradient image cropped with an axis-aligned rect the size of the
    // image reproduces the image
    QImage img(64, 64, QImage::Format_RGB888);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            img.setPixel(x, y, qRgb(x * 4, y * 4, 0));
    RotatedRect rect;
    rect.cx = 32.f;
    rect.cy = 32.f;
    rect.w = 64.f;
    rect.h = 64.f;
    rect.angle = 0.f;
    std::vector<float> tensor(64 * 64 * 3);
    cropRotatedRectToTensor(img, rect, 64, 0.f, 1.f, tensor.data());
    for (int i : {5, 20, 40, 60}) {
        EXPECT_NEAR(tensor[(i * 64 + i) * 3 + 0], i * 4 / 255.f, 0.03f);
        EXPECT_NEAR(tensor[(i * 64 + i) * 3 + 1], i * 4 / 255.f, 0.03f);
    }
}

TEST(FaceCapGeom, ProjectLandmarksInvertsTheCropMapping)
{
    RotatedRect rect;
    rect.cx = 100.f;
    rect.cy = 80.f;
    rect.w = 50.f;
    rect.h = 50.f;
    rect.angle = 0.5f;

    // crop-space corners + centre with z
    std::vector<float> pts = {
        0.5f, 0.5f, 2.f,   // centre
        0.f, 0.f, 0.f,     // top-left
        1.f, 1.f, 0.f,     // bottom-right
    };
    projectLandmarks(pts.data(), 3, 3, rect);
    EXPECT_NEAR(pts[0], 100.f, 1e-4);  // centre -> rect centre
    EXPECT_NEAR(pts[1], 80.f, 1e-4);
    EXPECT_NEAR(pts[2], 100.f, 1e-4);  // z scaled by rect.w
    // the two corners are diagonal through the centre: their midpoint is it
    EXPECT_NEAR((pts[3] + pts[6]) / 2.f, 100.f, 1e-3);
    EXPECT_NEAR((pts[4] + pts[7]) / 2.f, 80.f, 1e-3);
    // and the diagonal length is |(w, h)| rotated = sqrt(50^2 + 50^2)
    const float dx = pts[6] - pts[3];
    const float dy = pts[7] - pts[4];
    EXPECT_NEAR(std::sqrt(dx * dx + dy * dy), std::sqrt(2.f) * 50.f, 1e-2);
}

TEST(FaceCapGeom, PoseRectFromAlignmentKeypoints)
{
    Detection det;
    det.score = 1.f;
    det.box = {0.f, 0.f, 1.f, 1.f};
    // mid-hip at centre, alignment point straight ABOVE it (y up in image =
    // smaller y): target angle 90 deg means this is rotation 0
    det.keypoints = {{0.5f, 0.5f}, {0.5f, 0.3f}};
    const RotatedRect r = rectFromPoseDetection(det, 100, 100);
    EXPECT_NEAR(r.cx, 50.f, 1e-3);
    EXPECT_NEAR(r.cy, 50.f, 1e-3);
    EXPECT_NEAR(r.w, 2.f * 20.f * 1.25f, 1e-3);  // 2 * radius * 1.25
    EXPECT_NEAR(r.angle, 0.f, 1e-4);
}

#endif  // ENABLE_MOCAP
