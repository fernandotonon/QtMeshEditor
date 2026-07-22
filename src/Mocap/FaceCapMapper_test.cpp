#ifdef ENABLE_MOCAP

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QFile>

#include "Mocap/FaceCapCanonicalData.h"
#include "Mocap/FaceCapMapper.h"

namespace {

QString findMesh(const FaceCapMapper::Mapping& m, const QString& canonical)
{
    for (const auto& ch : m.channels) {
        if (QString::fromLatin1(FaceCap::kBlendshapeNames[ch.canonicalIndex])
            == canonical)
            return ch.meshTargetName;
    }
    return {};
}

}  // namespace

TEST(FaceCapMapper, ExactArkitNamesAllMatch)
{
    QStringList mesh;
    for (int i = 1; i < FaceCap::kBlendshapeCount; ++i)  // skip _neutral
        mesh << QString::fromLatin1(FaceCap::kBlendshapeNames[i]);
    const auto m = FaceCapMapper::build(mesh);
    EXPECT_EQ(m.channels.size(), FaceCap::kBlendshapeCount - 1);
    EXPECT_TRUE(m.unmatchedCanonical.isEmpty())
        << m.unmatchedCanonical.join(", ").toStdString();
    EXPECT_TRUE(m.unmatchedMesh.isEmpty());
}

TEST(FaceCapMapper, NormalizedMatchingHandlesSeparatorsAndSides)
{
    const QStringList mesh{
        QStringLiteral("Jaw_Open"),          // CC style
        QStringLiteral("mouth_smile_l"),     // snake + side letter
        QStringLiteral("MouthSmile.R"),      // dot side
        QStringLiteral("eye-blink-left"),    // kebab
        QStringLiteral("EyeBlink_R"),
    };
    const auto m = FaceCapMapper::build(mesh);
    EXPECT_EQ(findMesh(m, QStringLiteral("jawOpen")), QStringLiteral("Jaw_Open"));
    EXPECT_EQ(findMesh(m, QStringLiteral("mouthSmileLeft")),
              QStringLiteral("mouth_smile_l"));
    EXPECT_EQ(findMesh(m, QStringLiteral("mouthSmileRight")),
              QStringLiteral("MouthSmile.R"));
    EXPECT_EQ(findMesh(m, QStringLiteral("eyeBlinkLeft")),
              QStringLiteral("eye-blink-left"));
    EXPECT_EQ(findMesh(m, QStringLiteral("eyeBlinkRight")),
              QStringLiteral("EyeBlink_R"));
    EXPECT_TRUE(m.unmatchedMesh.isEmpty());
}

TEST(FaceCapMapper, AliasTableResolvesCommonConventions)
{
    const QStringList mesh{QStringLiteral("Blink_L"), QStringLiteral("MouthOpen")};
    const auto m = FaceCapMapper::build(mesh);
    EXPECT_EQ(findMesh(m, QStringLiteral("eyeBlinkLeft")), QStringLiteral("Blink_L"));
    EXPECT_EQ(findMesh(m, QStringLiteral("jawOpen")), QStringLiteral("MouthOpen"));
}

TEST(FaceCapMapper, UnmatchedAreReportedNeverDropped)
{
    const QStringList mesh{QStringLiteral("jawOpen"), QStringLiteral("SomeCustomTarget")};
    const auto m = FaceCapMapper::build(mesh);
    EXPECT_EQ(m.channels.size(), 1);
    // 50 canonical channels unmatched (52 - _neutral - jawOpen)
    EXPECT_EQ(m.unmatchedCanonical.size(), FaceCap::kBlendshapeCount - 2);
    ASSERT_EQ(m.unmatchedMesh.size(), 1);
    EXPECT_EQ(m.unmatchedMesh.first(), QStringLiteral("SomeCustomTarget"));
    // _neutral is by-design absent from both lists
    EXPECT_FALSE(m.unmatchedCanonical.contains(QStringLiteral("_neutral")));
}

TEST(FaceCapMapper, OverrideMapAndIgnore)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString overridePath = tmp.path() + QStringLiteral("/map.json");
    {
        QJsonObject map;
        map.insert(QStringLiteral("jawOpen"), QStringLiteral("WeirdJaw"));
        QJsonObject root;
        root.insert(QStringLiteral("map"), map);
        root.insert(QStringLiteral("ignore"),
                    QJsonArray{QStringLiteral("cheekPuff")});
        QFile f(overridePath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(root).toJson());
    }
    const QStringList mesh{QStringLiteral("WeirdJaw"), QStringLiteral("cheekPuff")};
    const auto m = FaceCapMapper::build(mesh, overridePath);
    EXPECT_EQ(findMesh(m, QStringLiteral("jawOpen")), QStringLiteral("WeirdJaw"));
    // ignored channel is not mapped even though the mesh name would match
    EXPECT_TRUE(findMesh(m, QStringLiteral("cheekPuff")).isEmpty());
    EXPECT_TRUE(m.ignored.contains(QStringLiteral("cheekPuff")));
    EXPECT_FALSE(m.unmatchedCanonical.contains(QStringLiteral("cheekPuff")));
    EXPECT_TRUE(m.error.isEmpty()) << m.error.toStdString();
}

TEST(FaceCapMapper, BadOverridePathReportsErrorButStillMaps)
{
    const QStringList mesh{QStringLiteral("jawOpen")};
    const auto m =
        FaceCapMapper::build(mesh, QStringLiteral("/nonexistent/map.json"));
    EXPECT_FALSE(m.error.isEmpty());
    EXPECT_EQ(m.channels.size(), 1);
}

TEST(FaceCapMapper, EmptyMeshTargetList)
{
    const auto m = FaceCapMapper::build({});
    EXPECT_TRUE(m.channels.isEmpty());
    EXPECT_EQ(m.unmatchedCanonical.size(), FaceCap::kBlendshapeCount - 1);
}

#endif  // ENABLE_MOCAP
