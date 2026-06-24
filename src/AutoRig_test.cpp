// Unit tests for AutoRig (#407). The pure-data core (templateJoints /
// fitTemplate) needs no Ogre/GL context, so these run everywhere — unlike
// rigEntity() which needs a loaded mesh (covered by the CLI coverage test
// under Xvfb on CI).

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "AutoRig.h"

namespace {

// Build a synthetic upright "humanoid-ish" point cloud: 2 units tall (y),
// ~1 wide at the shoulders, narrow elsewhere, centred on x/z=0.
std::vector<float> uprightCloud()
{
    std::vector<float> v;
    for (int i = 0; i < 2000; ++i) {
        const float y = (static_cast<float>(i) / 2000.0f) * 2.0f;
        const float w = (y > 1.4f && y < 1.7f) ? 0.9f : 0.35f;  // shoulders bulge
        for (int s = -1; s <= 1; s += 2) {
            v.push_back(s * w * 0.5f);
            v.push_back(y);
            v.push_back(0.0f);
        }
    }
    return v;
}

} // namespace

TEST(AutoRigCore, TemplatesAreNonEmptyAndWellParented)
{
    for (auto t : {AutoRig::Template::Humanoid, AutoRig::Template::Biped,
                   AutoRig::Template::Quadruped, AutoRig::Template::Generic}) {
        const auto js = AutoRig::templateJoints(t);
        ASSERT_FALSE(js.empty());
        // Exactly one root; every non-root parent index is a valid earlier joint.
        int roots = 0;
        for (size_t i = 0; i < js.size(); ++i) {
            if (js[i].parent < 0) { ++roots; continue; }
            EXPECT_GE(js[i].parent, 0);
            EXPECT_LT(static_cast<size_t>(js[i].parent), js.size());
            EXPECT_LT(static_cast<size_t>(js[i].parent), i)
                << "parent must precede child for single-pass bone build";
            // Normalised template coords stay in [0,1].
            for (int a = 0; a < 3; ++a) {
                EXPECT_GE(js[i].pos[a], 0.0);
                EXPECT_LE(js[i].pos[a], 1.0);
            }
        }
        EXPECT_EQ(roots, 1) << "template must have exactly one root";
    }
}

TEST(AutoRigCore, HumanoidHasExpectedBoneCount)
{
    EXPECT_EQ(AutoRig::templateJoints(AutoRig::Template::Humanoid).size(), 19u);
    EXPECT_EQ(AutoRig::templateJoints(AutoRig::Template::Generic).size(), 3u);
}

TEST(AutoRigCore, FitPlacesAllJointsInsideAABB)
{
    const auto cloud = uprightCloud();
    const int n = static_cast<int>(cloud.size() / 3);
    const auto tmpl = AutoRig::templateJoints(AutoRig::Template::Humanoid);

    AutoRig::Options o;
    o.tmpl = AutoRig::Template::Humanoid;
    o.upAxis = 1;
    int recentered = 0;
    const auto placed = AutoRig::fitTemplate(tmpl, cloud.data(), n, o, &recentered);

    ASSERT_EQ(placed.size(), tmpl.size());
    EXPECT_GT(recentered, 0) << "spine/limb-root joints should recentre on a real cloud";

    // Cloud AABB: x in [-0.45, 0.45], y in [0, 2], z == 0.
    for (const auto& j : placed) {
        EXPECT_GE(j.pos[1], -1e-3);
        EXPECT_LE(j.pos[1], 2.0 + 1e-3) << j.name.toStdString() << " y out of AABB";
        EXPECT_GE(j.pos[0], -0.45 - 1e-3);
        EXPECT_LE(j.pos[0],  0.45 + 1e-3) << j.name.toStdString() << " x out of AABB";
    }
}

TEST(AutoRigCore, FitRespectsVerticalOrdering)
{
    const auto cloud = uprightCloud();
    const int n = static_cast<int>(cloud.size() / 3);
    const auto tmpl = AutoRig::templateJoints(AutoRig::Template::Humanoid);
    AutoRig::Options o;
    const auto placed = AutoRig::fitTemplate(tmpl, cloud.data(), n, o, nullptr);

    auto yOf = [&](const QString& name) -> double {
        for (const auto& j : placed) if (j.name == name) return j.pos[1];
        return -1e9;
    };
    // Head above hips above feet.
    EXPECT_GT(yOf("Head"), yOf("Hips"));
    EXPECT_GT(yOf("Hips"), yOf("LeftFoot"));
    EXPECT_GT(yOf("Hips"), yOf("RightFoot"));
    // Symmetric feet stay on opposite sides of centre (x sign preserved).
    EXPECT_GT(yOf("Head"), 1.5);   // head lands in the upper portion
}

TEST(AutoRigCore, FitIsRobustToDegenerateInput)
{
    const auto tmpl = AutoRig::templateJoints(AutoRig::Template::Generic);
    AutoRig::Options o;
    int rc = -1;
    // Null / zero-count → returns the template unchanged, no crash, rc=0.
    const auto p0 = AutoRig::fitTemplate(tmpl, nullptr, 0, o, &rc);
    EXPECT_EQ(p0.size(), tmpl.size());
    EXPECT_EQ(rc, 0);

    // Single degenerate vertex (all same point) → no division blow-up.
    std::vector<float> one = {0.5f, 0.5f, 0.5f};
    const auto p1 = AutoRig::fitTemplate(tmpl, one.data(), 1, o, &rc);
    EXPECT_EQ(p1.size(), tmpl.size());
    for (const auto& j : p1)
        for (int a = 0; a < 3; ++a)
            EXPECT_TRUE(std::isfinite(j.pos[a]));
}

TEST(AutoRigCore, TemplateStringRoundTrip)
{
    using T = AutoRig::Template;
    for (auto t : {T::Humanoid, T::Biped, T::Quadruped, T::Generic})
        EXPECT_EQ(AutoRig::templateFromString(AutoRig::templateToString(t)), t);
    // Unknown → humanoid default; alias "quad".
    EXPECT_EQ(AutoRig::templateFromString("nonsense"), T::Humanoid);
    EXPECT_EQ(AutoRig::templateFromString("quad"), T::Quadruped);
    EXPECT_EQ(AutoRig::templateFromString("HUMANOID"), T::Humanoid);
}

TEST(AutoRigCore, ReportSerialization)
{
    AutoRig::Report r;
    r.applied = true;
    r.meshName = "robot";
    r.templateName = "humanoid";
    r.boneCount = 19;
    r.verticesSampled = 1234;
    r.jointsRecentered = 11;
    const auto j = AutoRig::reportToJson(r);
    EXPECT_TRUE(j["applied"].toBool());
    EXPECT_EQ(j["boneCount"].toInt(), 19);
    EXPECT_EQ(j["template"].toString(), "humanoid");
    EXPECT_FALSE(AutoRig::reportToText(r).isEmpty());

    AutoRig::Report fail;
    fail.applied = false;
    fail.error = "boom";
    EXPECT_TRUE(AutoRig::reportToText(fail).contains("boom"));
}

// ---- Marker-driven fit (#407 Mixamo-style) ------------------------------

namespace {
// Distance between two joint positions.
double jdist(const AutoRig::Joint& a, const AutoRig::Joint& b)
{
    double dx = a.pos[0] - b.pos[0];
    double dy = a.pos[1] - b.pos[1];
    double dz = a.pos[2] - b.pos[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
int jindex(const std::vector<AutoRig::Joint>& js, const QString& name)
{
    for (int i = 0; i < static_cast<int>(js.size()); ++i)
        if (js[i].name == name) return i;
    return -1;
}
} // namespace

TEST(AutoRigMarkers, OrderAndLabelsAreStable)
{
    const auto order = AutoRig::humanoidMarkerOrder();
    ASSERT_EQ(order.size(), 6u);
    EXPECT_EQ(order[0], AutoRig::MarkerId::Chin);
    EXPECT_EQ(order[5], AutoRig::MarkerId::Hips);
    for (auto id : order)
        EXPECT_FALSE(AutoRig::markerLabel(id).isEmpty());
}

TEST(AutoRigMarkers, EmptyMarkersMatchPlainFit)
{
    auto cloud = uprightCloud();
    auto tmpl  = AutoRig::templateJoints(AutoRig::Template::Humanoid);
    AutoRig::Options opts;  // +Y up, humanoid

    int recenterA = 0, recenterB = 0, applied = -1;
    auto plain   = AutoRig::fitTemplate(tmpl, cloud.data(),
                                        static_cast<int>(cloud.size() / 3),
                                        opts, &recenterA);
    auto marked  = AutoRig::fitTemplateWithMarkers(tmpl, cloud.data(),
                                        static_cast<int>(cloud.size() / 3),
                                        {}, opts, &recenterB, &applied);
    ASSERT_EQ(plain.size(), marked.size());
    EXPECT_EQ(applied, 0);
    for (size_t i = 0; i < plain.size(); ++i)
        EXPECT_LT(jdist(plain[i], marked[i]), 1e-6) << "joint " << i;
}

TEST(AutoRigMarkers, WristMarkerLaysWholeArmChainTowardIt)
{
    auto cloud = uprightCloud();
    auto tmpl  = AutoRig::templateJoints(AutoRig::Template::Humanoid);
    AutoRig::Options opts;

    // Skip if this template doesn't expose the named arm chain.
    auto base = AutoRig::fitTemplate(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3), opts);
    const int iShoulder = jindex(base, "LeftShoulder");
    const int iArm      = jindex(base, "LeftArm");
    const int iFore     = jindex(base, "LeftForeArm");
    const int iHand     = jindex(base, "LeftHand");
    if (iShoulder < 0 || iArm < 0 || iFore < 0 || iHand < 0)
        GTEST_SKIP() << "no left-arm chain";

    AutoRig::Marker wrist;
    wrist.id  = AutoRig::MarkerId::LeftWrist;
    wrist.set = true;
    wrist.pos = {1.25, 1.55, 0.10};   // far out from the body

    int applied = 0;
    auto marked = AutoRig::fitTemplateWithMarkers(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3),
                                     {wrist}, opts, nullptr, &applied);
    EXPECT_EQ(applied, 1);

    // Hand lands exactly on the marker.
    EXPECT_LT(std::abs(marked[iHand].pos[0] - wrist.pos[0]), 1e-6);
    EXPECT_LT(std::abs(marked[iHand].pos[1] - wrist.pos[1]), 1e-6);
    EXPECT_LT(std::abs(marked[iHand].pos[2] - wrist.pos[2]), 1e-6);

    // Shoulder (the anchor) is unchanged from the template fit.
    EXPECT_LT(jdist(marked[iShoulder], base[iShoulder]), 1e-6);

    // The intermediate joints lie evenly on the shoulder→hand segment:
    // LeftArm at 1/3, LeftForeArm at 2/3.
    const auto& a = marked[iShoulder].pos;
    for (int k = 0; k < 3; ++k) {
        const double arm13  = a[k] + (wrist.pos[k] - a[k]) * (1.0 / 3.0);
        const double fore23 = a[k] + (wrist.pos[k] - a[k]) * (2.0 / 3.0);
        EXPECT_LT(std::abs(marked[iArm].pos[k]  - arm13),  1e-6) << "arm axis " << k;
        EXPECT_LT(std::abs(marked[iFore].pos[k] - fore23), 1e-6) << "fore axis " << k;
    }

    // The upper arm (LeftArm) actually moved OUT toward the wrist — the bug we
    // fixed was that it stayed at its tucked template x while only the wrist moved.
    EXPECT_GT(std::abs(marked[iArm].pos[0]), std::abs(base[iArm].pos[0]));
}

TEST(AutoRigMarkers, HipsMarkerAnchorsPelvisOnly)
{
    auto cloud = uprightCloud();
    auto tmpl  = AutoRig::templateJoints(AutoRig::Template::Humanoid);
    AutoRig::Options opts;

    auto base = AutoRig::fitTemplate(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3), opts);
    const int iHips = jindex(base, "Hips");
    if (iHips < 0) GTEST_SKIP() << "no Hips joint";

    AutoRig::Marker hips;
    hips.id  = AutoRig::MarkerId::Hips;
    hips.set = true;
    hips.pos = {0.05, 0.9, 0.0};

    int applied = 0;
    auto marked = AutoRig::fitTemplateWithMarkers(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3),
                                     {hips}, opts, nullptr, &applied);
    EXPECT_EQ(applied, 1);
    EXPECT_LT(jdist(marked[iHips], AutoRig::Joint{"", -1, hips.pos}), 1e-6);
}

TEST(AutoRigMarkers, OrderHasTenWithAttachPointsBeforeTips)
{
    const auto order = AutoRig::humanoidMarkerOrder();
    ASSERT_EQ(order.size(), 10u);
    auto pos = [&](AutoRig::MarkerId id) {
        for (size_t i = 0; i < order.size(); ++i) if (order[i] == id) return (int)i;
        return -1;
    };
    // Attach points precede their tips: shoulder→wrist, hip→knee.
    EXPECT_GE(pos(AutoRig::MarkerId::LeftShoulder), 0);
    EXPECT_GE(pos(AutoRig::MarkerId::LeftUpLeg), 0);
    EXPECT_LT(pos(AutoRig::MarkerId::LeftShoulder),  pos(AutoRig::MarkerId::LeftWrist));
    EXPECT_LT(pos(AutoRig::MarkerId::RightShoulder), pos(AutoRig::MarkerId::RightWrist));
    EXPECT_LT(pos(AutoRig::MarkerId::LeftUpLeg),  pos(AutoRig::MarkerId::LeftKnee));
    EXPECT_LT(pos(AutoRig::MarkerId::RightUpLeg), pos(AutoRig::MarkerId::RightKnee));
    EXPECT_FALSE(AutoRig::markerLabel(AutoRig::MarkerId::LeftUpLeg).isEmpty());
    EXPECT_FALSE(AutoRig::markerLabel(AutoRig::MarkerId::LeftShoulder).isEmpty());
}

TEST(AutoRigMarkers, ChinAndHipsLaySpineBetweenThem)
{
    auto cloud = uprightCloud();
    auto tmpl  = AutoRig::templateJoints(AutoRig::Template::Humanoid);
    AutoRig::Options opts;

    auto base = AutoRig::fitTemplate(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3), opts);
    const int iHips  = jindex(base, "Hips");
    const int iSpine = jindex(base, "Spine");
    const int iChest = jindex(base, "Chest");
    const int iNeck  = jindex(base, "Neck");
    const int iHead  = jindex(base, "Head");
    if (iHips < 0 || iSpine < 0 || iChest < 0 || iNeck < 0 || iHead < 0)
        GTEST_SKIP() << "no spine chain";

    AutoRig::Marker hips;
    hips.id = AutoRig::MarkerId::Hips; hips.set = true; hips.pos = {0.0, 0.80, 0.0};
    AutoRig::Marker chin;
    chin.id = AutoRig::MarkerId::Chin; chin.set = true; chin.pos = {0.0, 2.20, 0.0};

    int applied = 0;
    auto marked = AutoRig::fitTemplateWithMarkers(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3),
                                     {hips, chin}, opts, nullptr, &applied);
    EXPECT_EQ(applied, 2);

    // Head=chin, Hips=hips, and Spine/Chest/Neck distributed evenly on the
    // segment: last = 3 spine joints + Head = 4 steps, so Spine@1/4, Chest@2/4,
    // Neck@3/4 between hips and chin.
    EXPECT_LT(jdist(marked[iHead], AutoRig::Joint{"", -1, chin.pos}), 1e-6);
    EXPECT_LT(jdist(marked[iHips], AutoRig::Joint{"", -1, hips.pos}), 1e-6);
    const auto& a = hips.pos;
    auto onSeg = [&](double t) {
        return std::array<double,3>{ a[0]+(chin.pos[0]-a[0])*t,
                                     a[1]+(chin.pos[1]-a[1])*t,
                                     a[2]+(chin.pos[2]-a[2])*t };
    };
    EXPECT_LT(jdist(marked[iSpine], AutoRig::Joint{"", -1, onSeg(1.0/4)}), 1e-6);
    EXPECT_LT(jdist(marked[iChest], AutoRig::Joint{"", -1, onSeg(2.0/4)}), 1e-6);
    EXPECT_LT(jdist(marked[iNeck],  AutoRig::Joint{"", -1, onSeg(3.0/4)}), 1e-6);
}

TEST(AutoRigMarkers, HipsCarriesThighRootsAndKneeLaysLowerLeg)
{
    auto cloud = uprightCloud();
    auto tmpl  = AutoRig::templateJoints(AutoRig::Template::Humanoid);
    AutoRig::Options opts;

    auto base = AutoRig::fitTemplate(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3), opts);
    const int iHips  = jindex(base, "Hips");
    const int iUpLeg = jindex(base, "LeftUpLeg");
    const int iKnee  = jindex(base, "LeftLeg");
    const int iFoot  = jindex(base, "LeftFoot");
    if (iHips < 0 || iUpLeg < 0 || iKnee < 0 || iFoot < 0)
        GTEST_SKIP() << "no left-leg chain";

    AutoRig::Marker hips;
    hips.id  = AutoRig::MarkerId::Hips;
    hips.set = true;
    hips.pos = {0.0, 1.05, 0.0};
    AutoRig::Marker knee;
    knee.id  = AutoRig::MarkerId::LeftKnee;
    knee.set = true;
    knee.pos = {0.40, 0.55, 0.05};

    // Expected thigh-root shift = the hips delta (UpLeg is carried with Hips).
    const auto& bH = base[iHips].pos;
    const std::array<double,3> d = { hips.pos[0]-bH[0], hips.pos[1]-bH[1], hips.pos[2]-bH[2] };
    const std::array<double,3> expUpLeg = { base[iUpLeg].pos[0]+d[0],
                                            base[iUpLeg].pos[1]+d[1],
                                            base[iUpLeg].pos[2]+d[2] };

    int applied = 0;
    auto marked = AutoRig::fitTemplateWithMarkers(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3),
                                     {hips, knee}, opts, nullptr, &applied);
    EXPECT_EQ(applied, 2);

    // Thigh root tracked the hips marker (didn't stay at its template pos).
    EXPECT_LT(jdist(marked[iUpLeg], AutoRig::Joint{"", -1, expUpLeg}), 1e-6);
    // Knee landed on its marker.
    EXPECT_LT(jdist(marked[iKnee], AutoRig::Joint{"", -1, knee.pos}), 1e-6);
    // Foot continues below the knee along thigh→knee (knee + (knee - upLeg)).
    const auto& U = marked[iUpLeg].pos;
    const std::array<double,3> expFoot = { knee.pos[0] + (knee.pos[0]-U[0]),
                                           knee.pos[1] + (knee.pos[1]-U[1]),
                                           knee.pos[2] + (knee.pos[2]-U[2]) };
    EXPECT_LT(jdist(marked[iFoot], AutoRig::Joint{"", -1, expFoot}), 1e-6);
}

TEST(AutoRigMarkers, ShoulderMarkerAnchorsAttachAndArmLaysFromIt)
{
    auto cloud = uprightCloud();
    auto tmpl  = AutoRig::templateJoints(AutoRig::Template::Humanoid);
    AutoRig::Options opts;

    auto base = AutoRig::fitTemplate(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3), opts);
    const int iShoulder = jindex(base, "LeftShoulder");
    const int iArm      = jindex(base, "LeftArm");
    const int iHand     = jindex(base, "LeftHand");
    if (iShoulder < 0 || iArm < 0 || iHand < 0) GTEST_SKIP() << "no left-arm chain";

    AutoRig::Marker shoulder;
    shoulder.id  = AutoRig::MarkerId::LeftShoulder;
    shoulder.set = true;
    shoulder.pos = {0.35, 1.60, 0.0};
    AutoRig::Marker wrist;
    wrist.id  = AutoRig::MarkerId::LeftWrist;
    wrist.set = true;
    wrist.pos = {1.30, 1.55, 0.10};

    int applied = 0;
    auto marked = AutoRig::fitTemplateWithMarkers(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3),
                                     {shoulder, wrist}, opts, nullptr, &applied);
    EXPECT_EQ(applied, 2);

    // Shoulder lands on its marker; the arm chain lays from THAT point, so
    // LeftArm = lerp(shoulderMarker, wristMarker, 1/3).
    EXPECT_LT(jdist(marked[iShoulder], AutoRig::Joint{"", -1, shoulder.pos}), 1e-6);
    for (int k = 0; k < 3; ++k) {
        const double arm13 =
            shoulder.pos[k] + (wrist.pos[k] - shoulder.pos[k]) * (1.0 / 3.0);
        EXPECT_LT(std::abs(marked[iArm].pos[k] - arm13), 1e-6) << "axis " << k;
    }
    EXPECT_LT(jdist(marked[iHand], AutoRig::Joint{"", -1, wrist.pos}), 1e-6);
}

// ---- Inference: unmarked joints derived from marked neighbours ----------

TEST(AutoRigMarkers, ShoulderInferredFromHipsAndChinSpan)
{
    // Mark only chin + hips (no shoulders): each shoulder should be inferred
    // ALONG the hips→head line (never above the head), not left at template.
    auto cloud = uprightCloud();
    auto tmpl  = AutoRig::templateJoints(AutoRig::Template::Humanoid);
    AutoRig::Options opts;
    auto base = AutoRig::fitTemplate(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3), opts);
    const int iLSh  = jindex(base, "LeftShoulder");
    const int iHead = jindex(base, "Head");
    const int iHips = jindex(base, "Hips");
    if (iLSh < 0 || iHead < 0 || iHips < 0) GTEST_SKIP() << "no spine/shoulder";

    AutoRig::Marker hips;
    hips.id = AutoRig::MarkerId::Hips; hips.set = true; hips.pos = {0.0, 0.80, 0.0};
    AutoRig::Marker chin;
    chin.id = AutoRig::MarkerId::Chin; chin.set = true; chin.pos = {0.0, 2.00, 0.0};

    int applied = 0;
    auto m = AutoRig::fitTemplateWithMarkers(tmpl, cloud.data(),
                static_cast<int>(cloud.size() / 3), {hips, chin}, opts, nullptr, &applied);
    EXPECT_EQ(applied, 2);
    // Shoulder up-coord lies strictly between hips and head (axis 1 = +Y).
    EXPECT_GT(m[iLSh].pos[1], hips.pos[1]);
    EXPECT_LT(m[iLSh].pos[1], chin.pos[1]);
}

TEST(AutoRigMarkers, HipsInferredFromUpLegsWhenUnmarked)
{
    // Mark only the two up-legs (no hips): pelvis should land at their midpoint
    // plus the template socket→pelvis rise — not at the template hips.
    auto cloud = uprightCloud();
    auto tmpl  = AutoRig::templateJoints(AutoRig::Template::Humanoid);
    AutoRig::Options opts;
    auto base = AutoRig::fitTemplate(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3), opts);
    const int iHips = jindex(base, "Hips");
    if (iHips < 0) GTEST_SKIP() << "no hips";

    AutoRig::Marker lu, ru;
    lu.id = AutoRig::MarkerId::LeftUpLeg;  lu.set = true; lu.pos = {0.30, 0.70, 0.0};
    ru.id = AutoRig::MarkerId::RightUpLeg; ru.set = true; ru.pos = {-0.30, 0.70, 0.0};

    int applied = 0;
    auto m = AutoRig::fitTemplateWithMarkers(tmpl, cloud.data(),
                static_cast<int>(cloud.size() / 3), {lu, ru}, opts, nullptr, &applied);
    EXPECT_EQ(applied, 2);
    // Pelvis centred between the sockets (x ≈ 0) and lifted above them (y > 0.70).
    EXPECT_LT(std::abs(m[iHips].pos[0] - 0.0), 1e-6);
    EXPECT_GT(m[iHips].pos[1], 0.70);
}

TEST(AutoRigMarkers, UnmarkedShoulderMirrorsMarkedOne)
{
    // Mark one shoulder; the other should mirror across the body (opposite
    // side-axis sign, ~symmetric), not stay at the template.
    auto cloud = uprightCloud();
    auto tmpl  = AutoRig::templateJoints(AutoRig::Template::Humanoid);
    AutoRig::Options opts;
    auto base = AutoRig::fitTemplate(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3), opts);
    const int iLSh = jindex(base, "LeftShoulder");
    const int iRSh = jindex(base, "RightShoulder");
    if (iLSh < 0 || iRSh < 0) GTEST_SKIP() << "no shoulders";

    AutoRig::Marker ls;
    ls.id = AutoRig::MarkerId::LeftShoulder; ls.set = true; ls.pos = {0.55, 1.50, 0.10};

    int applied = 0;
    auto m = AutoRig::fitTemplateWithMarkers(tmpl, cloud.data(),
                static_cast<int>(cloud.size() / 3), {ls}, opts, nullptr, &applied);
    EXPECT_EQ(applied, 1);
    EXPECT_LT(jdist(m[iLSh], AutoRig::Joint{"", -1, ls.pos}), 1e-6);
    // Right shoulder is on the opposite side (x sign flipped relative to L).
    EXPECT_LT(m[iRSh].pos[0], 0.0);
    // Same height + depth as the marked one (pure mirror across the side axis).
    EXPECT_LT(std::abs(m[iRSh].pos[1] - ls.pos[1]), 1e-6);
}

TEST(AutoRigMarkers, ShoulderMarkedWristSkippedStillLaysArm)
{
    // Shoulder marked, wrist skipped: the hand should reach out from the marked
    // shoulder by the template arm vector (not collapse onto the shoulder).
    auto cloud = uprightCloud();
    auto tmpl  = AutoRig::templateJoints(AutoRig::Template::Humanoid);
    AutoRig::Options opts;
    auto base = AutoRig::fitTemplate(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3), opts);
    const int iSh   = jindex(base, "LeftShoulder");
    const int iHand = jindex(base, "LeftHand");
    if (iSh < 0 || iHand < 0) GTEST_SKIP() << "no left arm";
    const double tArmLen = std::sqrt(
        std::pow(base[iHand].pos[0]-base[iSh].pos[0],2) +
        std::pow(base[iHand].pos[1]-base[iSh].pos[1],2) +
        std::pow(base[iHand].pos[2]-base[iSh].pos[2],2));

    AutoRig::Marker ls;
    ls.id = AutoRig::MarkerId::LeftShoulder; ls.set = true; ls.pos = {0.60, 1.55, 0.0};

    int applied = 0;
    auto m = AutoRig::fitTemplateWithMarkers(tmpl, cloud.data(),
                static_cast<int>(cloud.size() / 3), {ls}, opts, nullptr, &applied);
    EXPECT_EQ(applied, 1);
    EXPECT_LT(jdist(m[iSh], AutoRig::Joint{"", -1, ls.pos}), 1e-6);
    // Hand is ~one template arm-length away from the marked shoulder.
    const double handLen = std::sqrt(
        std::pow(m[iHand].pos[0]-ls.pos[0],2) +
        std::pow(m[iHand].pos[1]-ls.pos[1],2) +
        std::pow(m[iHand].pos[2]-ls.pos[2],2));
    EXPECT_GT(handLen, tArmLen * 0.5);
}

TEST(AutoRigMarkers, UpLegSetKneeSkippedClampsFootToMeshFloor)
{
    // Up-leg marked, knee skipped: the foot must land at (not below) the mesh
    // floor, and the knee must sit between the up-leg and the foot. Previously
    // the template thigh-vector extrapolation pushed the foot past the mesh.
    auto cloud = uprightCloud();   // y in [0, 2] → floor = 0
    auto tmpl  = AutoRig::templateJoints(AutoRig::Template::Humanoid);
    AutoRig::Options opts;
    auto base = AutoRig::fitTemplate(tmpl, cloud.data(),
                                     static_cast<int>(cloud.size() / 3), opts);
    const int iUp   = jindex(base, "LeftUpLeg");
    const int iKnee = jindex(base, "LeftLeg");
    const int iFoot = jindex(base, "LeftFoot");
    if (iUp < 0 || iKnee < 0 || iFoot < 0) GTEST_SKIP() << "no left leg";

    AutoRig::Marker up;
    up.id = AutoRig::MarkerId::LeftUpLeg; up.set = true; up.pos = {0.30, 0.90, 0.0};

    int applied = 0;
    auto m = AutoRig::fitTemplateWithMarkers(tmpl, cloud.data(),
                static_cast<int>(cloud.size() / 3), {up}, opts, nullptr, &applied);
    EXPECT_EQ(applied, 1);

    const double floor = 0.0;
    // Foot sits on (not below) the mesh floor.
    EXPECT_GE(m[iFoot].pos[1], floor - 1e-6);
    EXPECT_LT(std::abs(m[iFoot].pos[1] - floor), 1e-6);
    // Knee strictly between the up-leg (0.90) and the foot (0.0) in height.
    EXPECT_LT(m[iKnee].pos[1], m[iUp].pos[1]);
    EXPECT_GT(m[iKnee].pos[1], m[iFoot].pos[1]);
}
