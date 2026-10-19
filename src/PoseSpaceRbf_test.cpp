#include <gtest/gtest.h>

#include "PoseSpaceRbf.h"

#include <QBuffer>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>

using namespace PoseSpace;

namespace { constexpr double kPi = 3.14159265358979323846; }

namespace {
Quat aboutZ(double deg)
{
    const double h = deg * kPi / 360.0;
    return {std::cos(h), 0, 0, std::sin(h)};
}
}  // namespace

TEST(PoseSpaceRbf, AngleBetweenIsSignIndependentAndGeodesic)
{
    EXPECT_NEAR(angleBetween(aboutZ(0), aboutZ(90)), kPi / 2, 1e-9);
    const Quat a = aboutZ(40);
    const Quat neg{-a[0], -a[1], -a[2], -a[3]};   // the same rotation
    EXPECT_NEAR(angleBetween(a, neg), 0.0, 1e-9);
}

TEST(PoseSpaceRbf, LinearSolveMatchesAKnownSystem)
{
    // [2 1; 1 3] x = [3; 5]  ->  x = [0.8, 1.4]
    std::vector<double> B{3, 5};
    ASSERT_TRUE(solveLinear({2, 1, 1, 3}, B, 2, 1));
    EXPECT_NEAR(B[0], 0.8, 1e-12);
    EXPECT_NEAR(B[1], 1.4, 1e-12);
    std::vector<double> C{1, 1};
    EXPECT_FALSE(solveLinear({1, 2, 2, 4}, C, 2, 1));   // singular
}

TEST(PoseSpaceRbf, EverySamplePoseReproducesItsOwnOffsets)
{
    Set s;
    s.drivers = {QStringLiteral("elbow")};
    for (double deg : {30.0, 60.0, 90.0, 120.0}) {
        Sample sm;
        sm.rotations = {aboutZ(deg)};
        sm.vertices = {0, 2};
        sm.deltas = {float(deg), 0.f, 0.f, 0.f, float(-deg), 0.f};
        s.samples.push_back(sm);
    }
    QString err;
    ASSERT_TRUE(s.solve(&err)) << err.toStdString();
    for (const Sample& sm : s.samples) {
        const std::vector<float> d = s.evaluate(sm.rotations, 3);
        EXPECT_NEAR(d[0], sm.deltas[0], 1e-2);
        EXPECT_NEAR(d[6], sm.deltas[3 + 0 + 0 + 0] * 0 + sm.deltas[3], 1e-2);
        EXPECT_NEAR(d[3], 0.0f, 1e-2);   // an untouched vertex stays put
    }
}

TEST(PoseSpaceRbf, TheBindPoseHasNoOffsetAndBetweenSamplesItBlends)
{
    Set s;
    s.drivers = {QStringLiteral("elbow")};
    for (double deg : {45.0, 90.0}) {
        Sample sm;
        sm.rotations = {aboutZ(deg)};
        sm.vertices = {0};
        sm.deltas = {float(deg), 0.f, 0.f};
        s.samples.push_back(sm);
    }
    ASSERT_TRUE(s.solve());
    EXPECT_NEAR(s.evaluate({aboutZ(0)}, 1)[0], 0.0f, 1e-2);                 // identity: nothing
    const float mid = s.evaluate({aboutZ(67.5)}, 1)[0];
    EXPECT_GT(mid, 45.0f);                                                  // between the two samples
    EXPECT_LT(mid, 90.0f);
}

TEST(PoseSpaceRbf, MismatchedSetsAreRefusedWithAReason)
{
    Set s;
    QString err;
    EXPECT_FALSE(s.solve(&err));
    EXPECT_FALSE(err.isEmpty());
    s.drivers = {QStringLiteral("a"), QStringLiteral("b")};
    Sample sm;
    sm.rotations = {aboutZ(10)};   // one rotation for two drivers
    s.samples.push_back(sm);
    err.clear();
    EXPECT_FALSE(s.solve(&err));
    EXPECT_NE(err.indexOf(QStringLiteral("rotations")), -1);
}

TEST(PoseSpaceRbf, JsonRoundTripKeepsTheBehaviour)
{
    Set s;
    s.name = QStringLiteral("elbow_l");
    s.drivers = {QStringLiteral("lowerarm_l")};
    for (double deg : {50.0, 100.0}) {
        Sample sm;
        sm.rotations = {aboutZ(deg)};
        sm.vertices = {1};
        sm.deltas = {0.f, float(deg) * 0.01f, 0.f};
        s.samples.push_back(sm);
    }
    ASSERT_TRUE(s.solve());
    Set back;
    QString err;
    ASSERT_TRUE(Set::fromJson(s.toJson(), &back, &err)) << err.toStdString();
    const std::vector<Quat> q{aboutZ(75)};
    EXPECT_NEAR(back.evaluate(q, 2)[4], s.evaluate(q, 2)[4], 1e-5);
    QJsonObject bad = s.toJson();
    bad.insert(QStringLiteral("schema"), QStringLiteral("nope"));
    EXPECT_FALSE(Set::fromJson(bad, &back, &err));
}

TEST(PoseSpaceRbf, JsonRejectsFractionalOrOutOfRangeVertexIndices)
{
    Set s;
    s.drivers = {QStringLiteral("a")};
    Sample sm;
    sm.rotations = {aboutZ(30)};
    sm.vertices = {1};
    sm.deltas = {0.f, 0.f, 0.f};
    s.samples.push_back(sm);
    ASSERT_TRUE(s.solve());
    for (double bad : {1.5, 5e9, -1.0}) {
        QJsonObject o = s.toJson();
        QJsonArray samples = o.value(QStringLiteral("samples")).toArray();
        QJsonObject so = samples[0].toObject();
        so.insert(QStringLiteral("vertices"), QJsonArray{bad});
        samples[0] = so;
        o.insert(QStringLiteral("samples"), samples);
        Set back;
        QString err;
        EXPECT_FALSE(Set::fromJson(o, &back, &err)) << bad;
        EXPECT_FALSE(err.isEmpty());
    }
}

namespace {
Set sampleSet()
{
    Set s;
    s.name = QStringLiteral("elbow_l");
    s.drivers = {QStringLiteral("lowerarm_l")};
    for (double deg : {40.0, 80.0}) {
        Sample sm;
        sm.rotations = {aboutZ(deg)};
        sm.vertices = {2, 7};
        sm.deltas = {0.1f, 0.2f, 0.3f, float(deg) * 0.01f, 0.f, -0.5f};
        s.samples.push_back(sm);
    }
    s.solve();
    return s;
}
}  // namespace

TEST(PoseSpaceRbf, BinaryRoundTripIsExactAndSmallerThanJson)
{
    const Set s = sampleSet();
    QByteArray bytes;
    QBuffer w(&bytes);
    ASSERT_TRUE(w.open(QIODevice::WriteOnly));
    QString err;
    ASSERT_TRUE(writeBinary(&w, {s}, &err)) << err.toStdString();
    w.close();
    QBuffer r(&bytes);
    ASSERT_TRUE(r.open(QIODevice::ReadOnly));
    std::vector<Set> back;
    ASSERT_TRUE(readBinary(&r, &back, &err)) << err.toStdString();
    ASSERT_EQ(back.size(), 1u);
    EXPECT_EQ(back[0].name, s.name);
    EXPECT_EQ(back[0].samples.size(), s.samples.size());
    const std::vector<Quat> q{aboutZ(60)};
    for (size_t i = 0; i < 24; ++i) EXPECT_FLOAT_EQ(back[0].evaluate(q, 8)[i], s.evaluate(q, 8)[i]);
    EXPECT_LT(bytes.size(), QJsonDocument(s.toJson()).toJson(QJsonDocument::Compact).size());
}

TEST(PoseSpaceRbf, BinaryRejectsTruncatedAndForeignStreams)
{
    const Set s = sampleSet();
    QByteArray bytes;
    QBuffer w(&bytes);
    w.open(QIODevice::WriteOnly);
    ASSERT_TRUE(writeBinary(&w, {s}));
    w.close();
    std::vector<Set> back;
    QString err;
    for (int cut : {0, 3, 9, int(bytes.size()) / 2, int(bytes.size()) - 1}) {
        QByteArray part = bytes.left(cut);
        QBuffer r(&part);
        r.open(QIODevice::ReadOnly);
        EXPECT_FALSE(readBinary(&r, &back, &err)) << cut;
        EXPECT_FALSE(err.isEmpty());
    }
    QByteArray junk("NOTPSD-and-more-bytes");
    QBuffer rj(&junk);
    rj.open(QIODevice::ReadOnly);
    EXPECT_FALSE(readBinary(&rj, &back, &err));
}

TEST(PoseSpaceRbf, ChannelNamesRoundTripTheirPrefix)
{
    EXPECT_TRUE(isChannelName(channelName(QStringLiteral("calf_l"), 3)));
    EXPECT_FALSE(isChannelName(QStringLiteral("jawOpen")));
}
