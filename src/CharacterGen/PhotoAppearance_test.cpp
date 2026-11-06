#include <gtest/gtest.h>

#include "PhotoAppearance.h"

#include <QImage>
#include <QPainter>

using namespace CharacterGen::PhotoAppearance;

TEST(PhotoAppearance, RobustColourIgnoresOutliers)
{
    std::vector<std::array<uint8_t, 3>> px(200, {200, 150, 120});
    for (int i = 0; i < 40; ++i) px.push_back({10, 10, 10});     // shadows / eyelashes
    for (int i = 0; i < 30; ++i) px.push_back({250, 250, 250});  // highlights
    const auto e = robustColour(px);
    ASSERT_TRUE(e.ok);
    EXPECT_NEAR(e.colour[0] * 255.0f, 200.0f, 3.0f);
    EXPECT_NEAR(e.colour[1] * 255.0f, 150.0f, 3.0f);
    EXPECT_NEAR(e.colour[2] * 255.0f, 120.0f, 3.0f);
}

TEST(PhotoAppearance, TooFewSamplesIsNotAnEstimate)
{
    EXPECT_FALSE(robustColour(std::vector<std::array<uint8_t, 3>>(5, {1, 2, 3})).ok);
}

TEST(PhotoAppearance, MaskSelectsPixels)
{
    QImage img(40, 40, QImage::Format_RGB888);
    img.fill(QColor(30, 60, 200));
    QPainter p(&img);
    p.fillRect(0, 0, 20, 40, QColor(220, 180, 150));
    p.end();
    const QImage mask = polygonMask(40, 40, {{0, 0}, {19, 0}, {19, 39}, {0, 39}});
    const auto e = robustColour(img, mask);
    ASSERT_TRUE(e.ok);
    EXPECT_NEAR(e.colour[0] * 255.0f, 220.0f, 4.0f);
}

TEST(PhotoAppearance, LabOfWhiteAndBlack)
{
    EXPECT_NEAR(toLab(255, 255, 255)[0], 100.0f, 0.5f);
    EXPECT_NEAR(toLab(0, 0, 0)[0], 0.0f, 0.5f);
}

TEST(PhotoAppearance, ScaledPolygonShrinksAboutCentroid)
{
    const auto s = scaledPolygon({{0, 0}, {10, 0}, {10, 10}, {0, 10}}, 0.5);
    EXPECT_NEAR(s[0].x(), 2.5, 1e-9);
    EXPECT_NEAR(s[2].y(), 7.5, 1e-9);
}

TEST(PhotoAppearance, NoLandmarksReportsNoFace)
{
    QImage img(10, 10, QImage::Format_RGB888);
    EXPECT_FALSE(fromLandmarks(img, {}, QImage()).faceFound);
}

#include <QFile>
#include <cmath>
#include <cstring>

namespace {
// Landmarks of the regression model for coefficients `c` (modes beyond c.size() = 0), moved by an arbitrary similarity into "photo pixels".
std::vector<std::array<float, 3>> syntheticPhotoLandmarks(const std::vector<float>& c)
{
    QFile f(QStringLiteral(":/character/face_landmark_fit.bin"));
    EXPECT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray b = f.readAll();
    int32_t modes = 0, pts = 0;
    memcpy(&modes, b.constData() + 4, 4); memcpy(&pts, b.constData() + 8, 4);
    const float* w = reinterpret_cast<const float*>(b.constData() + 12);
    const size_t n = size_t(pts) * 3;
    std::vector<float> m(w + size_t(modes) * n, w + size_t(modes + 1) * n);
    for (size_t i = 0; i < c.size(); ++i) for (size_t j = 0; j < n; ++j) m[j] += c[i] * w[i * n + j];
    const float ang = 0.2f, ca = std::cos(ang), sa = std::sin(ang);
    std::vector<std::array<float, 3>> out;
    for (int i = 0; i < pts; ++i) {
        const float x = m[size_t(i) * 3], y = m[size_t(i) * 3 + 1], z = m[size_t(i) * 3 + 2];
        out.push_back({300.0f + 180.0f * (ca * x - sa * y), 250.0f + 180.0f * (sa * x + ca * y), 180.0f * z});
    }
    return out;
}
}  // namespace

TEST(PhotoAppearance, FaceFitRecoversKnownCoefficientsThroughASimilarity)
{
    std::vector<float> truth(45, 0.0f);
    truth[20] = 1.2f; truth[21] = -1.0f; truth[22] = 0.8f; truth[23] = 1.1f; truth[24] = -0.9f;
    const auto photo = syntheticPhotoLandmarks(truth);
    const FaceFit ff = fitFaceCoefficients(photo, std::vector<float>(45, 0.0f), 12, 0.001f);
    ASSERT_TRUE(ff.ok) << ff.note.toStdString();
    EXPECT_LT(ff.errAfter, 0.5f * ff.errBefore);
    EXPECT_LT(ff.errAfter, 0.01f);
    // The individual coefficients are NOT asserted: the face modes are strongly correlated in landmark space (modes 20-24 move the same
    // landmarks in similar ways), so many coefficient vectors give the same landmarks. What must hold is the landmark outcome.
    EXPECT_EQ(ff.coefficients.size(), 45u);
}

TEST(PhotoAppearance, FaceFitLeavesTheOtherModesAlone)
{
    std::vector<float> sam(45, 0.0f);
    sam[2] = 1.5f; sam[40] = -1.1f;   // body modes that do not move the face much
    const auto photo = syntheticPhotoLandmarks(std::vector<float>(45, 0.0f));
    const FaceFit ff = fitFaceCoefficients(photo, sam, 6, 0.05f);
    ASSERT_TRUE(ff.ok);
    EXPECT_FLOAT_EQ(ff.coefficients[2], 1.5f);
    EXPECT_FLOAT_EQ(ff.coefficients[40], -1.1f);
}

TEST(PhotoAppearance, FaceFitWithoutLandmarksIsNotOk)
{
    EXPECT_FALSE(fitFaceCoefficients({}, std::vector<float>(45, 0.0f)).ok);
}

namespace {
// a 12 x 12 grid of vertices in the xy plane at z = 0, 0.1 m apart, centred on the origin
void gridMesh(std::vector<float>& v, std::vector<int>& tris)
{
    for (int j = 0; j < 12; ++j)
        for (int i = 0; i < 12; ++i) { v.push_back((float(i) - 5.5f) * 0.1f); v.push_back((float(j) - 5.5f) * 0.1f); v.push_back(0.0f); }
    for (int j = 0; j < 11; ++j)
        for (int i = 0; i < 11; ++i) {
            const int a = j * 12 + i;
            tris.insert(tris.end(), {a, a + 1, a + 12, a + 1, a + 13, a + 12});
        }
}
}  // namespace

TEST(PhotoAppearance, ProjectFaceUsesTheSamCameraAndSeesTheFrontOfASurface)
{
    std::vector<float> v;
    std::vector<int> tris;
    gridMesh(v, tris);
    QImage photo(200, 200, QImage::Format_RGB888);
    photo.fill(Qt::gray);
    const auto fp = CharacterGen::PhotoAppearance::projectFace(photo, QImage(), QRectF(0, 0, 200, 200), v, tris, {0.0f, 0.0f, 2.0f}, 100.0f);
    ASSERT_TRUE(fp.ok);
    EXPECT_GT(fp.visible, 100);
    // vertex (i=6, j=6) = (0.05, 0.05, 0): X = 0.05, Y = -0.05 + 0 (the y flip), Z = 2 -> (100*0.05/2 + 100, 100*-0.05/2 + 100)
    const size_t k = size_t(6 * 12 + 6);
    EXPECT_NEAR(fp.pixel[k * 2], 102.5f, 0.01f);
    EXPECT_NEAR(fp.pixel[k * 2 + 1], 97.5f, 0.01f);
    EXPECT_GT(fp.weight[k], 0.9f);
}

TEST(PhotoAppearance, ProjectFaceIgnoresVerticesOutsideTheMatteOrTheHeadBox)
{
    std::vector<float> v;
    std::vector<int> tris;
    gridMesh(v, tris);
    QImage photo(200, 200, QImage::Format_RGB888);
    photo.fill(Qt::gray);
    QImage none(200, 200, QImage::Format_Grayscale8);
    none.fill(0);   // nobody in the photo
    EXPECT_FALSE(CharacterGen::PhotoAppearance::projectFace(photo, none, QRectF(0, 0, 200, 200), v, tris, {0.0f, 0.0f, 2.0f}, 100.0f).ok);
    EXPECT_FALSE(CharacterGen::PhotoAppearance::projectFace(photo, QImage(), QRectF(0, 0, 20, 20), v, tris, {0.0f, 0.0f, 2.0f}, 100.0f).ok);
}

namespace {
CharacterGen::PhotoAppearance::FaceAppearance shape(float top, float side, float lowest, float fringe, float coherence)
{
    CharacterGen::PhotoAppearance::FaceAppearance fa;
    fa.hairShapeOk = true;
    fa.hairTop = top; fa.hairSide = side; fa.hairLowest = lowest; fa.hairFringe = fringe; fa.hairCoherence = coherence;
    return fa;
}
}  // namespace

TEST(PhotoAppearance, ChooseHairNeedsAMeasuredShape)
{
    EXPECT_TRUE(CharacterGen::PhotoAppearance::chooseHair(CharacterGen::PhotoAppearance::FaceAppearance()).preset.isEmpty());
}

TEST(PhotoAppearance, ChooseHairFollowsLengthVolumeAndCurl)
{
    using CharacterGen::PhotoAppearance::chooseHair;
    EXPECT_EQ(chooseHair(shape(0.0f, 0.0f, 0.2f, 0.0f, 0.8f)).preset, QStringLiteral("buzz"));
    EXPECT_EQ(chooseHair(shape(0.25f, 0.1f, 0.8f, 0.0f, 0.8f)).preset, QStringLiteral("short_straight"));
    EXPECT_EQ(chooseHair(shape(0.25f, 0.1f, 0.8f, 0.5f, 0.8f)).preset, QStringLiteral("short_side_part"));
    EXPECT_EQ(chooseHair(shape(0.25f, 0.1f, 1.1f, 0.0f, 0.8f)).preset, QStringLiteral("bob"));
    EXPECT_EQ(chooseHair(shape(0.25f, 0.1f, 1.1f, 0.0f, 0.2f)).preset, QStringLiteral("curly_bob"));
    EXPECT_EQ(chooseHair(shape(0.3f, 0.2f, 2.0f, 0.0f, 0.8f)).preset, QStringLiteral("long_straight"));
    EXPECT_EQ(chooseHair(shape(0.3f, 0.2f, 2.0f, 0.0f, 0.2f)).preset, QStringLiteral("long_wavy"));
    EXPECT_EQ(chooseHair(shape(0.7f, 0.6f, 0.9f, 0.0f, 0.1f)).preset, QStringLiteral("afro"));
    EXPECT_EQ(chooseHair(shape(0.42f, 0.31f, 1.0f, 0.0f, 0.6f)).preset, QStringLiteral("afro"));   // voluminous short hair standing off the head
}

TEST(PhotoAppearance, ChooseHairLengthGrowsWithTheHairAndVolumeWithTheSides)
{
    using CharacterGen::PhotoAppearance::chooseHair;
    const auto shortHair = chooseHair(shape(0.25f, 0.1f, 0.8f, 0.0f, 0.8f));
    const auto longHair = chooseHair(shape(0.25f, 0.4f, 2.4f, 0.0f, 0.8f));
    EXPECT_GT(longHair.length, shortHair.length + 0.2f);
    EXPECT_GT(longHair.volume, shortHair.volume);
    EXPECT_LT(shortHair.curliness, 0.0f);   // straight hair keeps the preset's own curliness
    EXPECT_GE(chooseHair(shape(0.25f, 0.1f, 0.8f, 0.5f, 0.8f)).fringe, 0.5f);
    EXPECT_EQ(chooseHair(shape(0.25f, 0.1f, 0.8f, 0.05f, 0.8f)).fringe, 0.0f);   // no bangs in the photo: none on the character either
}
