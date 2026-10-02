// #524 — pure-data generator core: formulas, target grammar, JSON.

#include <gtest/gtest.h>

#include "AnimGenerators.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>

using namespace AnimGen;

static constexpr double M_PI_ = 3.14159265358979323846;

namespace {
Generator make(Type type, const QString& target = QStringLiteral("node:Hand/position.y"))
{
    Generator g;
    g.id = QStringLiteral("gen_1");
    g.type = type;
    EXPECT_TRUE(parseTarget(target, &g.target));
    return g;
}
} // namespace

TEST(AnimGenTarget, ParsesEveryKind)
{
    Target t;
    ASSERT_TRUE(parseTarget(QStringLiteral("node:Hand/position.y"), &t));
    EXPECT_EQ(t.kind, TargetKind::Node);
    EXPECT_EQ(t.object, QStringLiteral("Hand"));
    EXPECT_EQ(t.channel, QStringLiteral("position.y"));
    EXPECT_TRUE(t.clip.isEmpty());

    // Bone names may contain ':' — only the first one separates the kind.
    ASSERT_TRUE(parseTarget(QStringLiteral("bone:Rumba/mixamorig:Spine/rotation.z@Dance"), &t));
    EXPECT_EQ(t.kind, TargetKind::Bone);
    EXPECT_EQ(t.object, QStringLiteral("Rumba"));
    EXPECT_EQ(t.sub, QStringLiteral("mixamorig:Spine"));
    EXPECT_EQ(t.channel, QStringLiteral("rotation.z"));
    EXPECT_EQ(t.clip, QStringLiteral("Dance"));
    EXPECT_EQ(formatTarget(t), QStringLiteral("bone:Rumba/mixamorig:Spine/rotation.z@Dance"));

    ASSERT_TRUE(parseTarget(QStringLiteral("morph:Head/jawOpen/weight"), &t));
    EXPECT_EQ(t.sub, QStringLiteral("jawOpen"));
    ASSERT_TRUE(parseTarget(QStringLiteral("pose:Rumba/Smile/weight"), &t));
    EXPECT_EQ(t.kind, TargetKind::Pose);
    ASSERT_TRUE(parseTarget(QStringLiteral("light:KeyLight/intensity"), &t));
    EXPECT_EQ(t.kind, TargetKind::Light);
    ASSERT_TRUE(parseTarget(QStringLiteral("material:Body_MAT/diffuse.r"), &t));
    EXPECT_EQ(t.kind, TargetKind::Material);
}

TEST(AnimGenTarget, RejectsMalformedTargets)
{
    Target t;
    QString err;
    EXPECT_FALSE(parseTarget(QStringLiteral("Hand/position.y"), &t, &err));
    EXPECT_FALSE(parseTarget(QStringLiteral("widget:Hand/position.y"), &t, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("unknown target kind")));
    EXPECT_FALSE(parseTarget(QStringLiteral("node:Hand"), &t, &err));
    EXPECT_FALSE(parseTarget(QStringLiteral("node:Hand/colour"), &t, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("not valid")));
    EXPECT_FALSE(parseTarget(QStringLiteral("bone:Rumba/rotation.z"), &t, &err))
        << "a bone target needs entity AND bone";
    EXPECT_FALSE(parseTarget(QStringLiteral("light:Key/weight"), &t, &err));
}

TEST(AnimGenTarget, PathNeedsTheVectorChannel)
{
    Target node;
    ASSERT_TRUE(parseTarget(QStringLiteral("node:Drone/position"), &node));
    EXPECT_TRUE(validate(Type::FollowPath, node));
    EXPECT_FALSE(validate(Type::Sine, node)) << "scalar types use position.x/y/z";
    Target scalar;
    ASSERT_TRUE(parseTarget(QStringLiteral("node:Drone/position.x"), &scalar));
    EXPECT_FALSE(validate(Type::FollowPath, scalar));
    Target light;
    ASSERT_TRUE(parseTarget(QStringLiteral("light:Key/intensity"), &light));
    EXPECT_FALSE(validate(Type::FollowPath, light));
    EXPECT_TRUE(validate(Type::Noise, light));
}

TEST(AnimGenEval, SineMatchesTheFormula)
{
    Generator g = make(Type::Sine);
    g.amplitude = 0.5; g.frequency = 2.0; g.phaseDeg = 90.0; g.offset = 0.1;
    g.startTime = 1.0; g.duration = 3.0;
    for (double t : {1.0, 1.1, 2.37, 4.0}) {
        const double expect = 0.1 + 0.5 * std::sin(2.0 * M_PI_ * 2.0 * (t - 1.0) + M_PI_ / 2.0);
        EXPECT_NEAR(evaluateScalar(g, t, 10.0), expect, 1e-12) << t;
    }
    EXPECT_EQ(evaluateScalar(g, 0.5, 10.0), 0.0) << "silent before the window";
    EXPECT_EQ(evaluateScalar(g, 4.5, 10.0), 0.0) << "silent after the window";
}

TEST(AnimGenEval, DurationZeroRunsToTheClipEnd)
{
    Generator g = make(Type::Sine);
    g.frequency = 0.25; // a quarter cycle per second → peak at t=1
    EXPECT_NEAR(evaluateScalar(g, 1.0, 2.0), 1.0, 1e-12);
    EXPECT_EQ(evaluateScalar(g, 2.5, 2.0), 0.0);
    EXPECT_DOUBLE_EQ(windowEnd(g, 2.0), 2.0);
    EXPECT_DOUBLE_EQ(windowEnd(g, 0.0), 4.0) << "runtime targets have no clip; 4 s default";
}

TEST(AnimGenEval, NoiseIsDeterministicBoundedAndSeeded)
{
    Generator a = make(Type::Noise);
    a.amplitude = 1.0; a.noiseFrequency = 3.0; a.octaves = 4; a.seed = 7; a.duration = 10;
    Generator b = a;
    Generator c = a; c.seed = 8;
    double maxAbs = 0.0, diff = 0.0;
    for (int i = 0; i <= 500; ++i) {
        const double t = i * 0.02;
        const double va = evaluateScalar(a, t, 0.0);
        EXPECT_EQ(va, evaluateScalar(b, t, 0.0));
        maxAbs = std::max(maxAbs, std::abs(va));
        diff += std::abs(va - evaluateScalar(c, t, 0.0));
    }
    EXPECT_LE(maxAbs, 1.0 + 1e-9);
    EXPECT_GT(maxAbs, 0.2) << "the noise must actually move";
    EXPECT_GT(diff, 1.0) << "a different seed gives a different signal";
    // No forced zeros at round times (pure gradient noise vanishes on every
    // lattice point, i.e. on a regular beat).
    int nearZero = 0;
    for (int k = 0; k < 40; ++k) if (std::abs(noise1D(double(k), 9, 1)) < 1e-3) ++nearZero;
    EXPECT_LT(nearZero, 3);
    // Continuity: no jumps between close samples.
    for (int i = 0; i < 1000; ++i) {
        const double x = i * 0.0137;
        EXPECT_LT(std::abs(noise1D(x + 1e-4, 3, 1) - noise1D(x, 3, 1)), 0.01) << x;
    }
}

TEST(AnimGenEval, RampHoldsItsEndsAndEases)
{
    Generator g = make(Type::Ramp);
    g.rampFrom = 2.0; g.rampTo = 6.0; g.startTime = 1.0; g.duration = 2.0;
    EXPECT_DOUBLE_EQ(evaluateScalar(g, 0.0, 0.0), 2.0);
    EXPECT_DOUBLE_EQ(evaluateScalar(g, 2.0, 0.0), 4.0);
    EXPECT_DOUBLE_EQ(evaluateScalar(g, 5.0, 0.0), 6.0) << "a ramp to 6 stays at 6";
    g.ease = Ease::Smooth;
    EXPECT_DOUBLE_EQ(evaluateScalar(g, 2.0, 0.0), 4.0) << "smoothstep is symmetric";
    EXPECT_LT(evaluateScalar(g, 1.2, 0.0), 2.0 + 4.0 * 0.1) << "smooth starts slower than linear";
}

TEST(AnimGenEval, SpringSettlesOnTheTargetForEveryDampingRegime)
{
    for (double zeta : {0.2, 1.0, 2.5}) {
        Generator g = make(Type::Spring);
        g.springFrom = 0.0; g.springTo = 1.0; g.stiffness = 12.0; g.damping = zeta;
        EXPECT_NEAR(evaluateScalar(g, 0.0, 0.0), 0.0, 1e-12) << zeta;
        EXPECT_NEAR(evaluateScalar(g, 8.0, 0.0), 1.0, 1e-3) << zeta;
        // Zero initial velocity.
        const double v0 = (evaluateScalar(g, 1e-5, 0.0) - evaluateScalar(g, 0.0, 0.0)) / 1e-5;
        EXPECT_NEAR(v0, 0.0, 1e-2) << zeta;
    }
    // Under-damped overshoots, critically/over-damped never do.
    auto peak = [](double zeta) {
        Generator g; g.type = Type::Spring; g.springTo = 1.0; g.stiffness = 12.0; g.damping = zeta;
        double m = 0.0;
        for (int i = 0; i < 2000; ++i) m = std::max(m, evaluateScalar(g, i * 0.002, 0.0));
        return m;
    };
    EXPECT_GT(peak(0.2), 1.3);
    EXPECT_LE(peak(1.0), 1.0 + 1e-9);
    EXPECT_LE(peak(2.5), 1.0 + 1e-9);
}

TEST(AnimGenPath, PassesThroughEveryAnchorAtConstantSpeed)
{
    const std::vector<Ogre::Vector3> pts = {{0, 0, 0}, {4, 0, 0}, {4, 0, 4}, {0, 0, 4}};
    PathSampler open(pts, false);
    ASSERT_EQ(open.segmentCount(), 3);
    EXPECT_LT((open.position(0.0, true) - pts.front()).length(), 1e-4f);
    EXPECT_LT((open.position(1.0, true) - pts.back()).length(), 1e-4f);
    // Anchors are on the curve (segment-uniform parameter).
    for (int i = 0; i < 4; ++i)
        EXPECT_LT((open.position(double(i) / 3.0, false) - pts[size_t(i)]).length(), 1e-4f) << i;
    // Constant speed: equal parameter steps cover equal arc length.
    const auto a = open.position(0.25, true), b = open.position(0.5, true), c = open.position(0.75, true);
    const double l = open.length();
    EXPECT_GT(l, 11.0);
    double seg1 = 0.0, seg2 = 0.0;
    Ogre::Vector3 prev = a;
    for (int k = 1; k <= 200; ++k) { auto p = open.position(0.25 + 0.25 * k / 200.0, true); seg1 += (p - prev).length(); prev = p; }
    prev = b;
    for (int k = 1; k <= 200; ++k) { auto p = open.position(0.5 + 0.25 * k / 200.0, true); seg2 += (p - prev).length(); prev = p; }
    (void)c;
    EXPECT_NEAR(seg1, seg2, 0.02 * l);

    PathSampler closed(pts, true);
    EXPECT_EQ(closed.segmentCount(), 4);
    EXPECT_LT((closed.position(1.0, true) - pts.front()).length(), 1e-4f) << "a closed path returns home";
}

TEST(AnimGenPath, TangentAndOrientationFollowTheCurve)
{
    PathSampler line({{0, 0, 0}, {0, 0, -10}}, false);
    const auto t = line.tangent(0.5, true);
    EXPECT_NEAR(t.z, -1.0f, 1e-4f);
    const Ogre::Quaternion q = orientationAlong(t);
    // Ogre forward is local -Z: facing -Z needs no rotation.
    EXPECT_NEAR(q.w, 1.0f, 1e-4f);
    const Ogre::Quaternion qx = orientationAlong(Ogre::Vector3::UNIT_X);
    const Ogre::Vector3 fwd = qx * Ogre::Vector3::NEGATIVE_UNIT_Z;
    EXPECT_NEAR(fwd.x, 1.0f, 1e-4f);
    EXPECT_NEAR((qx * Ogre::Vector3::UNIT_Y).y, 1.0f, 1e-4f) << "stays upright";
}

TEST(AnimGenPath, ParameterLoopsAndHolds)
{
    Generator g = make(Type::FollowPath, QStringLiteral("node:Drone/position"));
    g.startTime = 1.0; g.duration = 4.0; g.loops = 2.0;
    EXPECT_DOUBLE_EQ(pathParameter(g, 0.0, 0.0), 0.0);
    EXPECT_NEAR(pathParameter(g, 2.0, 0.0), 0.5, 1e-12);
    EXPECT_NEAR(pathParameter(g, 3.5, 0.0), 0.25, 1e-12) << "second lap";
    EXPECT_DOUBLE_EQ(pathParameter(g, 5.0, 0.0), 1.0) << "ends on the last point, not back at 0";
    EXPECT_DOUBLE_EQ(pathParameter(g, 9.0, 0.0), 1.0);
}

TEST(AnimGenSampling, IncludesBothEnds)
{
    const auto t = sampleTimes(0.0, 1.0, 30);
    ASSERT_EQ(t.size(), 31u);
    EXPECT_DOUBLE_EQ(t.front(), 0.0);
    EXPECT_DOUBLE_EQ(t.back(), 1.0);
    const auto odd = sampleTimes(0.0, 1.05, 10);
    EXPECT_DOUBLE_EQ(odd.back(), 1.05) << "a non-integer span still ends exactly at 'to'";
}

TEST(AnimGenJson, RoundTripsEveryType)
{
    std::vector<Generator> gens;
    Generator s = make(Type::Sine); s.id = "a"; s.amplitude = 0.05; s.frequency = 1.5; s.phaseDeg = 30; gens.push_back(s);
    Generator n = make(Type::Noise, "light:Key/intensity"); n.id = "b"; n.seed = 42; n.octaves = 3; n.enabled = false; gens.push_back(n);
    Generator r = make(Type::Ramp, "material:M/diffuse.r"); r.id = "c"; r.rampFrom = 0.2; r.rampTo = 0.9; r.ease = Ease::Smooth; gens.push_back(r);
    Generator sp = make(Type::Spring, "morph:Head/jawOpen/weight"); sp.id = "d"; sp.springTo = 0.7; sp.damping = 0.4; sp.baked = true; gens.push_back(sp);
    Generator p = make(Type::FollowPath, "node:Drone/position@Hover"); p.id = "e";
    p.pathPoints = {{0, 1, 2}, {3, 4, 5}}; p.pathClosed = true; p.orientToPath = true; p.loops = 3;
    p.state[QStringLiteral("base")] = QStringLiteral("opaque");
    gens.push_back(p);

    const QByteArray bytes = QJsonDocument(toDocument(gens)).toJson();
    std::vector<Generator> back;
    QString err;
    ASSERT_TRUE(fromDocument(QJsonDocument::fromJson(bytes).object(), &back, &err)) << err.toStdString();
    ASSERT_EQ(back.size(), gens.size());
    EXPECT_DOUBLE_EQ(back[0].amplitude, 0.05);
    EXPECT_DOUBLE_EQ(back[0].phaseDeg, 30);
    EXPECT_EQ(back[1].seed, 42u);
    EXPECT_FALSE(back[1].enabled);
    EXPECT_EQ(back[2].ease, Ease::Smooth);
    EXPECT_DOUBLE_EQ(back[2].rampTo, 0.9);
    EXPECT_DOUBLE_EQ(back[3].springTo, 0.7);
    EXPECT_TRUE(back[3].baked);
    EXPECT_EQ(back[4].pathPoints.size(), 2u);
    EXPECT_FLOAT_EQ(back[4].pathPoints[1].z, 5.0f);
    EXPECT_TRUE(back[4].pathClosed);
    EXPECT_EQ(back[4].target.clip, QStringLiteral("Hover"));
    EXPECT_EQ(back[4].state.value(QStringLiteral("base")).toString(), QStringLiteral("opaque"))
        << "adapter state round-trips untouched";
    // Same values everywhere after the round trip.
    for (size_t i = 0; i < 4; ++i)
        for (double t : {0.0, 0.7, 1.9})
            EXPECT_DOUBLE_EQ(evaluateScalar(back[i], t, 3.0), evaluateScalar(gens[i], t, 3.0));
}

TEST(AnimGenJson, RejectsBadDocuments)
{
    std::vector<Generator> out;
    QString err;
    EXPECT_FALSE(fromDocument(QJsonObject{{"schema", "other"}}, &out, &err));
    QJsonObject g{{"id", "x"}, {"type", "sine"}, {"target", "node:N/position.y"}, {"amplitude", "big"}};
    QJsonObject doc{{"schema", schemaId()}, {"generators", QJsonArray{g}}};
    EXPECT_FALSE(fromDocument(doc, &out, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("finite number"))) << err.toStdString();
    QJsonObject ok{{"id", "x"}, {"type", "sine"}, {"target", "node:N/position.y"}};
    QJsonObject dup{{"schema", schemaId()}, {"generators", QJsonArray{ok, ok}}};
    EXPECT_FALSE(fromDocument(dup, &out, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("duplicate")));
}

TEST(AnimGenParams, ApplyParamCoversTheCliSurface)
{
    Generator g = make(Type::Spring);
    QString err;
    EXPECT_TRUE(applyParam(&g, "to", "2.5", &err));
    EXPECT_DOUBLE_EQ(g.springTo, 2.5) << "'to' routes to the spring for a spring";
    EXPECT_TRUE(applyParam(&g, "duration", "-3", &err));
    EXPECT_DOUBLE_EQ(g.duration, 0.0);
    EXPECT_TRUE(applyParam(&g, "octaves", "20", &err));
    EXPECT_EQ(g.octaves, 8);
    EXPECT_TRUE(applyParam(&g, "points", "0,0,0; 1,2,3", &err));
    EXPECT_EQ(g.pathPoints.size(), 2u);
    EXPECT_FALSE(applyParam(&g, "points", "1,2", &err));
    EXPECT_FALSE(applyParam(&g, "amplitude", "nan", &err));
    EXPECT_FALSE(applyParam(&g, "wobble", "1", &err));
    EXPECT_TRUE(err.contains(QStringLiteral("unknown")));
    EXPECT_TRUE(applyParam(&g, "closed", "yes", &err));
    EXPECT_TRUE(g.pathClosed);
    EXPECT_EQ(uniqueId({g}), QStringLiteral("gen_2")) << "gen_1 is taken";
}
