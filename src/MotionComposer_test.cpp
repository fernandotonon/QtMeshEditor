#include <gtest/gtest.h>

#include "MotionComposer.h"
#include "MotionLibrary.h"

#include <QByteArray>

#include <cmath>
#include <cstdio>

// MotionComposer is Ogre-free (like MotionLibrary), so these run headless with
// no GL/network. They pin the three stages the composed path is made of:
// parsing, take compilation, and seam stitching.

namespace {

// A library with distinguishable poses per action so a stitch is observable:
// each clip's joints rotate by a per-clip angle about X, ramping across frames.
QByteArray libWithActions(const std::vector<std::pair<QString, int>>& actions)
{
    auto poseAt = [](double ang) {
        const double h = ang * 0.5;
        const double x = std::sin(h), w = std::cos(h);
        QByteArray p = "[";
        for (int j = 0; j < 22; ++j) {
            if (j) p += ",";
            p += "[" + QByteArray::number(x, 'g', 8) + ",0,0,"
               + QByteArray::number(w, 'g', 8) + "]";
        }
        return p + "]";
    };
    QByteArray json = "{\"schema\":\"qtmesh-motion-library-v1\",\"fps\":30,";
    json += "\"joints\":[";
    const char* J[] = {"hip","abdomen","chest","neck","neck1","head","rcollar",
        "rshoulder","relbow","rhand","lcollar","lshoulder","lelbow","lhand",
        "rbuttock","rhip","rknee","rfoot","lbuttock","lhip","lknee","lfoot"};
    for (int j = 0; j < 22; ++j) { if (j) json += ","; json += "\""; json += J[j]; json += "\""; }
    json += "],\"clips\":[";
    for (size_t c = 0; c < actions.size(); ++c) {
        if (c) json += ",";
        const int frames = actions[c].second;
        QByteArray fr = "[";
        for (int f = 0; f < frames; ++f) {
            if (f) fr += ",";
            // each clip occupies its own angular band; frames ramp within it
            // Bands are widely separated so two clips can NEVER share a
            // pose: otherwise bestCut finds a perfect match, the seam step
            // is exactly 0, and any assertion about blending is vacuous.
            fr += poseAt(4.0 * double(c) + 0.02 * double(f));
        }
        fr += "]";
        json += "{\"action\":\"" + actions[c].first.toUtf8()
              + "\",\"source\":\"test " + QByteArray::number(int(c))
              + "\",\"quats\":" + fr + "}";
    }
    json += "]}";
    return json;
}

MotionLibrary makeLib(const std::vector<std::pair<QString, int>>& actions)
{
    MotionLibrary lib;
    EXPECT_TRUE(lib.loadFromJson(libWithActions(actions))) << lib.error().toStdString();
    return lib;
}

} // namespace

// ---- parsing ---------------------------------------------------------------

TEST(MotionComposer, ParsesMultiStepPromptInOrder)
{
    const MotionLibrary lib = makeLib({{"walk", 20}, {"sit", 20},
                                       {"wave", 20}, {"idle", 20}});
    // The epic's own acceptance prompt.
    const auto s = MotionComposer::parse(
        QStringLiteral("walk forward, sit down, wave twice, then stand up"), lib);
    ASSERT_EQ(s.steps.size(), 4u);
    EXPECT_EQ(s.steps[0].action.toStdString(), "walk");
    EXPECT_EQ(s.steps[1].action.toStdString(), "sit");
    EXPECT_EQ(s.steps[2].action.toStdString(), "wave");
    EXPECT_EQ(s.steps[3].action.toStdString(), "idle");   // "stand" -> idle
    EXPECT_EQ(s.steps[2].repeat, 2);                      // "twice"
}

TEST(MotionComposer, SingleActionPromptYieldsOneStep)
{
    // Composing must degrade exactly to the old single-clip behaviour.
    const MotionLibrary lib = makeLib({{"walk", 12}, {"run", 12}});
    const auto s = MotionComposer::parse(QStringLiteral("walking forward"), lib);
    ASSERT_EQ(s.steps.size(), 1u);
    EXPECT_EQ(s.steps[0].action.toStdString(), "walk");
    EXPECT_EQ(s.steps[0].repeat, 1);
}

TEST(MotionComposer, ParsesRepeatAndDuration)
{
    const MotionLibrary lib = makeLib({{"wave", 30}, {"walk", 30}});
    const auto s = MotionComposer::parse(
        QStringLiteral("walk for 2 seconds then wave 3 times"), lib);
    ASSERT_EQ(s.steps.size(), 2u);
    EXPECT_NEAR(s.steps[0].durationS, 2.0f, 1e-4f);
    EXPECT_EQ(s.steps[1].repeat, 3);
}

TEST(MotionComposer, ReportsUnresolvedFragments)
{
    const MotionLibrary lib = makeLib({{"walk", 10}});
    const auto s = MotionComposer::parse(
        QStringLiteral("walk then juggle chainsaws"), lib);
    ASSERT_EQ(s.steps.size(), 1u);
    ASSERT_EQ(s.unresolved.size(), 1u);      // reported, never silently played
    EXPECT_TRUE(s.unresolved[0].contains(QStringLiteral("juggle")));
}

TEST(MotionComposer, ParsesJsonScript)
{
    const MotionLibrary lib = makeLib({{"walk", 10}, {"wave", 10}});
    const QByteArray js = R"({"steps":[{"action":"walk","duration_s":1.5},
                                       {"action":"wave","repeat":2}]})";
    const auto s = MotionComposer::parseJson(js, lib);
    ASSERT_EQ(s.steps.size(), 2u);
    EXPECT_NEAR(s.steps[0].durationS, 1.5f, 1e-4f);
    EXPECT_EQ(s.steps[1].repeat, 2);
}

TEST(MotionComposer, JsonRejectsInventedAction)
{
    const MotionLibrary lib = makeLib({{"walk", 10}});
    const auto s = MotionComposer::parseJson(
        R"({"steps":[{"action":"teleport"}]})", lib);
    EXPECT_TRUE(s.steps.empty());
    ASSERT_EQ(s.unresolved.size(), 1u);
}

// ---- pure helpers ----------------------------------------------------------

TEST(MotionComposer, PoseDistanceIsZeroForIdenticalPose)
{
    std::vector<std::array<float, 4>> a(22, {0.0f, 0.0f, 0.0f, 1.0f});
    EXPECT_NEAR(MotionComposer::poseDistance(a, a), 0.0, 1e-9);
}

TEST(MotionComposer, PoseDistanceIgnoresQuaternionSign)
{
    // q and -q are the SAME rotation; a sign-sensitive metric would rank an
    // identical pose as maximally distant and pick a nonsense cut point.
    std::vector<std::array<float, 4>> a(22, {0.0f, 0.0f, 0.0f, 1.0f});
    std::vector<std::array<float, 4>> b(22, {0.0f, 0.0f, 0.0f, -1.0f});
    EXPECT_NEAR(MotionComposer::poseDistance(a, b), 0.0, 1e-6);
}

TEST(MotionComposer, BestCutFindsMatchingPosePair)
{
    // Take A ends on pose P; take B contains P at index 2. The cut must land on
    // that pair rather than defaulting to (last, first). Every other frame gets
    // a DISTINCT angle so exactly one pair matches — with repeated filler poses
    // several pairs tie at distance 0 and the assertion would be meaningless.
    auto poseAt = [](float ang) {
        const float h = ang * 0.5f;
        return std::vector<std::array<float, 4>>(
            22, std::array<float, 4>{std::sin(h), 0.0f, 0.0f, std::cos(h)});
    };
    const float target = 1.1f;
    std::vector<std::vector<std::array<float, 4>>> A{
        poseAt(0.10f), poseAt(0.20f), poseAt(0.30f), poseAt(target)};
    std::vector<std::vector<std::array<float, 4>>> B{
        poseAt(0.50f), poseAt(0.60f), poseAt(target), poseAt(0.80f)};

    const auto [ea, sb] = MotionComposer::bestCut(A, B, 4);
    EXPECT_EQ(ea, 3);
    EXPECT_EQ(sb, 2);
}

TEST(MotionComposer, SlerpEndpointsAndMidpoint)
{
    const std::array<float, 4> a{0.0f, 0.0f, 0.0f, 1.0f};
    const std::array<float, 4> b{0.7071068f, 0.0f, 0.0f, 0.7071068f};
    const auto at0 = MotionComposer::slerp(a, b, 0.0f);
    EXPECT_NEAR(at0[3], 1.0f, 1e-4f);
    const auto at1 = MotionComposer::slerp(a, b, 1.0f);
    EXPECT_NEAR(at1[0], b[0], 1e-4f);
    const auto mid = MotionComposer::slerp(a, b, 0.5f);
    // Halfway between 0 and 90 degrees is 45.
    EXPECT_NEAR(2.0f * std::acos(std::clamp(mid[3], -1.0f, 1.0f)),
                static_cast<float>(M_PI) / 4.0f, 1e-3f);
}

// ---- composition -----------------------------------------------------------

TEST(MotionComposer, ComposesMultiStepIntoOneContinuousClip)
{
    const MotionLibrary lib = makeLib({{"walk", 20}, {"sit", 20}, {"wave", 20}});
    const auto script = MotionComposer::parse(
        QStringLiteral("walk then sit then wave"), lib);
    ASSERT_EQ(script.steps.size(), 3u);

    const auto comp = MotionComposer::compose(script, lib);
    ASSERT_TRUE(comp.ok) << comp.error.toStdString();
    EXPECT_EQ(comp.actions.size(), 3u);
    EXPECT_EQ(comp.seamFrames.size(), 2u);       // two junctions
    EXPECT_EQ(comp.jointCount, 22);
    // Longer than any single take: the steps really were concatenated.
    EXPECT_GT(comp.frames(), 20);
    for (const auto& pose : comp.quats) EXPECT_EQ(pose.size(), 22u);
}

TEST(MotionComposer, SeamBlendSmoothsTheJunction)
{
    // Isolate the BLEND. Two takes whose poses never coincide, so bestCut
    // cannot hide the discontinuity by finding an already-matching pair: any
    // junction is a real jump, and only the crossfade can soften it.
    //
    // NB comparing compose(blend=6) against compose(blend=0) does NOT isolate
    // blending — blendFrames also widens bestCut's search window, so the two
    // runs pick different seams. Here the window is held fixed and only the
    // blend length differs.
    auto poseAt = [](float ang) {
        const float h = ang * 0.5f;
        return std::vector<std::array<float, 4>>(
            22, std::array<float, 4>{std::sin(h), 0.0f, 0.0f, std::cos(h)});
    };
    // A sits near 0 rad throughout; B sits near 1.5 rad throughout.
    std::vector<std::vector<std::array<float, 4>>> A, B;
    for (int f = 0; f < 12; ++f) A.push_back(poseAt(0.01f * float(f)));
    for (int f = 0; f < 12; ++f) B.push_back(poseAt(1.50f + 0.01f * float(f)));

    auto worstStep = [](const std::vector<std::vector<std::array<float, 4>>>& c) {
        double worst = 0.0;
        for (size_t f = 1; f < c.size(); ++f)
            worst = std::max(worst, MotionComposer::poseDistance(c[f - 1], c[f]));
        return worst;
    };

    // Hard cut: splice B straight onto A.
    std::vector<std::vector<std::array<float, 4>>> hard = A;
    for (const auto& p : B) hard.push_back(p);

    // Blended: ramp across the first `blend` frames of B, the same rule
    // compose() applies at each junction.
    const int blend = 6;
    std::vector<std::vector<std::array<float, 4>>> soft = A;
    for (size_t k = 0; k < B.size(); ++k) {
        std::vector<std::array<float, 4>> pose = B[k];
        if (static_cast<int>(k) < blend) {
            const float t = float(k + 1) / float(blend + 1);
            const auto& prev = soft.back();
            for (size_t j = 0; j < pose.size(); ++j)
                pose[j] = MotionComposer::slerp(prev[j], pose[j], t);
        }
        soft.push_back(std::move(pose));
    }

    EXPECT_LT(worstStep(soft) * 2.0, worstStep(hard));
}

TEST(MotionComposer, ComposeAppliesTheBlendAtItsSeams)
{
    // The rule is tested above; this pins that compose() actually APPLIES it.
    // Two actions whose poses are far apart (the fixture puts each clip in its
    // own angular band), so the junction is a genuine jump that only the
    // in-compose crossfade can soften. Without it the first post-seam frame
    // lands on B's raw pose; with it that frame sits partway between.
    const MotionLibrary lib = makeLib({{"walk", 16}, {"sit", 16}});
    const auto script = MotionComposer::parse(QStringLiteral("walk then sit"), lib);
    ASSERT_EQ(script.steps.size(), 2u);

    const auto c = MotionComposer::compose(script, lib, /*blendFrames=*/6);
    ASSERT_TRUE(c.ok) << c.error.toStdString();
    ASSERT_EQ(c.seamFrames.size(), 1u);
    const int seam = c.seamFrames[0];
    ASSERT_GT(seam, 0);
    ASSERT_LT(seam, c.frames());

    // The step ACROSS the seam must be smaller than the gap between the two
    // takes' own poses — i.e. the crossfade really interpolated.
    const double acrossSeam =
        MotionComposer::poseDistance(c.quats[size_t(seam - 1)],
                                     c.quats[size_t(seam)]);
    // Compare against the raw distance the un-blended splice would have had:
    // the last pre-seam pose vs the take-B pose 'blend' frames later, which the
    // ramp has fully reached by then.
    const int after = std::min(c.frames() - 1, seam + 6);
    const double fullGap =
        MotionComposer::poseDistance(c.quats[size_t(seam - 1)],
                                     c.quats[size_t(after)]);
    EXPECT_LT(acrossSeam, fullGap)
        << "seam step " << acrossSeam << " should be well under the "
        << fullGap << " gap the blend ramps across";
}

TEST(MotionComposer, RepeatLengthensTheStep)
{
    const MotionLibrary lib = makeLib({{"wave", 10}});
    const auto once = MotionComposer::compose(
        MotionComposer::parse(QStringLiteral("wave"), lib), lib);
    const auto twice = MotionComposer::compose(
        MotionComposer::parse(QStringLiteral("wave twice"), lib), lib);
    ASSERT_TRUE(once.ok);
    ASSERT_TRUE(twice.ok);
    EXPECT_GT(twice.frames(), once.frames());
}

TEST(MotionComposer, DurationTrimsTheTake)
{
    const MotionLibrary lib = makeLib({{"walk", 60}});   // 2 s at 30 fps
    const auto full = MotionComposer::compose(
        MotionComposer::parse(QStringLiteral("walk"), lib), lib);
    const auto cut = MotionComposer::compose(
        MotionComposer::parse(QStringLiteral("walk for 1 second"), lib), lib);
    ASSERT_TRUE(full.ok);
    ASSERT_TRUE(cut.ok);
    EXPECT_EQ(full.frames(), 60);
    EXPECT_EQ(cut.frames(), 30);
}

TEST(MotionComposer, EmptyScriptFailsCleanly)
{
    const MotionLibrary lib = makeLib({{"walk", 10}});
    const auto comp = MotionComposer::compose(
        MotionComposer::parse(QStringLiteral("juggle chainsaws"), lib), lib);
    EXPECT_FALSE(comp.ok);
    EXPECT_FALSE(comp.error.isEmpty());
}

// ---- surface-facing selection ----------------------------------------------

TEST(MotionComposer, SelectionFallsBackToSingleClipForOneAction)
{
    // A one-action prompt must keep the EXACT shipped behaviour, including the
    // finger side-channel that a stitched multi-take clip cannot carry.
    const MotionLibrary lib = makeLib({{"walk", 20}, {"wave", 20}});
    const auto sel = MotionComposer::selectForPrompt(
        QStringLiteral("walk forward"), lib);
    ASSERT_TRUE(sel.ok) << sel.error.toStdString();
    EXPECT_FALSE(sel.composed);
    EXPECT_EQ(sel.action.toStdString(), "walk");
    EXPECT_EQ(sel.quats.size(), 20u);     // the take, untouched
}

TEST(MotionComposer, SelectionComposesMultiStepPrompt)
{
    const MotionLibrary lib = makeLib({{"walk", 20}, {"sit", 20}, {"wave", 20}});
    const auto sel = MotionComposer::selectForPrompt(
        QStringLiteral("walk then sit then wave"), lib);
    ASSERT_TRUE(sel.ok) << sel.error.toStdString();
    EXPECT_TRUE(sel.composed);
    EXPECT_EQ(sel.steps.size(), 3u);
    // Clip name reflects the sequence so different prompts don't collide.
    EXPECT_EQ(sel.action.toStdString(), "walk_sit_wave");
    EXPECT_GT(sel.quats.size(), 20u);
    // Fingers are intentionally dropped across a composition.
    EXPECT_TRUE(sel.fingers.empty());
}

TEST(MotionComposer, SelectionAcceptsAJsonScript)
{
    const MotionLibrary lib = makeLib({{"walk", 20}, {"wave", 20}});
    const auto sel = MotionComposer::selectForPrompt(
        QString(), lib, R"({"steps":[{"action":"walk"},{"action":"wave"}]})");
    ASSERT_TRUE(sel.ok) << sel.error.toStdString();
    EXPECT_TRUE(sel.composed);
    EXPECT_EQ(sel.steps.size(), 2u);
}

TEST(MotionComposer, SelectionFailsCleanlyOnUnknownPrompt)
{
    const MotionLibrary lib = makeLib({{"walk", 10}});
    const auto sel = MotionComposer::selectForPrompt(
        QStringLiteral("juggle chainsaws"), lib);
    EXPECT_FALSE(sel.ok);
    EXPECT_FALSE(sel.error.isEmpty());
}
