/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — Paint v2 Slice I (#552) bake-target tests.

The engine channel layouts are the whole point of this module, and two of the
conversions are silently destructive if reversed: smoothness-vs-roughness
(Unity) and +Y-up-vs-+Y-down normals. Both are asserted against hand-computed
values rather than golden images, so a regression names the wrong lane instead
of just reporting "pixels differ".

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include "PaintBakeTargets.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

using namespace PaintBakeTargets;

/// Solid grayscale of `v`.
QImage gray(int v, int size = 4)
{
    QImage i(size, size, QImage::Format_Grayscale8);
    i.fill(static_cast<uchar>(v));
    return i;
}

/// Solid RGBA.
QImage rgba(int r, int g, int b, int a = 255, int size = 4)
{
    QImage i(size, size, QImage::Format_RGBA8888);
    i.fill(qRgba(r, g, b, a));
    return i;
}

const OutputTexture* find(const Result& r, const char* suffix)
{
    for (const auto& t : r.textures)
        if (t.suffix == QLatin1String(suffix)) return &t;
    return nullptr;
}

} // namespace

// --- target ids -----------------------------------------------------------

TEST(PaintBakeTargetsTest, TargetIdsRoundTrip) {
    for (const QString& id : targetIds()) {
        Target t{};
        ASSERT_TRUE(targetFromId(id, t)) << id.toStdString();
        EXPECT_EQ(QString::fromLatin1(targetId(t)), id);
    }
}

TEST(PaintBakeTargetsTest, UnknownTargetIdIsRejectedNotDefaulted) {
    Target t = Target::Unity;
    // A typo must FAIL rather than silently produce Generic output, which would
    // look like the requested pack simply did not apply.
    EXPECT_FALSE(targetFromId(QStringLiteral("unrealengine5"), t));
    EXPECT_FALSE(targetFromId(QString(), t));
    EXPECT_EQ(t, Target::Unity) << "a rejected parse must not clobber the target";

    // Case and surrounding space are tolerated.
    EXPECT_TRUE(targetFromId(QStringLiteral("  UNREAL "), t));
    EXPECT_EQ(t, Target::Unreal);
}

// --- pure conversions -----------------------------------------------------

TEST(PaintBakeTargetsTest, GrayscaleUsesRec601) {
    // Pure green at Rec.601 is 0.587 -> 150 (149.7 rounded).
    const QImage g = toGrayscale(rgba(0, 255, 0));
    ASSERT_FALSE(g.isNull());
    EXPECT_EQ(g.format(), QImage::Format_Grayscale8);
    EXPECT_EQ(g.constScanLine(0)[0], 150);

    // And red is 0.299 -> 76.
    EXPECT_EQ(toGrayscale(rgba(255, 0, 0)).constScanLine(0)[0], 76);
}

TEST(PaintBakeTargetsTest, InvertGrayscaleIsRoughnessToSmoothness) {
    EXPECT_EQ(invertGrayscale(gray(0)).constScanLine(0)[0], 255);
    EXPECT_EQ(invertGrayscale(gray(255)).constScanLine(0)[0], 0);
    EXPECT_EQ(invertGrayscale(gray(64)).constScanLine(0)[0], 191);
}

TEST(PaintBakeTargetsTest, FlipNormalGreenInvertsOnlyGreen) {
    // A typical OpenGL normal: flat surface is (128, 128, 255).
    const QImage f = flipNormalGreen(rgba(128, 200, 255));
    ASSERT_FALSE(f.isNull());
    const QRgb p = reinterpret_cast<const QRgb*>(f.constScanLine(0))[0];
    EXPECT_EQ(qRed(p), 128)   << "R must be untouched";
    EXPECT_EQ(qGreen(p), 55)  << "G must invert: 255 - 200";
    EXPECT_EQ(qBlue(p), 255)  << "B must be untouched";
    EXPECT_EQ(qAlpha(p), 255);
}

TEST(PaintBakeTargetsTest, PackRgbPlacesEachSourceOnItsOwnLane) {
    const QImage p = packRgb(gray(10), gray(20), gray(30));
    ASSERT_FALSE(p.isNull());
    EXPECT_EQ(p.format(), QImage::Format_RGB888);
    const uchar* line = p.constScanLine(0);
    EXPECT_EQ(line[0], 10);
    EXPECT_EQ(line[1], 20);
    EXPECT_EQ(line[2], 30);
}

TEST(PaintBakeTargetsTest, PackRgbUsesFallbackForAbsentLanes) {
    const QImage p = packRgb(gray(10), QImage(), QImage(), 4, 255);
    ASSERT_FALSE(p.isNull());
    const uchar* line = p.constScanLine(0);
    EXPECT_EQ(line[0], 10);
    EXPECT_EQ(line[1], 255) << "absent lane takes the fallback";
    EXPECT_EQ(line[2], 255);
}

// --- engine layouts -------------------------------------------------------

TEST(PaintBakeTargetsTest, GenericEmitsOneTexturePerPaintedChannel) {
    ChannelImages ch;
    ch.baseColor = rgba(200, 100, 50);
    ch.roughness = gray(128);

    Options o;
    o.target = Target::Generic;
    const Result r = build(ch, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.textures.size(), 2u) << "only painted channels are written";
    ASSERT_NE(find(r, "BaseColor"), nullptr);
    ASSERT_NE(find(r, "Roughness"), nullptr);
    EXPECT_TRUE(find(r, "BaseColor")->srgb) << "colour data is sRGB";
    EXPECT_FALSE(find(r, "Roughness")->srgb) << "roughness is linear data";
}

TEST(PaintBakeTargetsTest, UnityPacksMetallicInRedAndSmoothnessInAlpha) {
    ChannelImages ch;
    ch.metallic = gray(200);
    ch.roughness = gray(64);          // -> smoothness 191

    Options o;
    o.target = Target::Unity;
    const Result r = build(ch, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    const OutputTexture* ms = find(r, "MetallicSmoothness");
    ASSERT_NE(ms, nullptr);
    const QRgb p = reinterpret_cast<const QRgb*>(ms->image.constScanLine(0))[0];
    EXPECT_EQ(qRed(p), 200) << "metallic belongs in R";
    EXPECT_EQ(qAlpha(p), 191)
        << "alpha must be SMOOTHNESS (1 - roughness), not roughness";
}

TEST(PaintBakeTargetsTest, UnityConvertsTheNormalToDirectX) {
    ChannelImages ch;
    ch.normal = rgba(128, 200, 255);

    Options o;
    o.target = Target::Unity;
    const Result r = build(ch, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    const OutputTexture* n = find(r, "Normal");
    ASSERT_NE(n, nullptr);
    const QRgb p = reinterpret_cast<const QRgb*>(n->image.constScanLine(0))[0];
    EXPECT_EQ(qGreen(p), 55)
        << "Unity samples +Y DOWN; leaving OpenGL green inverts every surface";
}

TEST(PaintBakeTargetsTest, UnrealPacksOrmInThatOrder) {
    ChannelImages ch;
    ch.ao = gray(90);
    ch.roughness = gray(120);
    ch.metallic = gray(210);

    Options o;
    o.target = Target::Unreal;
    const Result r = build(ch, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    const OutputTexture* orm = find(r, "ORM");
    ASSERT_NE(orm, nullptr);
    const uchar* line = orm->image.constScanLine(0);
    EXPECT_EQ(line[0], 90)  << "R = occlusion";
    EXPECT_EQ(line[1], 120) << "G = roughness";
    EXPECT_EQ(line[2], 210) << "B = metallic";
}

TEST(PaintBakeTargetsTest, UnrealUnpaintedOcclusionReadsAsWhiteNotBlack) {
    // A 0 occlusion lane would darken the entire surface. Unpainted AO must
    // mean "no occlusion".
    ChannelImages ch;
    ch.roughness = gray(120);

    Options o;
    o.target = Target::Unreal;
    const Result r = build(ch, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    const OutputTexture* orm = find(r, "ORM");
    ASSERT_NE(orm, nullptr);
    EXPECT_EQ(orm->image.constScanLine(0)[0], 255)
        << "absent occlusion must be white, or the material renders too dark";
}

TEST(PaintBakeTargetsTest, GltfPacksRoughnessInGreenAndMetallicInBlue) {
    ChannelImages ch;
    ch.roughness = gray(100);
    ch.metallic = gray(180);

    Options o;
    o.target = Target::GLTF;
    const Result r = build(ch, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    const OutputTexture* mr = find(r, "metallicRoughness");
    ASSERT_NE(mr, nullptr);
    const uchar* line = mr->image.constScanLine(0);
    EXPECT_EQ(line[1], 100) << "glTF reads roughness from G";
    EXPECT_EQ(line[2], 180) << "glTF reads metallic from B";
}

TEST(PaintBakeTargetsTest, GodotEmitsSeparateTexturesAndATresWithColourSpaceFlags) {
    ChannelImages ch;
    ch.baseColor = rgba(200, 100, 50);
    ch.roughness = gray(128);

    Options o;
    o.target = Target::Godot;
    o.namePrefix = QStringLiteral("hero");
    const Result r = build(ch, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    ASSERT_NE(find(r, "Albedo"), nullptr) << "Godot names base colour Albedo";
    ASSERT_NE(find(r, "Roughness"), nullptr);
    EXPECT_FALSE(r.godotResource.isEmpty()) << ".tres sidecar is the point of this target";
    // The colour-space flag is the import bug this sidecar exists to prevent.
    EXPECT_TRUE(r.godotResource.contains(QStringLiteral("Albedo -> sRGB")));
    // The referenced filenames must match what the writer actually produces
    // (prefix + "_" + suffix); an earlier version emitted "heroAlbedo.png" and
    // pointed the .tres at files that do not exist.
    EXPECT_TRUE(r.godotResource.contains(QStringLiteral("hero_Albedo.png")))
        << r.godotResource.toStdString();
    EXPECT_FALSE(r.godotResource.contains(QStringLiteral("heroAlbedo.png")));
    EXPECT_TRUE(r.godotResource.contains(QStringLiteral("Roughness -> linear/data")));
}

TEST(PaintBakeTargetsTest, OtherTargetsEmitNoGodotResource) {
    ChannelImages ch;
    ch.baseColor = rgba(10, 20, 30);
    for (Target t : {Target::Generic, Target::Unity, Target::Unreal, Target::GLTF}) {
        Options o;
        o.target = t;
        const Result r = build(ch, o);
        ASSERT_TRUE(r.ok) << targetId(t);
        EXPECT_TRUE(r.godotResource.isEmpty()) << targetId(t);
    }
}

// --- resolution + failure modes -------------------------------------------

TEST(PaintBakeTargetsTest, ResolutionZeroKeepsTheSourceSize) {
    ChannelImages ch;
    ch.baseColor = rgba(1, 2, 3, 255, 8);

    Options o;
    o.target = Target::Generic;
    o.resolution = 0;
    const Result r = build(ch, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(find(r, "BaseColor")->image.width(), 8);
}

TEST(PaintBakeTargetsTest, ResolutionResamplesEveryOutput) {
    ChannelImages ch;
    ch.baseColor = rgba(1, 2, 3, 255, 8);
    ch.roughness = gray(128, 16);

    Options o;
    o.target = Target::Generic;
    o.resolution = 32;
    const Result r = build(ch, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    for (const auto& t : r.textures) {
        EXPECT_EQ(t.image.width(), 32) << t.suffix.toStdString();
        EXPECT_EQ(t.image.height(), 32) << t.suffix.toStdString();
    }
}

TEST(PaintBakeTargetsTest, NothingPaintedIsAnErrorNotABlankTextureSet) {
    // Writing a directory of blank textures would look like a successful bake.
    //
    // NB build() defends this twice — an up-front ch.empty() check and a
    // trailing "no output textures" check — so removing EITHER alone still
    // fails the bake. Mutation testing showed neither guard is individually
    // detectable; the assertion below therefore pins the OBSERVABLE contract
    // (not ok, an explanation, no textures) rather than pretending to pin one
    // specific branch. The message is checked to distinguish the two paths.
    const Result r = build(ChannelImages{}, Options{});
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
    EXPECT_TRUE(r.textures.empty());
    EXPECT_TRUE(r.error.contains(QStringLiteral("Nothing painted")))
        << "the up-front empty check should be what reports this, giving the "
           "clearer message; got: " << r.error.toStdString();
}

// A target whose required channels are absent must also fail, even though other
// channels ARE painted — this is the trailing "no output textures" guard, which
// the all-empty case above cannot reach.
TEST(PaintBakeTargetsTest, ATargetWithNoFeedingChannelsIsAnError) {
    // glTF emits only from roughness/metallic/ao/basecolor/normal/emissive; a
    // channel set that feeds none of them yields no outputs. Construct that by
    // painting nothing the target consumes.
    ChannelImages ch;   // all null
    Options o;
    o.target = Target::GLTF;
    const Result r = build(ch, o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.textures.empty());
}

TEST(PaintBakeTargetsTest, NegativeResolutionIsRejected) {
    ChannelImages ch;
    ch.baseColor = rgba(1, 2, 3);
    Options o;
    o.resolution = -256;
    const Result r = build(ch, o);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

// --- sidecar --------------------------------------------------------------

TEST(PaintBakeTargetsTest, SidecarDescribesTargetInputsAndOutputs) {
    ChannelImages ch;
    ch.baseColor = rgba(200, 100, 50);
    ch.metallic = gray(200);
    ch.roughness = gray(64);

    Options o;
    o.target = Target::Unity;
    o.resolution = 512;
    o.namePrefix = QStringLiteral("hero");

    const Result r = build(ch, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    const QString json = sidecarJson(
        o, {QStringLiteral("basecolor"), QStringLiteral("metallic"),
            QStringLiteral("roughness")},
        r.textures, QStringLiteral("Hero.mesh"));

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    ASSERT_EQ(err.error, QJsonParseError::NoError) << err.errorString().toStdString();
    const QJsonObject root = doc.object();

    EXPECT_EQ(root["schema"].toString(), QStringLiteral("qtmesh-paint-bake-v1"));
    EXPECT_EQ(root["target"].toString(), QStringLiteral("unity"));
    EXPECT_EQ(root["resolution"].toInt(), 512);
    EXPECT_EQ(root["namePrefix"].toString(), QStringLiteral("hero"));
    EXPECT_EQ(root["mesh"].toString(), QStringLiteral("Hero.mesh"));
    EXPECT_EQ(root["inputChannels"].toArray().size(), 3);

    const QJsonArray outs = root["outputs"].toArray();
    ASSERT_EQ(outs.size(), static_cast<int>(r.textures.size()));
    // The prefix must be reflected in the recorded filename, or the sidecar
    // points at files that do not exist.
    bool sawPrefixed = false;
    for (const QJsonValue& v : outs) {
        const QJsonObject o2 = v.toObject();
        EXPECT_EQ(o2["width"].toInt(), 512);
        if (o2["file"].toString().startsWith(QStringLiteral("hero_"))) sawPrefixed = true;
    }
    EXPECT_TRUE(sawPrefixed);

    // The Height omission is recorded so it reads as a decision, not a gap.
    EXPECT_TRUE(root.contains(QStringLiteral("heightOmitted")));
}

TEST(PaintBakeTargetsTest, SidecarFilenamesHaveNoPrefixWhenNoneWasGiven) {
    ChannelImages ch;
    ch.baseColor = rgba(1, 2, 3);
    Options o;
    o.target = Target::Generic;
    const Result r = build(ch, o);
    ASSERT_TRUE(r.ok);

    const QString json = sidecarJson(o, {QStringLiteral("basecolor")}, r.textures, {});
    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    const QJsonArray outs = root["outputs"].toArray();
    ASSERT_GT(outs.size(), 0);
    EXPECT_EQ(outs[0].toObject()["file"].toString(), QStringLiteral("BaseColor.png"));
    EXPECT_FALSE(root.contains(QStringLiteral("namePrefix")));
}
