/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — BrushEngine tests (Paint v2 Slice J, issue #553).

#553 names this file explicitly. BrushEngine is the colour-source decision for
every brush stamp (Slice A #544), so the cases that matter are the ones a caller
can get subtly wrong: which gradient mode reads which SampleParams field, the
fallback when a ramp is missing, and the wrap behaviour of the three t-mappers.

Pure data — no Qt, no Ogre, no scene.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "BrushEngine.h"

#include <cmath>

namespace {

using namespace BrushEngine;

/// A two-stop black→white ramp, so a sampled t reads straight off the red
/// channel and an assertion can name the expected value.
GradientRamp::Ramp blackToWhite()
{
    GradientRamp::Ramp r;
    r.name = "bw";
    r.stops.push_back({0.0f, GradientRamp::Rgba{0.0f, 0.0f, 0.0f, 1.0f}});
    r.stops.push_back({1.0f, GradientRamp::Rgba{1.0f, 1.0f, 1.0f, 1.0f}});
    return r;
}

} // namespace

// --- t mappers -------------------------------------------------------------

TEST(BrushEngineTest, LinearStrokeTWrapsIntoUnitRange) {
    // One full wavelength returns to the start of the ramp.
    EXPECT_NEAR(linearStrokeT(0.0f, 4.0f), 0.0f, 1e-5f);
    EXPECT_NEAR(linearStrokeT(2.0f, 4.0f), 0.5f, 1e-5f);
    // Past one wavelength it must WRAP, not clamp — a clamped stroke would
    // freeze at the ramp's end colour for the rest of a long stroke.
    const float wrapped = linearStrokeT(6.0f, 4.0f);
    EXPECT_GE(wrapped, 0.0f);
    EXPECT_LE(wrapped, 1.0f);
    EXPECT_NEAR(wrapped, 0.5f, 1e-5f);
}

TEST(BrushEngineTest, LinearStrokeTAppliesPhase) {
    EXPECT_NEAR(linearStrokeT(0.0f, 4.0f, 0.25f), 0.25f, 1e-5f);
}

TEST(BrushEngineTest, LinearStrokeTSurvivesADegenerateWavelength) {
    // A zero/negative wavelength would divide by zero; it must degrade to a
    // finite value rather than producing NaN and poisoning every dab colour.
    EXPECT_TRUE(std::isfinite(linearStrokeT(1.0f, 0.0f)));
    EXPECT_TRUE(std::isfinite(linearStrokeT(1.0f, -4.0f)));
}

TEST(BrushEngineTest, RadialTIsZeroAtCentreAndOneAtTheEdge) {
    EXPECT_NEAR(radialT(0.0f, 0.0f), 0.0f, 1e-5f);
    EXPECT_NEAR(radialT(1.0f, 0.0f), 1.0f, 1e-5f);
    EXPECT_NEAR(radialT(0.0f, -1.0f), 1.0f, 1e-5f) << "sign must not matter";
    // Beyond the footprint edge it clamps: the ramp has no data past 1.
    EXPECT_NEAR(radialT(3.0f, 4.0f), 1.0f, 1e-5f);
}

TEST(BrushEngineTest, AngularTCoversTheFullCircleWithoutLeavingUnitRange) {
    for (int deg = 0; deg < 360; deg += 15) {
        const float rad = static_cast<float>(deg) * 3.14159265f / 180.0f;
        const float t = angularT(std::cos(rad), std::sin(rad));
        EXPECT_GE(t, 0.0f) << "deg=" << deg;
        EXPECT_LT(t, 1.0001f) << "deg=" << deg;
    }
    // Opposite directions must land half a cycle apart.
    EXPECT_NEAR(std::abs(angularT(1.0f, 0.0f) - angularT(-1.0f, 0.0f)), 0.5f, 1e-4f);
}

// --- colour source ---------------------------------------------------------

TEST(BrushEngineTest, SolidSourceIgnoresTheRampEntirely) {
    const auto ramp = blackToWhite();
    SampleParams p;
    p.source = ColorSource::Solid;
    p.solid = GradientRamp::Rgba{0.25f, 0.5f, 0.75f, 1.0f};
    p.ramp = &ramp;
    p.strokeT = 1.0f;             // would be WHITE if the ramp were consulted

    const auto c = sampleColor(p);
    EXPECT_NEAR(c.r, 0.25f, 1e-5f);
    EXPECT_NEAR(c.g, 0.5f, 1e-5f);
    EXPECT_NEAR(c.b, 0.75f, 1e-5f);
}

TEST(BrushEngineTest, GradientWithoutARampFallsBackToSolid) {
    // The fallback matters: a null ramp returning black would silently paint
    // black instead of the user's chosen colour.
    SampleParams p;
    p.source = ColorSource::Gradient;
    p.ramp = nullptr;
    p.solid = GradientRamp::Rgba{0.1f, 0.2f, 0.3f, 1.0f};

    const auto c = sampleColor(p);
    EXPECT_NEAR(c.r, 0.1f, 1e-5f);
    EXPECT_NEAR(c.g, 0.2f, 1e-5f);
    EXPECT_NEAR(c.b, 0.3f, 1e-5f);
}

TEST(BrushEngineTest, LinearGradientReadsStrokeTNotTheOffset) {
    const auto ramp = blackToWhite();
    SampleParams p;
    p.source = ColorSource::Gradient;
    p.ramp = &ramp;
    p.mode = GradientMode::Linear;
    p.strokeT = 1.0f;
    p.dx = 0.0f; p.dy = 0.0f;     // centre — radial would read black here

    EXPECT_NEAR(sampleColor(p).r, 1.0f, 1e-4f)
        << "Linear must use strokeT; reading dx/dy would return the centre colour";
}

TEST(BrushEngineTest, RadialGradientReadsTheOffsetNotStrokeT) {
    const auto ramp = blackToWhite();
    SampleParams p;
    p.source = ColorSource::Gradient;
    p.ramp = &ramp;
    p.mode = GradientMode::Radial;
    p.strokeT = 0.0f;             // would be BLACK if strokeT were used
    p.dx = 1.0f; p.dy = 0.0f;     // footprint edge -> white

    EXPECT_NEAR(sampleColor(p).r, 1.0f, 1e-4f)
        << "Radial must use dx/dy; reading strokeT would return the stroke colour";
}

TEST(BrushEngineTest, RadialCentreAndEdgeDifferAcrossTheRamp) {
    const auto ramp = blackToWhite();
    SampleParams p;
    p.source = ColorSource::Gradient;
    p.ramp = &ramp;
    p.mode = GradientMode::Radial;

    p.dx = 0.0f; p.dy = 0.0f;
    const float centre = sampleColor(p).r;
    p.dx = 1.0f; p.dy = 0.0f;
    const float edge = sampleColor(p).r;
    EXPECT_LT(centre, edge) << "a radial gradient must vary from centre to edge";
}

TEST(BrushEngineTest, AngularGradientVariesWithDirection) {
    const auto ramp = blackToWhite();
    SampleParams p;
    p.source = ColorSource::Gradient;
    p.ramp = &ramp;
    p.mode = GradientMode::Angular;

    p.dx = 1.0f; p.dy = 0.0f;
    const float a = sampleColor(p).r;
    p.dx = -1.0f; p.dy = 0.0f;
    const float b = sampleColor(p).r;
    EXPECT_GT(std::abs(a - b), 0.2f)
        << "opposite directions must sample distinctly different ramp positions";
}

TEST(BrushEngineTest, PhaseJitterShiftsTheSampledColour) {
    const auto ramp = blackToWhite();
    SampleParams p;
    p.source = ColorSource::Gradient;
    p.ramp = &ramp;
    p.mode = GradientMode::Linear;
    p.strokeT = 0.0f;

    const float noJitter = sampleColor(p).r;
    p.phaseJitter = 0.5f;
    const float jittered = sampleColor(p).r;
    EXPECT_GT(std::abs(jittered - noJitter), 0.2f)
        << "phaseJitter must move the sample point, or ramp jitter does nothing";
}

TEST(BrushEngineTest, AnEmptyRampDoesNotProduceNaN) {
    // An empty ramp is reachable via a corrupt/partial custom ramp file; the
    // colour must stay finite so it cannot poison the paint buffer.
    GradientRamp::Ramp empty;
    empty.name = "empty";
    SampleParams p;
    p.source = ColorSource::Gradient;
    p.ramp = &empty;
    p.solid = GradientRamp::Rgba{0.4f, 0.4f, 0.4f, 1.0f};

    const auto c = sampleColor(p);
    EXPECT_TRUE(std::isfinite(c.r));
    EXPECT_TRUE(std::isfinite(c.g));
    EXPECT_TRUE(std::isfinite(c.b));
    EXPECT_TRUE(std::isfinite(c.a));
}
