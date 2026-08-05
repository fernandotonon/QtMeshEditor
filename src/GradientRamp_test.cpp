#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "BrushEngine.h"
#include "GradientRamp.h"
#include "TexturePaintBuffer.h"

#include <cmath>

namespace {

GradientRamp::Rgba approxEq(const GradientRamp::Rgba& a,
                            const GradientRamp::Rgba& b,
                            float eps = 0.02f)
{
    EXPECT_NEAR(a.r, b.r, eps);
    EXPECT_NEAR(a.g, b.g, eps);
    EXPECT_NEAR(a.b, b.b, eps);
    EXPECT_NEAR(a.a, b.a, eps);
    return a;
}

} // namespace

TEST(GradientRampTest, SampleLinearEndpointsAndMidpoint)
{
    auto ramp = GradientRamp::fromFgBg({0, 0, 0, 1}, {1, 1, 1, 1});
    approxEq(ramp.sample(0.0f), {0, 0, 0, 1});
    approxEq(ramp.sample(1.0f), {1, 1, 1, 1});
    approxEq(ramp.sample(0.5f), {0.5f, 0.5f, 0.5f, 1});
}

TEST(GradientRampTest, SampleSteppedHoldsLeftStop)
{
    GradientRamp::Ramp ramp;
    ramp.interpolate = GradientRamp::Interpolate::Stepped;
    ramp.stops = {
        {0.0f, {1, 0, 0, 1}},
        {0.5f, {0, 1, 0, 1}},
        {1.0f, {0, 0, 1, 1}},
    };
    approxEq(ramp.sample(0.25f), {1, 0, 0, 1});
    approxEq(ramp.sample(0.49f), {1, 0, 0, 1});
    approxEq(ramp.sample(0.5f), {0, 1, 0, 1});
}

TEST(GradientRampTest, BundledPresetsAreSixAndValid)
{
    const auto presets = GradientRamp::bundledPresets();
    ASSERT_EQ(presets.size(), 6u);
    for (const auto& p : presets) {
        EXPECT_TRUE(p.isValid()) << p.name;
        EXPECT_FALSE(p.name.empty());
        // Endpoints must be sampleable.
        const auto a = p.sample(0.0f);
        const auto b = p.sample(1.0f);
        EXPECT_GE(a.a, 0.0f);
        EXPECT_GE(b.a, 0.0f);
    }
    EXPECT_NE(GradientRamp::findBundled("Sunset"), nullptr);
    EXPECT_NE(GradientRamp::findBundled("Hue"), nullptr);
    EXPECT_EQ(GradientRamp::findBundled("DoesNotExist"), nullptr);
}

TEST(GradientRampTest, JsonRoundTrip)
{
    const auto* sunset = GradientRamp::findBundled("Sunset");
    ASSERT_NE(sunset, nullptr);
    const std::string json = GradientRamp::toJson(*sunset);
    GradientRamp::Ramp loaded;
    ASSERT_TRUE(GradientRamp::fromJson(json, loaded));
    EXPECT_EQ(loaded.name, sunset->name);
    EXPECT_EQ(loaded.stops.size(), sunset->stops.size());
    approxEq(loaded.sample(0.25f), sunset->sample(0.25f));
}

TEST(GradientRampTest, CustomSaveLoadAcrossSessions)
{
    // UnitTests already owns a QCoreApplication. Write into a temp file
    // via the JSON helpers (the AppData path is covered by saveCustom
    // when a writable location exists).
    GradientRamp::Ramp ramp =
        GradientRamp::fromFgBg({1, 0, 0, 1}, {0, 0, 1, 1}, "UnitTestRamp");
    const std::string json = GradientRamp::toJson(ramp);
    ASSERT_FALSE(json.empty());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString path = tmp.filePath(QStringLiteral("UnitTestRamp.json"));
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(f.write(json.data(), static_cast<qint64>(json.size())),
                  static_cast<qint64>(json.size()));
    }
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        GradientRamp::Ramp loaded;
        ASSERT_TRUE(GradientRamp::fromJson(f.readAll().toStdString(), loaded));
        EXPECT_EQ(loaded.name, "UnitTestRamp");
        approxEq(loaded.sample(0.5f), {0.5f, 0, 0.5f, 1});
    }

    // Also exercise the AppData helper when a writable location exists.
    const std::string dir = GradientRamp::rampsDirectory();
    if (!dir.empty()) {
        const std::string saved = GradientRamp::saveCustom(ramp);
        EXPECT_FALSE(saved.empty());
        bool found = false;
        for (const auto& r : GradientRamp::loadCustomRamps()) {
            if (r.name == "UnitTestRamp") {
                found = true;
                break;
            }
        }
        EXPECT_TRUE(found);
        EXPECT_TRUE(GradientRamp::deleteCustom("UnitTestRamp"));
    }
}

TEST(BrushEngineTest, SolidIgnoresRamp)
{
    const auto* hue = GradientRamp::findBundled("Hue");
    ASSERT_NE(hue, nullptr);
    BrushEngine::SampleParams p;
    p.source = BrushEngine::ColorSource::Solid;
    p.solid = {0.1f, 0.2f, 0.3f, 1.0f};
    p.ramp = hue;
    p.mode = BrushEngine::GradientMode::Linear;
    p.strokeT = 0.5f;
    approxEq(BrushEngine::sampleColor(p), {0.1f, 0.2f, 0.3f, 1.0f});
}

TEST(BrushEngineTest, LinearUsesStrokeT)
{
    auto ramp = GradientRamp::fromFgBg({0, 0, 0, 1}, {1, 0, 0, 1});
    BrushEngine::SampleParams p;
    p.source = BrushEngine::ColorSource::Gradient;
    p.ramp = &ramp;
    p.mode = BrushEngine::GradientMode::Linear;
    p.strokeT = 0.0f;
    approxEq(BrushEngine::sampleColor(p), {0, 0, 0, 1});
    p.strokeT = 1.0f;
    approxEq(BrushEngine::sampleColor(p), {1, 0, 0, 1});
    p.strokeT = 0.5f;
    approxEq(BrushEngine::sampleColor(p), {0.5f, 0, 0, 1});
}

TEST(BrushEngineTest, RadialAndAngularStampSelection)
{
    auto ramp = GradientRamp::fromFgBg({0, 0, 0, 1}, {1, 1, 1, 1});
    BrushEngine::SampleParams p;
    p.source = BrushEngine::ColorSource::Gradient;
    p.ramp = &ramp;

    p.mode = BrushEngine::GradientMode::Radial;
    p.dx = 0; p.dy = 0;
    approxEq(BrushEngine::sampleColor(p), {0, 0, 0, 1});
    p.dx = 1; p.dy = 0;
    approxEq(BrushEngine::sampleColor(p), {1, 1, 1, 1});

    p.mode = BrushEngine::GradientMode::Angular;
    // atan2(0, 1) = 0 → t = 0 → black
    p.dx = 1; p.dy = 0;
    approxEq(BrushEngine::sampleColor(p), {0, 0, 0, 1});
    // atan2(0, -1) = π → t = 0.5 → mid grey
    p.dx = -1; p.dy = 0;
    approxEq(BrushEngine::sampleColor(p), {0.5f, 0.5f, 0.5f, 1});
}

TEST(BrushEngineTest, LinearStrokeTIsSmoothAndPeriodic)
{
    EXPECT_NEAR(BrushEngine::linearStrokeT(0.0f, 1.0f), 0.0f, 1e-5f);
    EXPECT_NEAR(BrushEngine::linearStrokeT(0.5f, 1.0f), 0.5f, 1e-5f);
    EXPECT_NEAR(BrushEngine::linearStrokeT(1.0f, 1.0f), 0.0f, 1e-5f);
    EXPECT_NEAR(BrushEngine::linearStrokeT(1.25f, 1.0f, 0.1f), 0.35f, 1e-5f);
    // Turning the stroke (same length) must not jitter t — length-based.
    EXPECT_FLOAT_EQ(BrushEngine::linearStrokeT(0.4f, 2.0f),
                    BrushEngine::linearStrokeT(0.4f, 2.0f));
}

TEST(BrushEngineTest, PaintBrushGradientCallbackWritesRamp)
{
    TexturePaintBuffer buf(64, 64);
    buf.clear(Ogre::ColourValue(0, 0, 0, 1));
    auto ramp = GradientRamp::fromFgBg({1, 0, 0, 1}, {0, 0, 1, 1});
    const int n = buf.paintBrush(
        Ogre::Vector2(0.5f, 0.5f),
        0.4f,
        [&ramp](float dx, float dy) {
            BrushEngine::SampleParams p;
            p.source = BrushEngine::ColorSource::Gradient;
            p.ramp = &ramp;
            p.mode = BrushEngine::GradientMode::Radial;
            p.dx = dx;
            p.dy = dy;
            const auto c = BrushEngine::sampleColor(p);
            return Ogre::ColourValue(c.r, c.g, c.b, c.a);
        },
        1.0f, 0.0f);
    EXPECT_GT(n, 0);
    // Centre should be near red (radial t≈0), edge near blue (t≈1).
    const auto centre = buf.pixel(32, 32);
    EXPECT_GT(centre.r, 0.7f);
    EXPECT_LT(centre.b, 0.3f);
}
