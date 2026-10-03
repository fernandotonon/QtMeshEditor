// #526 — AnimGraph: pure state-machine runtime + JSON.

#include <gtest/gtest.h>

#include "AnimGraph.h"

#include <QJsonArray>

using namespace AnimGraph;

namespace {

double clipLen(const QString& clip, const void*)
{
    if (clip == QLatin1String("Idle")) return 2.0;
    if (clip == QLatin1String("Walk")) return 1.0;
    if (clip == QLatin1String("Run")) return 0.8;
    if (clip == QLatin1String("Wave")) return 1.5;
    if (clip == QLatin1String("Draw")) return 0.5;
    return 0.0;
}

Transition tr(const QString& id, const QString& from, const QString& to, std::vector<Condition> c, double dur = 0.2)
{
    Transition t;
    t.id = id;
    t.from = from;
    t.to = to;
    t.conditions = std::move(c);
    t.duration = dur;
    return t;
}

// idle → walk (speed > 0.1), walk → run (speed > 2), run → walk (speed <= 2),
// walk → idle (speed <= 0.1)
Graph locomotion()
{
    Graph g;
    g.entry = QStringLiteral("idle");
    g.states = {{QStringLiteral("idle"), QStringLiteral("Idle")},
                {QStringLiteral("walk"), QStringLiteral("Walk")},
                {QStringLiteral("run"), QStringLiteral("Run")}};
    g.params = {{QStringLiteral("speed"), ParamType::Float, 0.0}};
    g.transitions = {
        tr("t1", "idle", "walk", {{"speed", Op::Greater, 0.1}}),
        tr("t2", "walk", "run", {{"speed", Op::Greater, 2.0}}),
        tr("t3", "run", "walk", {{"speed", Op::LessEq, 2.0}}),
        tr("t4", "walk", "idle", {{"speed", Op::LessEq, 0.1}}),
    };
    return g;
}

double weightOf(const std::vector<Contribution>& cs, const QString& clip)
{
    double w = 0.0;
    for (const auto& c : cs) if (c.clip == clip) w += c.weight;
    return w;
}

} // namespace

TEST(AnimGraphCurves, LinearEaseStep)
{
    EXPECT_DOUBLE_EQ(curveWeight(Curve::Linear, 0.25), 0.25);
    EXPECT_DOUBLE_EQ(curveWeight(Curve::Ease, 0.5), 0.5);
    EXPECT_LT(curveWeight(Curve::Ease, 0.1), 0.1) << "ease starts slower than linear";
    EXPECT_DOUBLE_EQ(curveWeight(Curve::Step, 0.99), 0.0);
    EXPECT_DOUBLE_EQ(curveWeight(Curve::Step, 1.0), 1.0);
    EXPECT_DOUBLE_EQ(curveWeight(Curve::Linear, 7.0), 1.0) << "clamped";
}

TEST(AnimGraphRuntime, SpeedParameterDrivesIdleWalkRunWithBlends)
{
    Graph g = locomotion();
    Runtime rt;
    ASSERT_TRUE(start(g, &rt, clipLen, nullptr));
    auto params = g.params;
    EXPECT_EQ(step(g, &params, &rt, 0.1, clipLen, nullptr), QString());
    EXPECT_DOUBLE_EQ(weightOf(contributions(rt), "Idle"), 1.0);

    params[0].value = 1.0;
    EXPECT_EQ(step(g, &params, &rt, 0.016, clipLen, nullptr), QStringLiteral("t1"));
    EXPECT_EQ(rt.current.primary.state, QStringLiteral("walk"));
    ASSERT_TRUE(rt.blending);
    step(g, &params, &rt, 0.1, clipLen, nullptr);   // half of the 0.2 s blend
    auto cs = contributions(rt);
    EXPECT_NEAR(weightOf(cs, "Walk"), 0.5, 1e-9);
    EXPECT_NEAR(weightOf(cs, "Idle"), 0.5, 1e-9);
    step(g, &params, &rt, 0.2, clipLen, nullptr);
    EXPECT_FALSE(rt.blending);
    EXPECT_DOUBLE_EQ(weightOf(contributions(rt), "Walk"), 1.0);

    params[0].value = 3.0;
    EXPECT_EQ(step(g, &params, &rt, 0.016, clipLen, nullptr), QStringLiteral("t2"));
    EXPECT_EQ(rt.current.primary.state, QStringLiteral("run"));
    params[0].value = 0.0;
    EXPECT_EQ(step(g, &params, &rt, 0.016, clipLen, nullptr), QStringLiteral("t3"));
    EXPECT_EQ(step(g, &params, &rt, 0.016, clipLen, nullptr), QStringLiteral("t4"));
    EXPECT_EQ(rt.current.primary.state, QStringLiteral("idle"));
}

TEST(AnimGraphRuntime, ClocksLoopOrClampAndHonourSpeed)
{
    Graph g = locomotion();
    g.states[0].speed = 2.0;
    Runtime rt;
    ASSERT_TRUE(start(g, &rt, clipLen, nullptr));
    auto params = g.params;
    step(g, &params, &rt, 1.5, clipLen, nullptr);   // 3 s at 2x on a 2 s loop → 1 s
    EXPECT_NEAR(rt.current.primary.time, 1.0, 1e-9);
    g.states[0].loop = false;
    ASSERT_TRUE(start(g, &rt, clipLen, nullptr));
    step(g, &params, &rt, 5.0, clipLen, nullptr);
    EXPECT_DOUBLE_EQ(rt.current.primary.time, 2.0) << "a one-shot clip holds its last frame";
}

TEST(AnimGraphRuntime, TriggerIsConsumedAndExitTimeWaits)
{
    Graph g;
    g.entry = QStringLiteral("idle");
    g.states = {{QStringLiteral("idle"), QStringLiteral("Idle")}, {QStringLiteral("draw"), QStringLiteral("Draw"), false}};
    g.params = {{QStringLiteral("draw"), ParamType::Trigger, 0.0}};
    Transition back = tr("back", "draw", "idle", {}, 0.1);
    back.exitTime = 1.0;   // after the draw finishes
    g.transitions = {tr("go", "idle", "draw", {{"draw", Op::IsTrue, 0}}), back};
    ASSERT_TRUE(validate(g));
    Runtime rt;
    ASSERT_TRUE(start(g, &rt, clipLen, nullptr));
    auto params = g.params;
    params[0].value = 1.0;
    EXPECT_EQ(step(g, &params, &rt, 0.01, clipLen, nullptr), QStringLiteral("go"));
    EXPECT_DOUBLE_EQ(params[0].value, 0.0) << "the trigger is consumed";
    EXPECT_EQ(step(g, &params, &rt, 0.2, clipLen, nullptr), QString()) << "exit time not reached";
    EXPECT_EQ(step(g, &params, &rt, 0.4, clipLen, nullptr), QStringLiteral("back"));
}

TEST(AnimGraphRuntime, AnyStateSourceDoesNotRestartTheTarget)
{
    Graph g = locomotion();
    g.transitions = {tr("any", "*", "run", {{"speed", Op::Greater, 2.0}})};
    Runtime rt;
    ASSERT_TRUE(start(g, &rt, clipLen, nullptr));
    auto params = g.params;
    params[0].value = 5.0;
    EXPECT_EQ(step(g, &params, &rt, 0.01, clipLen, nullptr), QStringLiteral("any"));
    step(g, &params, &rt, 0.3, clipLen, nullptr);
    const double t = rt.current.primary.time;
    EXPECT_EQ(step(g, &params, &rt, 0.1, clipLen, nullptr), QString());
    EXPECT_NEAR(rt.current.primary.time, t + 0.1, 1e-9) << "no re-entry";
}

TEST(AnimGraphRuntime, MaskedTransitionLayersOverTheRunningBody)
{
    Graph g = locomotion();
    g.states.push_back({QStringLiteral("wave"), QStringLiteral("Wave")});
    g.params.push_back({QStringLiteral("waving"), ParamType::Bool, 0.0});
    Transition wave = tr("w", "run", "wave", {{"waving", Op::IsTrue, 0}}, 0.2);
    wave.mask = {QStringLiteral("Spine"), QStringLiteral("LeftArm"), QStringLiteral("RightArm")};
    g.transitions.insert(g.transitions.begin(), wave);
    g.transitions.push_back(tr("unwave", "wave", "run", {{"waving", Op::IsFalse, 0}}, 0.2));
    ASSERT_TRUE(validate(g));

    Runtime rt;
    ASSERT_TRUE(start(g, &rt, clipLen, nullptr));
    auto params = g.params;
    params[0].value = 3.0;
    step(g, &params, &rt, 0.01, clipLen, nullptr);   // idle → walk
    step(g, &params, &rt, 0.5, clipLen, nullptr);    // walk → run
    step(g, &params, &rt, 0.5, clipLen, nullptr);
    ASSERT_EQ(rt.current.primary.state, QStringLiteral("run"));
    const double runTime = rt.current.primary.time;

    params[1].value = 1.0;
    EXPECT_EQ(step(g, &params, &rt, 0.01, clipLen, nullptr), QStringLiteral("w"));
    step(g, &params, &rt, 0.5, clipLen, nullptr);
    ASSERT_FALSE(rt.blending);
    const auto cs = contributions(rt);
    ASSERT_EQ(cs.size(), 2u);
    // Wave on the mask, run everywhere else — and run kept its clock.
    EXPECT_EQ(cs[0].clip, QStringLiteral("Wave"));
    EXPECT_FALSE(cs[0].invertMask);
    EXPECT_EQ(cs[0].mask.size(), 3);
    EXPECT_EQ(cs[1].clip, QStringLiteral("Run"));
    EXPECT_TRUE(cs[1].invertMask);
    EXPECT_NEAR(cs[1].time, std::fmod(runTime + 0.51, 0.8), 1e-9);

    params[1].value = 0.0;
    EXPECT_EQ(step(g, &params, &rt, 0.01, clipLen, nullptr), QStringLiteral("unwave"));
    step(g, &params, &rt, 0.5, clipLen, nullptr);
    const auto back = contributions(rt);
    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back[0].clip, QStringLiteral("Run"));
    EXPECT_TRUE(back[0].mask.isEmpty());
}

TEST(AnimGraphRuntime, StepCurveSnapsWithoutBlending)
{
    Graph g = locomotion();
    g.transitions[0].curve = Curve::Step;
    Runtime rt;
    ASSERT_TRUE(start(g, &rt, clipLen, nullptr));
    auto params = g.params;
    params[0].value = 1.0;
    step(g, &params, &rt, 0.01, clipLen, nullptr);
    EXPECT_FALSE(rt.blending);
    EXPECT_DOUBLE_EQ(weightOf(contributions(rt), "Walk"), 1.0);
}

TEST(AnimGraphJson, RoundTripAndValidation)
{
    Graph g = locomotion();
    g.transitions[1].mask = {QStringLiteral("Spine")};
    g.transitions[1].exitTime = 0.5;
    g.transitions[1].curve = Curve::Ease;
    g.states[2].x = 120;
    Graph back;
    QString err;
    ASSERT_TRUE(fromJson(toJson(g), &back, &err)) << err.toStdString();
    EXPECT_EQ(back.entry, QStringLiteral("idle"));
    ASSERT_EQ(back.transitions.size(), 4u);
    EXPECT_EQ(back.transitions[1].mask, QStringList{QStringLiteral("Spine")});
    EXPECT_DOUBLE_EQ(back.transitions[1].exitTime, 0.5);
    EXPECT_EQ(back.transitions[1].curve, Curve::Ease);
    EXPECT_EQ(back.transitions[0].conditions[0].op, Op::Greater);
    EXPECT_DOUBLE_EQ(back.states[2].x, 120.0);

    Graph bad = locomotion();
    bad.transitions[0].to = QStringLiteral("nowhere");
    EXPECT_FALSE(validate(bad, &err));
    bad = locomotion();
    bad.transitions[0].conditions[0].param = QStringLiteral("ghost");
    EXPECT_FALSE(validate(bad, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("ghost")));
    bad = locomotion();
    bad.entry = QStringLiteral("nope");
    EXPECT_FALSE(validate(bad, &err));
    EXPECT_FALSE(fromJson(QJsonObject{{"schema", "other"}}, &back, &err));

    EXPECT_EQ(uniqueStateName(g, QStringLiteral("idle")), QStringLiteral("idle 2"));
    EXPECT_EQ(uniqueTransitionId(g), QStringLiteral("t5"));
}
