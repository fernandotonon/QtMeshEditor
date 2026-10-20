#include <gtest/gtest.h>

#include "SamBodyPredictor.h"

#include <QImage>
#include <QPainter>
#include <cmath>

using namespace SamBody;

// Values below are what Meta's reference pipeline (GetBBoxCenterScale + TopdownAffine + CLIFF
// condition + get_ray_condition) produced for a 1000x1255 photo with the person box
// [140,20,730,1225], captured while verifying the ONNX export.
static CropGeometry reference()
{
    return cropGeometry(1000, 1255, QRectF(140, 20, 590, 1205));
}

TEST(SamBodyCrop, MatchesTheReferencePipeline)
{
    const CropGeometry g = reference();
    ASSERT_TRUE(g.valid);
    EXPECT_NEAR(g.center[0], 435.0, 1e-9);
    EXPECT_NEAR(g.center[1], 622.5, 1e-9);
    EXPECT_NEAR(g.scale, 1506.25, 1e-9);                       // padded 1.25 -> 0.75 aspect -> 1:1
    EXPECT_NEAR(g.affine[0], 0.33991700410842896, 1e-6);
    EXPECT_NEAR(g.affine[2], 108.13610076904297, 1e-3);
    EXPECT_NEAR(g.affine[4], 0.33991700410842896, 1e-6);
    EXPECT_NEAR(g.affine[5], 44.40166091918945, 1e-3);
    EXPECT_NEAR(g.focal, 1604.6884765625, 1e-2);               // sqrt(w^2+h^2)
    EXPECT_NEAR(g.cond[0], -0.04050630331039429, 1e-6);
    EXPECT_NEAR(g.cond[1], -0.0031158695928752422, 1e-6);
    EXPECT_NEAR(g.cond[2], 0.9386557340621948, 1e-6);
}

TEST(SamBodyCrop, WideBoxesGrowTheHeightToo)
{
    // A box wider than person-shaped: the height is raised to w/0.75 first, the crop is still square.
    const CropGeometry g = cropGeometry(800, 600, QRectF(0, 100, 800, 300));
    ASSERT_TRUE(g.valid);
    EXPECT_NEAR(g.scale, 800 * 1.25 / 0.75, 1e-9);   // 0.75 aspect first (height = w/0.75), then 1:1
}

TEST(SamBodyCrop, DegenerateBoxesAreRejected)
{
    EXPECT_FALSE(cropGeometry(100, 100, QRectF(10, 10, 0, 50)).valid);
    EXPECT_FALSE(cropGeometry(100, 100, QRectF(10, 10, 50, -1)).valid);
    EXPECT_FALSE(cropGeometry(0, 100, QRectF(10, 10, 50, 50)).valid);
    EXPECT_FALSE(cropGeometry(100, 100, QRectF(std::nan(""), 0, 50, 50)).valid);
}

TEST(SamBodyRays, MatchTheReferenceCorners)
{
    const CropGeometry g = reference();
    const std::vector<float> r = rayCondition(g);
    ASSERT_EQ(r.size(), static_cast<size_t>(2 * kCropSize * kCropSize));
    const size_t plane = static_cast<size_t>(kCropSize) * kCropSize;
    EXPECT_NEAR(r[0], -0.5098341703414917f, 1e-5);                       // x, pixel (0,0)
    EXPECT_NEAR(r[plane], -0.47244372963905334f, 1e-5);                  // y, pixel (0,0)
    const size_t last = plane - 1;
    EXPECT_NEAR(r[last], 0.42698824405670166f, 1e-5);
    EXPECT_NEAR(r[plane + last], 0.46437868475914f, 1e-5);
    // pixel row 256, column 100
    EXPECT_NEAR(r[256 * kCropSize + 100], -0.3265029788017273f, 1e-5);
    EXPECT_NEAR(r[plane + 256 * kCropSize + 100], -0.0031158695928752422f, 1e-5);
}

TEST(SamBodyWarp, CropsTheBoxAndZeroPadsOutsideThePhoto)
{
    // 200x100 photo: left half red, right half blue. Box = the whole photo.
    QImage img(200, 100, QImage::Format_RGB888);
    img.fill(Qt::red);
    QPainter p(&img);
    p.fillRect(100, 0, 100, 100, Qt::blue);
    p.end();
    const CropGeometry g = cropGeometry(200, 100, QRectF(0, 0, 200, 100), 1.0);
    ASSERT_TRUE(g.valid);
    const std::vector<float> c = warpCrop(img, g);
    const size_t plane = static_cast<size_t>(kCropSize) * kCropSize;
    ASSERT_EQ(c.size(), 3 * plane);
    // The crop is a 200x200 square centred on the photo: rows 0..49 and 150..199 are outside (black).
    auto px = [&](int x, int y, int ch) { return c[ch * plane + static_cast<size_t>(y) * kCropSize + x]; };
    EXPECT_NEAR(px(10, 10, 0) + px(10, 10, 1) + px(10, 10, 2), 0.0f, 1e-6f);        // above the photo
    EXPECT_NEAR(px(100, 256, 0), 1.0f, 1e-3f);                                       // red half
    EXPECT_NEAR(px(100, 256, 2), 0.0f, 1e-3f);
    EXPECT_NEAR(px(400, 256, 2), 1.0f, 1e-3f);                                       // blue half
    EXPECT_NEAR(px(400, 256, 0), 0.0f, 1e-3f);
}

TEST(SamBodyWarp, InvalidInputGivesAZeroBuffer)
{
    const std::vector<float> c = warpCrop(QImage(), CropGeometry{});
    EXPECT_EQ(c.size(), static_cast<size_t>(3 * kCropSize * kCropSize));
    for (float v : c) ASSERT_EQ(v, 0.0f);
}

TEST(SamBodyBounds, FindsTheForeground)
{
    QImage img(50, 40, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    for (int y = 10; y < 30; ++y) for (int x = 5; x < 20; ++x) img.setPixel(x, y, qRgba(1, 2, 3, 255));
    img.setPixel(40, 2, qRgba(0, 0, 0, 40));                   // below the threshold: ignored
    const QRectF b = alphaBounds(img);
    EXPECT_EQ(b, QRectF(5, 10, 15, 20));
    EXPECT_TRUE(alphaBounds(QImage(10, 10, QImage::Format_RGB32)).isEmpty());   // no alpha channel
    QImage empty(10, 10, QImage::Format_ARGB32); empty.fill(Qt::transparent);
    EXPECT_TRUE(alphaBounds(empty).isEmpty());
}

TEST(SamBodyParams, NeutralZeroesOnlyThePose)
{
    std::vector<float> p(kModelParams);
    for (int i = 0; i < kModelParams; ++i) p[i] = static_cast<float>(i + 1);
    const std::vector<float> n = neutralParams(p);
    ASSERT_EQ(n.size(), p.size());
    for (int i = 0; i < kPoseParams; ++i) EXPECT_EQ(n[i], 0.0f);
    for (int i = kPoseParams; i < kModelParams; ++i) EXPECT_EQ(n[i], p[i]);     // scale params kept
}

TEST(SamBodyObj, WritesVerticesAndOneBasedFacesAndTurnsAboutY)
{
    const std::vector<float> v = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    const std::vector<int> t = {0, 1, 2};
    const QByteArray plain = toObj(v, t, false);
    EXPECT_TRUE(plain.contains("v 1 2 3\n"));
    EXPECT_TRUE(plain.contains("f 1 2 3\n"));
    const QByteArray turned = toObj(v, t, true);
    EXPECT_TRUE(turned.contains("v -1 2 -3\n"));        // 180 degrees about Y
    EXPECT_TRUE(turned.contains("v -7 8 -9\n"));
    EXPECT_FALSE(turned.contains(','));                  // never a locale decimal comma
}

TEST(SamBodyModels, AvailabilityMatchesTheBuild)
{
#ifdef ENABLE_ONNX
    EXPECT_TRUE(isAvailable());
#else
    EXPECT_FALSE(isAvailable());
    QString e;
    EXPECT_FALSE(ensureModelsBlocking(&e));
    EXPECT_FALSE(e.isEmpty());
    EXPECT_FALSE(predict(QImage(8, 8, QImage::Format_RGB888)).ok);
#endif
}
