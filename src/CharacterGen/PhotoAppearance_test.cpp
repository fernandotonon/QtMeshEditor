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
