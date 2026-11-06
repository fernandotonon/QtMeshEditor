#include <gtest/gtest.h>

#include "BoneDisplay.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

using namespace BoneDisplay;

namespace {

Document rigWithFaceAndArm()
{
    Document doc;
    EXPECT_TRUE(addGroup(&doc, QStringLiteral("Face Controls"), Color{1.f, 1.f, 0.f, 0.7f}));
    EXPECT_TRUE(addGroup(&doc, QStringLiteral("Arm IK"), Color{0.2f, 0.6f, 1.f, 0.7f}));
    EXPECT_TRUE(setMembership(&doc, QStringLiteral("Jaw"), QStringLiteral("Face Controls"), true));
    EXPECT_TRUE(setMembership(&doc, QStringLiteral("Jaw"), QStringLiteral("Arm IK"), true));
    EXPECT_TRUE(setMembership(&doc, QStringLiteral("Hand.L"), QStringLiteral("Arm IK"), true));
    return doc;
}

} // namespace

TEST(BoneDisplayGroups, ABoneCanBelongToSeveralGroups)
{
    Document doc = rigWithFaceAndArm();
    const Bone jaw = boneOrDefault(doc, QStringLiteral("Jaw"));
    EXPECT_EQ(jaw.groups, QStringList({QStringLiteral("Face Controls"), QStringLiteral("Arm IK")}));
    EXPECT_TRUE(removeGroup(&doc, QStringLiteral("Arm IK")));
    EXPECT_EQ(boneOrDefault(doc, QStringLiteral("Jaw")).groups, QStringList({QStringLiteral("Face Controls")}));
    EXPECT_TRUE(boneOrDefault(doc, QStringLiteral("Hand.L")).groups.isEmpty());
    QString error;
    EXPECT_FALSE(addGroup(&doc, QStringLiteral("Face Controls"), Color{}, &error));
    EXPECT_FALSE(addGroup(&doc, QStringLiteral("  "), Color{}, &error));
}

TEST(BoneDisplayLayers, BitmasksHideBonesWhoseLayersAreOff)
{
    Document doc;
    EXPECT_TRUE(isVisible(doc, QStringLiteral("Hips")));
    EXPECT_TRUE(setBoneLayer(&doc, QStringLiteral("Hips"), 0, true));
    EXPECT_TRUE(setBoneLayer(&doc, QStringLiteral("Hips"), 4, true));
    EXPECT_TRUE(layerBit(boneOrDefault(doc, QStringLiteral("Hips")).layers, 0));
    EXPECT_TRUE(layerBit(boneOrDefault(doc, QStringLiteral("Hips")).layers, 4));
    EXPECT_FALSE(setBoneLayer(&doc, QStringLiteral("Hips"), 32, true));
    EXPECT_FALSE(setVisibleLayer(&doc, -1, false));

    EXPECT_TRUE(setVisibleLayer(&doc, 0, false));
    EXPECT_TRUE(isVisible(doc, QStringLiteral("Hips")));
    EXPECT_TRUE(setVisibleLayer(&doc, 4, false));
    EXPECT_FALSE(isVisible(doc, QStringLiteral("Hips")));

    EXPECT_TRUE(setVisibleLayer(&doc, 4, true));
    EXPECT_TRUE(setBoneLayer(&doc, QStringLiteral("Hips"), 0, false));
    EXPECT_TRUE(setBoneLayer(&doc, QStringLiteral("Hips"), 4, false));
    EXPECT_EQ(boneOrDefault(doc, QStringLiteral("Hips")).layers, 0u);
    EXPECT_FALSE(isVisible(doc, QStringLiteral("Hips")));
}

TEST(BoneDisplayIsolate, IsolateHidesBonesOutsideTheGroup)
{
    Document doc = rigWithFaceAndArm();
    EXPECT_FALSE(setIsolatedGroup(&doc, QStringLiteral("Missing")));
    EXPECT_TRUE(setIsolatedGroup(&doc, QStringLiteral("Face Controls")));
    EXPECT_TRUE(isVisible(doc, QStringLiteral("Jaw")));
    EXPECT_FALSE(isVisible(doc, QStringLiteral("Hand.L")));
    EXPECT_FALSE(isVisible(doc, QStringLiteral("Hips")));
    EXPECT_TRUE(setIsolatedGroup(&doc, QString()));
    EXPECT_TRUE(isVisible(doc, QStringLiteral("Hand.L")));
    EXPECT_TRUE(isVisible(doc, QStringLiteral("Hips")));
}

TEST(BoneDisplayLocks, SelectionAndChannelsAreIndependent)
{
    Document doc;
    EXPECT_TRUE(canSelect(doc, QStringLiteral("Hips")));
    EXPECT_FALSE(channelLocked(doc, QStringLiteral("Hips"), Channel::Translation));
    EXPECT_FALSE(channelLocked(doc, QStringLiteral("Hips"), Channel::Rotation));
    EXPECT_FALSE(channelLocked(doc, QStringLiteral("Hips"), Channel::Scale));

    EXPECT_TRUE(setSelectionLocked(&doc, QStringLiteral("Hips"), true));
    EXPECT_TRUE(isVisible(doc, QStringLiteral("Hips")));
    EXPECT_FALSE(canSelect(doc, QStringLiteral("Hips")));

    EXPECT_TRUE(setChannelLocked(&doc, QStringLiteral("Hips"), Channel::Rotation, true));
    EXPECT_FALSE(channelLocked(doc, QStringLiteral("Hips"), Channel::Translation));
    EXPECT_TRUE(channelLocked(doc, QStringLiteral("Hips"), Channel::Rotation));
    EXPECT_FALSE(channelLocked(doc, QStringLiteral("Hips"), Channel::Scale));

    EXPECT_TRUE(setVisible(&doc, QStringLiteral("Hips"), false));
    EXPECT_FALSE(isVisible(doc, QStringLiteral("Hips")));
    EXPECT_FALSE(canSelect(doc, QStringLiteral("Hips")));
}

TEST(BoneDisplayColor, GroupColourWinsWhenRequested)
{
    Document doc = rigWithFaceAndArm();
    bool has = true;
    effectiveColor(doc, QStringLiteral("Jaw"), &has);
    EXPECT_FALSE(has);

    EXPECT_TRUE(setUseGroupColor(&doc, QStringLiteral("Jaw"), true));
    const Color grouped = effectiveColor(doc, QStringLiteral("Jaw"), &has);
    EXPECT_TRUE(has);
    EXPECT_FLOAT_EQ(grouped.r, 1.f);
    EXPECT_FLOAT_EQ(grouped.g, 1.f);

    Color orange {1.f, 0.4f, 0.f, 0.5f};
    EXPECT_TRUE(setColor(&doc, QStringLiteral("Jaw"), orange));
    EXPECT_TRUE(setUseGroupColor(&doc, QStringLiteral("Jaw"), false));
    const Color own = effectiveColor(doc, QStringLiteral("Jaw"), &has);
    EXPECT_TRUE(has);
    EXPECT_FLOAT_EQ(own.g, 0.4f);
}

TEST(BoneDisplayJson, RoundTripsShapesLayersLocksAndCustomMesh)
{
    Document doc = rigWithFaceAndArm();
    EXPECT_TRUE(setShape(&doc, QStringLiteral("Hand.L"), Shape::CustomMesh));
    EXPECT_TRUE(setCustomMesh(&doc, QStringLiteral("Hand.L"), QStringLiteral("CircleCtrl")));
    EXPECT_TRUE(setScale(&doc, QStringLiteral("Hand.L"), Vec3{1.5f, 0.5f, 1.5f}));
    EXPECT_TRUE(setOffset(&doc, QStringLiteral("Hand.L"), Vec3{0.f, 0.2f, 0.f}));
    EXPECT_TRUE(setRotation(&doc, QStringLiteral("Hand.L"), Vec3{0.f, 90.f, 0.f}));
    EXPECT_TRUE(setSelectionLocked(&doc, QStringLiteral("Hips"), true));
    EXPECT_TRUE(setChannelLocked(&doc, QStringLiteral("Hips"), Channel::Translation, true));
    EXPECT_TRUE(setVisibleLayer(&doc, 3, false));
    EXPECT_TRUE(setIsolatedGroup(&doc, QStringLiteral("Arm IK")));

    QString error;
    Document back;
    ASSERT_TRUE(fromJson(toJson(doc), &back, &error)) << error.toStdString();
    EXPECT_EQ(back.groups.size(), 2);
    EXPECT_EQ(back.isolatedGroup, QStringLiteral("Arm IK"));
    EXPECT_FALSE(layerBit(back.visibleLayers, 3));
    const Bone hand = boneOrDefault(back, QStringLiteral("Hand.L"));
    EXPECT_EQ(hand.shape, Shape::CustomMesh);
    EXPECT_EQ(hand.customMesh, QStringLiteral("CircleCtrl"));
    EXPECT_FLOAT_EQ(hand.scale.x, 1.5f);
    EXPECT_FLOAT_EQ(hand.offset.y, 0.2f);
    EXPECT_FLOAT_EQ(hand.rotationDeg.y, 90.f);
    EXPECT_TRUE(boneOrDefault(back, QStringLiteral("Hips")).selectionLocked);
    EXPECT_TRUE(boneOrDefault(back, QStringLiteral("Hips")).lockTranslation);
    EXPECT_FALSE(boneOrDefault(back, QStringLiteral("Hips")).lockRotation);

    Document rejected;
    EXPECT_FALSE(fromJson(QJsonObject{{QStringLiteral("schema"), QStringLiteral("nope")}}, &rejected, &error));
    EXPECT_FALSE(setScale(&doc, QStringLiteral("Hand.L"), Vec3{0.f, 1.f, 1.f}));
}

TEST(BoneDisplaySceneJson, RoundTripsPerNodeDocuments)
{
    NamedDocument hero;
    hero.node = QStringLiteral("Hero");
    EXPECT_TRUE(addGroup(&hero.document, QStringLiteral("Spine"), Color{1.f, 0.8f, 0.2f, 0.7f}));
    EXPECT_TRUE(setMembership(&hero.document, QStringLiteral("Chest"), QStringLiteral("Spine"), true));
    NamedDocument prop;
    prop.node = QStringLiteral("Prop");
    QList<NamedDocument> scene {prop, hero};
    QString error;
    QList<NamedDocument> back;
    ASSERT_TRUE(sceneFromJson(sceneToJson(scene), &back, &error)) << error.toStdString();
    ASSERT_EQ(back.size(), 1);
    EXPECT_EQ(back.first().node, QStringLiteral("Hero"));
    EXPECT_EQ(boneOrDefault(back.first().document, QStringLiteral("Chest")).groups,
              QStringList({QStringLiteral("Spine")}));
}

TEST(BoneDisplayGltfExtras, MergesIntoTheSceneAndSurvivesAGlb)
{
    Document doc;
    EXPECT_TRUE(setShape(&doc, QStringLiteral("Head"), Shape::Sphere));
    QJsonObject gltf;
    gltf.insert(QStringLiteral("scenes"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("S")}}});
    gltf.insert(QStringLiteral("asset"), QJsonObject{{QStringLiteral("version"), QStringLiteral("2.0")}});
    mergeGltfExtras(&gltf, toJson(doc));
    QString error;
    Document fromExtras;
    ASSERT_TRUE(fromJson(gltfExtrasPayload(gltf), &fromExtras, &error)) << error.toStdString();
    EXPECT_EQ(boneOrDefault(fromExtras, QStringLiteral("Head")).shape, Shape::Sphere);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("rig.glb"));
    QFile seed(path);
    ASSERT_TRUE(seed.open(QIODevice::WriteOnly));
    const QByteArray json = QJsonDocument(gltf).toJson(QJsonDocument::Compact);
    QByteArray bin("BINDATA");
    auto u32 = [](QByteArray* out, quint32 value) {
        out->append(static_cast<char>(value & 0xffu));
        out->append(static_cast<char>((value >> 8) & 0xffu));
        out->append(static_cast<char>((value >> 16) & 0xffu));
        out->append(static_cast<char>((value >> 24) & 0xffu));
    };
    QByteArray jsonPad = json;
    while (jsonPad.size() % 4) jsonPad.append(' ');
    while (bin.size() % 4) bin.append('\0');
    QByteArray glb;
    const quint32 total = 12u + 8u + static_cast<quint32>(jsonPad.size()) + 8u + static_cast<quint32>(bin.size());
    u32(&glb, 0x46546C67u);
    u32(&glb, 2u);
    u32(&glb, total);
    u32(&glb, static_cast<quint32>(jsonPad.size()));
    u32(&glb, 0x4E4F534Au);
    glb.append(jsonPad);
    u32(&glb, static_cast<quint32>(bin.size()));
    u32(&glb, 0x004E4942u);
    glb.append(bin);
    ASSERT_EQ(seed.write(glb), glb.size());
    seed.close();

    Document updated = doc;
    EXPECT_TRUE(setShape(&updated, QStringLiteral("Head"), Shape::Ring));
    ASSERT_TRUE(writeGltfExtras(path, toJson(updated), &error)) << error.toStdString();
    QJsonObject payload;
    ASSERT_TRUE(readGltfExtras(path, &payload, &error)) << error.toStdString();
    Document readBack;
    ASSERT_TRUE(fromJson(payload, &readBack, &error)) << error.toStdString();
    EXPECT_EQ(boneOrDefault(readBack, QStringLiteral("Head")).shape, Shape::Ring);

    QFile reread(path);
    ASSERT_TRUE(reread.open(QIODevice::ReadOnly));
    const QByteArray stored = reread.readAll();
    EXPECT_TRUE(stored.contains("BINDATA"));
}
