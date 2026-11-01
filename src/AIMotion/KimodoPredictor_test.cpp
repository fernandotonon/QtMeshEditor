#include <gtest/gtest.h>

#include "AIMotionCLI.h"
#include "KimodoPredictor.h"

#include <QJsonArray>
#include <QJsonDocument>

using namespace AIMotion;

TEST(KimodoPredictor, ArgumentsCarryEverySettingAndNoCredentials)
{
    KimodoPredictor::Options o;
    o.segments = {{QStringLiteral("A person walks forward"), 3.0}, {QStringLiteral("waves with the right hand"), 2.5}};
    o.numSamples = 3;
    o.hasSeed = true;
    o.seed = 7;
    o.rootMotion = false;
    o.postprocess = false;
    o.textEncoderUrl = QStringLiteral("http://127.0.0.1:9550");
    o.textEncoderQuant = QStringLiteral("int4");
    const QStringList a = KimodoPredictor::buildArguments(o, QStringLiteral("/tmp/out"));

    const int seg = a.indexOf(QStringLiteral("--segments"));
    ASSERT_GE(seg, 0);
    const QJsonArray segs = QJsonDocument::fromJson(a[seg + 1].toUtf8()).array();
    ASSERT_EQ(segs.size(), 2);
    EXPECT_EQ(segs[1].toObject().value(QStringLiteral("text")).toString(), QStringLiteral("waves with the right hand"));
    EXPECT_DOUBLE_EQ(segs[1].toObject().value(QStringLiteral("duration")).toDouble(), 2.5);
    EXPECT_EQ(a[a.indexOf(QStringLiteral("--num-samples")) + 1], QStringLiteral("3"));
    EXPECT_EQ(a[a.indexOf(QStringLiteral("--seed")) + 1], QStringLiteral("7"));
    EXPECT_TRUE(a.contains(QStringLiteral("--no-root-motion")));
    EXPECT_TRUE(a.contains(QStringLiteral("--no-postprocess")));
    EXPECT_EQ(a[a.indexOf(QStringLiteral("--model")) + 1], QStringLiteral("Kimodo-SOMA-RP-v1.2"));
    EXPECT_EQ(a[a.indexOf(QStringLiteral("--text-encoder-quant")) + 1], QStringLiteral("int4"));
    for (const QString& s : a) EXPECT_FALSE(s.contains(QStringLiteral("hf_"), Qt::CaseInsensitive)) << "no token on argv";
}

TEST(KimodoPredictor, NoSeedMeansNoSeedArgument)
{
    KimodoPredictor::Options o;
    o.segments = {{QStringLiteral("jump"), 2.0}};
    const QStringList a = KimodoPredictor::buildArguments(o, QStringLiteral("/tmp/out"));
    EXPECT_FALSE(a.contains(QStringLiteral("--seed")));
    EXPECT_FALSE(a.contains(QStringLiteral("--no-root-motion")));
    EXPECT_FALSE(a.contains(QStringLiteral("--text-encoder-quant"))) << "empty = the sidecar's own default";
}

TEST(KimodoPredictor, InvalidRequestsFailBeforeStartingPython)
{
    KimodoPredictor::Options o;
    EXPECT_FALSE(KimodoPredictor::predict(o).ok) << "no segments";
    o.segments = {{QStringLiteral("walk"), 0.0}};
    EXPECT_TRUE(KimodoPredictor::predict(o).error.contains(QStringLiteral("duration")));
    o.segments = {{QStringLiteral("walk"), 2.0}};
    o.numSamples = 40;
    EXPECT_TRUE(KimodoPredictor::predict(o).error.contains(QStringLiteral("1..16")));
    o.numSamples = 1;
    o.textEncoderQuant = QStringLiteral("int3");
    EXPECT_TRUE(KimodoPredictor::predict(o).error.contains(QStringLiteral("int8"))) << "an unknown quantization is refused, not passed through";
}

TEST(KimodoPredictor, SegmentSpecSplitsOnTheLastAt)
{
    MotionSegment s;
    QString e;
    ASSERT_TRUE(parseSegment(QStringLiteral("look @ the sky@2.5"), &s, &e)) << e.toStdString();
    EXPECT_EQ(s.text, QStringLiteral("look @ the sky"));
    EXPECT_DOUBLE_EQ(s.duration, 2.5);
    EXPECT_FALSE(parseSegment(QStringLiteral("walk"), &s, &e));
    EXPECT_FALSE(parseSegment(QStringLiteral("walk@0"), &s, &e));
    EXPECT_FALSE(parseSegment(QStringLiteral("@3"), &s, &e));
}
