/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "BoneTextureBaker.h"
#include "MinimalEXRWriter.h"
#include "TestHelpers.h"

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreFrameListener.h>
#include <OgreHardwareBufferManager.h>
#include <OgreKeyFrame.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgreRoot.h>
#include <OgreSkeletonInstance.h>
#include <OgreSkeletonManager.h>
#include <OgreSubMesh.h>

#include <cmath>
#include <cstring>
#include <atomic>
#include <limits>
#include <random>
#include <thread>

using namespace BoneTexture;

namespace {

Mat3x4 translation(float x, float y, float z)
{
    Mat3x4 m;
    m.at(0, 3) = x; m.at(1, 3) = y; m.at(2, 3) = z;
    return m;
}

Mat3x4 rotationZ(float radians, float tx = 0, float ty = 0, float tz = 0)
{
    Mat3x4 m;
    const float c = std::cos(radians), s = std::sin(radians);
    m.at(0, 0) = c; m.at(0, 1) = -s;
    m.at(1, 0) = s; m.at(1, 1) = c;
    m.at(0, 3) = tx; m.at(1, 3) = ty; m.at(2, 3) = tz;
    return m;
}

// Two clips over `bones` bones; every matrix carries a value that names
// its (clip, frame, bone) so a mis-addressed texel is obvious.
BakedClips taggedBake(int bones, int framesA, int framesB)
{
    BakedClips b;
    for (int i = 0; i < bones; ++i) b.bones.push_back({ QStringLiteral("bone%1").arg(i), i - 1 });
    Clip a; a.name = QStringLiteral("A"); a.frameCount = framesA; a.duration = 1.0; a.fps = framesA - 1;
    Clip c; c.name = QStringLiteral("B"); c.frameCount = framesB; c.duration = 2.0; c.fps = (framesB - 1) / 2.0;
    b.clips = { a, c };
    layoutClips(b.clips);
    const int counts[2] = { framesA, framesB };
    for (int ci = 0; ci < 2; ++ci) {
        std::vector<std::vector<Mat3x4>> frames;
        for (int f = 0; f < counts[ci]; ++f) {
            std::vector<Mat3x4> row;
            for (int bi = 0; bi < bones; ++bi) {
                Mat3x4 m;
                for (int k = 0; k < 12; ++k)
                    m.m[static_cast<size_t>(k)] = ci * 1000.0f + f * 100.0f + bi * 10.0f + k * 0.5f;
                row.push_back(m);
            }
            frames.push_back(row);
        }
        b.frames.push_back(frames);
    }
    return b;
}

void expectMatNear(const Mat3x4& a, const Mat3x4& b, float tol)
{
    for (size_t i = 0; i < 12; ++i) EXPECT_NEAR(a.m[i], b.m[i], tol) << "element " << i;
}

// Reads back the scanlines of an uncompressed RGBA EXR written by
// MinimalEXR (channel planes A,B,G,R per scanline, payload at the end).
std::vector<float> readOwnExr(const QString& path, int width, int height, bool half)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = f.readAll();
    const int bps = half ? 2 : 4;
    const qsizetype rowSize = 8 + static_cast<qsizetype>(width) * 4 * bps;
    const qsizetype start = bytes.size() - rowSize * height;
    if (start <= 0) return {};
    std::vector<float> out(static_cast<size_t>(width) * height * 4);
    auto sampleAt = [&](qsizetype off) {
        if (half) {
            uint16_t h;
            std::memcpy(&h, bytes.constData() + off, 2);
            return MinimalEXR::halfToFloat(h);
        }
        float v;
        std::memcpy(&v, bytes.constData() + off, 4);
        return v;
    };
    const int planeToChannel[4] = { 3, 2, 1, 0 };  // A,B,G,R
    for (int y = 0; y < height; ++y) {
        const qsizetype rowOff = start + rowSize * y + 8;
        for (int p = 0; p < 4; ++p)
            for (int x = 0; x < width; ++x)
                out[(static_cast<size_t>(y) * width + x) * 4 + planeToChannel[p]] =
                    sampleAt(rowOff + (static_cast<qsizetype>(p) * width + x) * bps);
    }
    return out;
}

} // namespace

// ===========================================================================
// Pure data
// ===========================================================================

TEST(BoneTextureLayout, FrameCountSamplesBothEnds)
{
    EXPECT_EQ(frameCountFor(1.0, 30.0), 31);
    EXPECT_EQ(frameCountFor(0.5, 30.0), 16);
    EXPECT_EQ(frameCountFor(0.0, 30.0), 1) << "a static clip is one frame";
    EXPECT_EQ(frameCountFor(0.01, 30.0), 2) << "a positive duration keeps both ends";
    EXPECT_EQ(frameCountFor(1.0, 0.0), 1);
}

TEST(BoneTextureLayout, ClipsStackVertically)
{
    std::vector<Clip> clips(3);
    clips[0].frameCount = 31;
    clips[1].frameCount = 2;
    clips[2].frameCount = 10;
    EXPECT_EQ(layoutClips(clips), 43);
    EXPECT_EQ(clips[0].firstRow, 0);
    EXPECT_EQ(clips[1].firstRow, 31);
    EXPECT_EQ(clips[2].firstRow, 33);
}

TEST(BoneTexturePack, RoundTripsEveryMatrixInItsTexels)
{
    const BakedClips b = taggedBake(3, 4, 3);
    int w = 0, h = 0;
    const std::vector<float> tex = pack(b, &w, &h);
    ASSERT_FALSE(tex.empty());
    EXPECT_EQ(w, 9);
    EXPECT_EQ(h, 7);
    for (size_t ci = 0; ci < 2; ++ci)
        for (int f = 0; f < b.clips[ci].frameCount; ++f)
            for (int bi = 0; bi < 3; ++bi)
                expectMatNear(unpack(tex, w, bi, b.clips[ci].firstRow + f),
                              b.frames[ci][static_cast<size_t>(f)][static_cast<size_t>(bi)], 0.0f);
}

TEST(BoneTexturePack, TexelHoldsOneMatrixRow)
{
    // Bone 1, clip B frame 2 → row 4 + 2, x = 1*3 + r. Texel r = matrix row r.
    const BakedClips b = taggedBake(3, 4, 3);
    int w = 0, h = 0;
    const std::vector<float> tex = pack(b, &w, &h);
    const Mat3x4& m = b.frames[1][2][1];
    for (int r = 0; r < 3; ++r) {
        const size_t idx = (static_cast<size_t>(6) * w + 1 * 3 + r) * 4;
        for (int c = 0; c < 4; ++c) EXPECT_FLOAT_EQ(tex[idx + c], m.at(r, c)) << r << "," << c;
    }
}

TEST(BoneTexturePack, BoneOrderIsTheBonesArrayOrder)
{
    BakedClips b = taggedBake(4, 2, 2);
    int w = 0, h = 0;
    const std::vector<float> tex = pack(b, &w, &h);
    // Bone k's first element encodes 10*k: columns follow the bones array.
    for (int k = 0; k < 4; ++k) EXPECT_FLOAT_EQ(unpack(tex, w, k, 0).m[0], 10.0f * k);
}

TEST(BoneTextureSample, InterpolatesWithinAClipAndNeverBleedsIntoTheNext)
{
    BakedClips b;
    b.bones = { { QStringLiteral("root"), -1 } };
    Clip a; a.name = QStringLiteral("A"); a.frameCount = 3; a.duration = 1.0; a.fps = 2.0;
    Clip c; c.name = QStringLiteral("B"); c.frameCount = 2; c.duration = 1.0; c.fps = 1.0;
    b.clips = { a, c };
    layoutClips(b.clips);
    b.frames = { { { translation(0, 0, 0) }, { translation(1, 0, 0) }, { translation(2, 0, 0) } },
                 { { translation(100, 0, 0) }, { translation(200, 0, 0) } } };
    int w = 0, h = 0;
    const auto tex = pack(b, &w, &h);
    const Clip& A = b.clips[0];

    EXPECT_FLOAT_EQ(sample(tex, w, A, 0, 0.25, false).at(0, 3), 0.5f);
    EXPECT_FLOAT_EQ(sample(tex, w, A, 0, 0.75, false).at(0, 3), 1.5f);
    // The clip END is its own last row, not the next clip's first.
    EXPECT_FLOAT_EQ(sample(tex, w, A, 0, 1.0, false).at(0, 3), 2.0f);
    EXPECT_FLOAT_EQ(sample(tex, w, A, 0, 5.0, false).at(0, 3), 2.0f) << "clamped";
    EXPECT_NEAR(sample(tex, w, A, 0, 0.999, false).at(0, 3), 1.998f, 1e-4f);
    // Looping wraps to the clip start.
    EXPECT_FLOAT_EQ(sample(tex, w, A, 0, 1.25, true).at(0, 3), 0.5f);
    EXPECT_FLOAT_EQ(sample(tex, w, A, 0, -0.25, true).at(0, 3), 1.5f);
    // Clip B reads its own rows.
    EXPECT_FLOAT_EQ(sample(tex, w, b.clips[1], 0, 0.5, false).at(0, 3), 150.0f);
}

TEST(BoneTextureSample, SingleFrameClipIsStatic)
{
    BakedClips b;
    b.bones = { { QStringLiteral("root"), -1 } };
    Clip a; a.name = QStringLiteral("Pose"); a.frameCount = 1;
    b.clips = { a };
    layoutClips(b.clips);
    b.frames = { { { translation(3, 0, 0) } } };
    int w = 0, h = 0;
    const auto tex = pack(b, &w, &h);
    EXPECT_FLOAT_EQ(sample(tex, w, b.clips[0], 0, 0.7, true).at(0, 3), 3.0f);
}

TEST(BoneTextureSample, CrossfadeBlendsTwoClips)
{
    BakedClips b;
    b.bones = { { QStringLiteral("root"), -1 } };
    Clip a; a.name = QStringLiteral("A"); a.frameCount = 2; a.duration = 1.0; a.fps = 1.0;
    Clip c = a; c.name = QStringLiteral("B");
    b.clips = { a, c };
    layoutClips(b.clips);
    b.frames = { { { translation(0, 0, 0) }, { translation(0, 0, 0) } },
                 { { translation(0, 4, 0) }, { translation(0, 4, 0) } } };
    int w = 0, h = 0;
    const auto tex = pack(b, &w, &h);
    EXPECT_FLOAT_EQ(crossfade(tex, w, b.clips[0], 0.3, b.clips[1], 0.6, 0, 0.25f, true).at(1, 3), 1.0f);
    EXPECT_FLOAT_EQ(crossfade(tex, w, b.clips[0], 0.3, b.clips[1], 0.6, 0, 2.0f, true).at(1, 3), 4.0f)
        << "weight clamped to [0,1]";
}

TEST(BoneTextureSkin, BlendsWeightedMatricesAndRenormalises)
{
    const std::vector<Mat3x4> mats = { translation(1, 0, 0), translation(0, 2, 0) };
    const float p[3] = { 0, 0, 5 };
    const int idx[4] = { 0, 1, -1, 7 };   // invalid indices are ignored
    const float wts[4] = { 0.25f, 0.75f, 0.5f, 0.5f };
    float out[3];
    skinVertex(mats, p, nullptr, idx, wts, out, nullptr);
    EXPECT_FLOAT_EQ(out[0], 0.25f);
    EXPECT_FLOAT_EQ(out[1], 1.5f);
    EXPECT_FLOAT_EQ(out[2], 5.0f);

    // Weights that do not sum to 1 are renormalised.
    const float half[4] = { 0.1f, 0.3f, 0, 0 };
    skinVertex(mats, p, nullptr, idx, half, out, nullptr);
    EXPECT_FLOAT_EQ(out[0], 0.25f);
    EXPECT_FLOAT_EQ(out[1], 1.5f);
}

TEST(BoneTextureSkin, RotatesNormalsAndKeepsThemUnit)
{
    const std::vector<Mat3x4> mats = { rotationZ(static_cast<float>(M_PI) / 2, 5, 0, 0) };
    const float p[3] = { 1, 0, 0 }, n[3] = { 1, 0, 0 };
    const int idx[4] = { 0, 0, 0, 0 };
    const float wts[4] = { 1, 0, 0, 0 };
    float op[3], on[3];
    skinVertex(mats, p, n, idx, wts, op, on);
    EXPECT_NEAR(op[0], 5.0f, 1e-6f);
    EXPECT_NEAR(op[1], 1.0f, 1e-6f);
    EXPECT_NEAR(on[0], 0.0f, 1e-6f);
    EXPECT_NEAR(on[1], 1.0f, 1e-6f) << "normals ignore translation";
}

TEST(BoneTextureSkin, ZeroWeightsLeaveTheVertexInBindPose)
{
    const std::vector<Mat3x4> mats = { translation(9, 9, 9) };
    const float p[3] = { 1, 2, 3 };
    const int idx[4] = { 0, 0, 0, 0 };
    const float wts[4] = { 0, 0, 0, 0 };
    float out[3];
    skinVertex(mats, p, nullptr, idx, wts, out, nullptr);
    EXPECT_FLOAT_EQ(out[0], 1.0f);
    EXPECT_FLOAT_EQ(out[2], 3.0f);
}

TEST(BoneTextureValidate, ReportsActionableErrors)
{
    BakedClips b = taggedBake(3, 4, 3);
    EXPECT_TRUE(validate(b).isEmpty());
    EXPECT_TRUE(validate(b, 8).contains(QStringLiteral("bones need a 9-texel-wide")));
    // 3 bones (9 texels wide) fit a 12 limit; 13 + 7 rows do not.
    const BakedClips tall = taggedBake(3, 13, 7);
    EXPECT_TRUE(validate(tall, 12).contains(QStringLiteral("20 frames in total")))
        << validate(tall, 12).toStdString();

    BakedClips nan = b;
    nan.frames[1][1][2].m[5] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_TRUE(validate(nan).contains(QStringLiteral("bone 'bone2' has a non-finite")));

    BakedClips shortFrames = b;
    shortFrames.frames[0].pop_back();
    EXPECT_FALSE(validate(shortFrames).isEmpty());
    EXPECT_TRUE(pack(shortFrames, nullptr, nullptr).empty());

    BakedClips unlaid = b;
    unlaid.clips[1].firstRow = 0;
    EXPECT_TRUE(validate(unlaid).contains(QStringLiteral("layoutClips")));

    EXPECT_FALSE(validate(BakedClips{}).isEmpty());
}

TEST(BoneTextureHalf, ConvertsExactAndSpecialValues)
{
    EXPECT_EQ(MinimalEXR::floatToHalf(1.0f), 0x3c00);
    EXPECT_EQ(MinimalEXR::floatToHalf(-2.0f), 0xc000);
    EXPECT_EQ(MinimalEXR::floatToHalf(0.0f), 0x0000);
    EXPECT_EQ(MinimalEXR::floatToHalf(65504.0f), 0x7bff);
    EXPECT_EQ(MinimalEXR::floatToHalf(1e6f), 0x7c00);
    EXPECT_EQ(MinimalEXR::floatToHalf(std::ldexp(1.0f, -24)), 0x0001) << "smallest subnormal";
    EXPECT_EQ(MinimalEXR::floatToHalf(1.0f + std::ldexp(1.0f, -11)), 0x3c00) << "tie rounds to even";
    EXPECT_FLOAT_EQ(MinimalEXR::halfToFloat(0x3555), 0.333251953125f);
    EXPECT_FLOAT_EQ(MinimalEXR::halfToFloat(0x0001), std::ldexp(1.0f, -24));
    EXPECT_TRUE(std::isinf(MinimalEXR::halfToFloat(0x7c00)));
    for (uint32_t h = 0; h < 0x7c00; ++h)   // every finite positive half survives
        ASSERT_EQ(MinimalEXR::floatToHalf(MinimalEXR::halfToFloat(static_cast<uint16_t>(h))), h);
}

TEST(BoneTextureHalf, RotationMatricesStayWithinHalfPrecision)
{
    // Rotation entries are in [-1,1]: the half step there is <= 2^-11.
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> ang(-3.14159f, 3.14159f);
    for (int i = 0; i < 1000; ++i) {
        const Mat3x4 m = rotationZ(ang(rng));
        for (float v : m.m)
            EXPECT_NEAR(MinimalEXR::halfToFloat(MinimalEXR::floatToHalf(v)), v, 4.9e-4f);
    }
}

TEST(BoneTextureSidecar, DescribesLayoutBonesAndClips)
{
    BakedClips b = taggedBake(2, 3, 2);
    b.sourceMaxInfluences = 6;
    b.verticesOverLimit = 12;
    const QJsonObject root = QJsonDocument::fromJson(
        buildSidecarJson(b, QStringLiteral("x_bones.exr"), QStringLiteral("rgba16f"), 6, 5).toUtf8()).object();
    EXPECT_EQ(root[QStringLiteral("schema")].toString(), QStringLiteral("qtmesh-bone-texture-v1"));
    const QJsonObject tex = root[QStringLiteral("texture")].toObject();
    EXPECT_EQ(tex[QStringLiteral("file")].toString(), QStringLiteral("x_bones.exr"));
    EXPECT_EQ(tex[QStringLiteral("width")].toInt(), 6);
    EXPECT_EQ(tex[QStringLiteral("height")].toInt(), 5);
    EXPECT_EQ(tex[QStringLiteral("encoding")].toString(), QStringLiteral("rgba16f"));
    EXPECT_EQ(tex[QStringLiteral("texelsPerBone")].toInt(), 3);
    const QJsonArray bones = root[QStringLiteral("bones")].toArray();
    ASSERT_EQ(bones.size(), 2);
    EXPECT_EQ(bones[1].toObject()[QStringLiteral("name")].toString(), QStringLiteral("bone1"));
    EXPECT_EQ(bones[1].toObject()[QStringLiteral("parent")].toInt(), 0);
    const QJsonArray clips = root[QStringLiteral("clips")].toArray();
    ASSERT_EQ(clips.size(), 2);
    EXPECT_EQ(clips[1].toObject()[QStringLiteral("firstRow")].toInt(), 3);
    EXPECT_EQ(clips[1].toObject()[QStringLiteral("frameCount")].toInt(), 2);
    EXPECT_DOUBLE_EQ(clips[1].toObject()[QStringLiteral("fps")].toDouble(), 0.5);
    const QJsonObject skin = root[QStringLiteral("skinning")].toObject();
    EXPECT_EQ(skin[QStringLiteral("maxInfluences")].toInt(), 4);
    EXPECT_EQ(skin[QStringLiteral("sourceMaxInfluences")].toInt(), 6);
    EXPECT_EQ(skin[QStringLiteral("verticesOverLimit")].toInt(), 12);
}

TEST(BoneTextureWrite, WritesFloatTextureThatReadsBack)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const BakedClips b = taggedBake(3, 4, 3);
    const WriteResult r = write(b, tmp.path(), QStringLiteral("hero"), QStringLiteral("rgba32f"));
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.texturePath.endsWith(QStringLiteral("hero_bones.exr")));
    EXPECT_TRUE(QFile::exists(r.jsonPath));
    EXPECT_FALSE(QFile::exists(r.texturePath + QStringLiteral(".qtmesh_tmp")));

    int w = 0, h = 0;
    const auto expected = pack(b, &w, &h);
    const auto back = readOwnExr(r.texturePath, w, h, false);
    ASSERT_EQ(back.size(), expected.size());
    for (size_t i = 0; i < back.size(); ++i) ASSERT_FLOAT_EQ(back[i], expected[i]) << i;
}

TEST(BoneTextureWrite, HalfEncodingStoresHalfValues)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    BakedClips b;
    b.bones = { { QStringLiteral("a"), -1 }, { QStringLiteral("b"), 0 } };
    Clip c; c.name = QStringLiteral("Spin"); c.frameCount = 8; c.duration = 1.0; c.fps = 7.0;
    b.clips = { c };
    layoutClips(b.clips);
    std::vector<std::vector<Mat3x4>> frames;
    for (int f = 0; f < 8; ++f)
        frames.push_back({ rotationZ(0.3f * f, 0.1f * f), rotationZ(-0.2f * f, 0, 1.25f) });
    b.frames = { frames };

    const WriteResult r = write(b, tmp.path(), QStringLiteral("spin"), QStringLiteral("RGBA16F"));
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    const QJsonObject root = QJsonDocument::fromJson([&] {
        QFile f(r.jsonPath); f.open(QIODevice::ReadOnly); return f.readAll(); }()).object();
    EXPECT_EQ(root[QStringLiteral("texture")].toObject()[QStringLiteral("encoding")].toString(),
              QStringLiteral("rgba16f"));

    int w = 0, h = 0;
    const auto expected = pack(b, &w, &h);
    const auto back = readOwnExr(r.texturePath, w, h, true);
    ASSERT_EQ(back.size(), expected.size());
    for (size_t i = 0; i < back.size(); ++i)
        ASSERT_EQ(back[i], MinimalEXR::halfToFloat(MinimalEXR::floatToHalf(expected[i]))) << i;
}

TEST(BoneTextureWrite, RefusesBadInputWithoutTouchingDisk)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const BakedClips b = taggedBake(3, 4, 3);
    WriteResult r = write(b, tmp.path(), QStringLiteral("x"), QStringLiteral("png"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("rgba32f")));
    r = write(b, tmp.path(), QStringLiteral("x"), QStringLiteral("rgba32f"), 8);
    EXPECT_FALSE(r.ok);
    r = write(b, tmp.path(), QString(), QStringLiteral("rgba32f"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(QDir(tmp.path()).entryList(QDir::Files).isEmpty());
}

// ===========================================================================
// Ogre adapter — sampled matrices must reproduce Ogre's own skinning.
// ===========================================================================

namespace {

// Two-bone chain with mixed weights, root translation + rotations on both
// bones, one owner (shared vertex data).
Ogre::Entity* createBlendedSkinEntity(const std::string& name)
{
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        name + "_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* root = skel->createBone("Hips", 0);
    root->setPosition(Ogre::Vector3(0, 1, 0));
    auto* child = skel->createBone("Spine", 1);
    child->setPosition(Ogre::Vector3(0, 0.5f, 0));
    child->setOrientation(Ogre::Quaternion(Ogre::Degree(10), Ogre::Vector3::UNIT_X));
    root->addChild(child);
    skel->setBindingPose();

    auto* anim = skel->createAnimation("Sway", 1.0f);
    auto* rt = anim->createNodeTrack(0, root);
    auto* ct = anim->createNodeTrack(1, child);
    for (int k = 0; k <= 4; ++k) {
        const float t = k * 0.25f;
        auto* rk = rt->createNodeKeyFrame(t);
        rk->setTranslate(Ogre::Vector3(0.3f * std::sin(t * 6.28f), 0, 0.2f * t));
        rk->setRotation(Ogre::Quaternion(Ogre::Degree(40.0f * t), Ogre::Vector3::UNIT_Y));
        auto* ck = ct->createNodeKeyFrame(t);
        ck->setRotation(Ogre::Quaternion(Ogre::Degree(-60.0f * std::sin(t * 3.14f)), Ogre::Vector3::UNIT_Z));
        ck->setScale(Ogre::Vector3(1.0f + 0.2f * t, 1, 1));
    }
    auto* idle = skel->createAnimation("Idle", 0.5f);
    auto* it = idle->createNodeTrack(1, child);
    it->createNodeKeyFrame(0.0f);
    it->createNodeKeyFrame(0.5f)->setRotation(Ogre::Quaternion(Ogre::Degree(20), Ogre::Vector3::UNIT_X));

    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name + "_mesh", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    decl->addElement(0, 12, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    const float verts[] = {
        -0.2f, 1.0f, 0.0f,   0, 0, 1,
         0.2f, 1.2f, 0.1f,   0, 0, 1,
         0.0f, 1.6f, -0.1f,  0, 1, 0,
         0.1f, 1.9f, 0.0f,   1, 0, 0,
    };
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 4, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    vbuf->writeData(0, sizeof(verts), verts);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 4;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 6, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    const uint16_t idx[] = { 0, 1, 2, 1, 3, 2 };
    ibuf->writeData(0, sizeof(idx), idx);
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 6;

    const float weights[4][2] = { { 1.0f, 0.0f }, { 0.7f, 0.3f }, { 0.4f, 0.6f }, { 0.0f, 1.0f } };
    for (unsigned short v = 0; v < 4; ++v)
        for (unsigned short b = 0; b < 2; ++b)
            if (weights[v][b] > 0.0f) {
                Ogre::VertexBoneAssignment vba;
                vba.vertexIndex = v;
                vba.boneIndex = b;
                vba.weight = weights[v][b];
                mesh->addBoneAssignment(vba);
            }
    mesh->_notifySkeleton(skel);
    mesh->_setBounds(Ogre::AxisAlignedBox(-2, -2, -2, 3, 3, 3));
    mesh->_setBoundingSphereRadius(4.0f);
    mesh->load();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode(name.c_str());
    auto* entity = sceneMgr->createEntity(name, mesh);
    node->attachObject(entity);
    return entity;
}

// Ogre's software-skinned positions for `clip` at `t`.
std::vector<Ogre::Vector3> ogreSkinnedPositions(Ogre::Entity* entity, const std::string& clip, float t)
{
    entity->addSoftwareAnimationRequest(false);
    auto* states = entity->getAllAnimationStates();
    for (auto it = states->getAnimationStateIterator(); it.hasMoreElements();)
        it.getNext()->setEnabled(false);
    auto* s = states->getAnimationState(clip);
    s->setEnabled(true);
    s->setWeight(1.0f);
    s->setLoop(false);
    s->setTimePosition(t);
    states->_notifyDirty();
    Ogre::FrameEvent ev{};
    Ogre::Root::getSingleton()._fireFrameRenderingQueued(ev);
    entity->_updateAnimation();

    std::vector<Ogre::Vector3> out;
    const Ogre::VertexData* vd = entity->_getSkelAnimVertexData();
    const auto* pos = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    auto buf = vd->vertexBufferBinding->getBuffer(pos->getSource());
    Ogre::HardwareBufferLockGuard lock(buf, Ogre::HardwareBuffer::HBL_READ_ONLY);
    const auto* base = static_cast<const unsigned char*>(lock.pData);
    for (size_t v = 0; v < vd->vertexCount; ++v) {
        float* p;
        pos->baseVertexPointerToElement(const_cast<unsigned char*>(base + v * buf->getVertexSize()), &p);
        out.emplace_back(p[0], p[1], p[2]);
    }
    return out;
}

} // namespace

class BoneTextureOgreTest : public ::testing::Test
{
protected:
    void SetUp() override { ASSERT_TRUE(tryInitOgre()) << "Ogre init required"; }
    void TearDown() override
    {
        if (auto* mgr = Manager::getSingletonPtr()) {
            if (auto* scene = mgr->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
        }
    }
};

TEST_F(BoneTextureOgreTest, RefusesUnriggedEntityAndUnknownClip)
{
    BakedClips out;
    QString err;
    EXPECT_FALSE(sampleEntity(nullptr, {}, &out, &err));

    auto* entity = createBlendedSkinEntity("BT_Unknown");
    SampleOptions opts;
    opts.clips = { QStringLiteral("Nope") };
    EXPECT_FALSE(sampleEntity(entity, opts, &out, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("Nope")));
    opts.clips.clear();
    opts.fps = 0;
    EXPECT_FALSE(sampleEntity(entity, opts, &out, &err));
}

TEST_F(BoneTextureOgreTest, BoneOrderFollowsOgreHandles)
{
    auto* entity = createBlendedSkinEntity("BT_Order");
    BakedClips out;
    QString err;
    ASSERT_TRUE(sampleEntity(entity, {}, &out, &err)) << err.toStdString();
    ASSERT_EQ(out.bones.size(), 2u);
    EXPECT_EQ(out.bones[0].name, QStringLiteral("Hips"));
    EXPECT_EQ(out.bones[0].parent, -1);
    EXPECT_EQ(out.bones[1].name, QStringLiteral("Spine"));
    EXPECT_EQ(out.bones[1].parent, 0);
    ASSERT_EQ(out.clips.size(), 2u) << "empty clip list = every skeletal animation";
    EXPECT_EQ(out.sourceMaxInfluences, 2);
    EXPECT_EQ(out.verticesOverLimit, 0);
}

TEST_F(BoneTextureOgreTest, TextureSkinningMatchesOgreSoftwareSkinning)
{
    auto* entity = createBlendedSkinEntity("BT_Parity");
    SampleOptions opts;
    opts.clips = { QStringLiteral("Sway"), QStringLiteral("Idle") };
    opts.fps = 20.0;
    BakedClips baked;
    QString err;
    ASSERT_TRUE(sampleEntity(entity, opts, &baked, &err)) << err.toStdString();
    ASSERT_EQ(baked.clips[0].frameCount, 21);
    ASSERT_EQ(baked.clips[1].frameCount, 11);
    EXPECT_EQ(baked.clips[1].firstRow, 21);

    int w = 0, h = 0;
    const auto tex = pack(baked, &w, &h);
    ASSERT_FALSE(tex.empty());

    const float bind[4][3] = { { -0.2f, 1.0f, 0.0f }, { 0.2f, 1.2f, 0.1f },
                               { 0.0f, 1.6f, -0.1f }, { 0.1f, 1.9f, 0.0f } };
    const float weights[4][4] = { { 1, 0, 0, 0 }, { 0.7f, 0.3f, 0, 0 },
                                  { 0.4f, 0.6f, 0, 0 }, { 0, 1, 0, 0 } };
    const int indices[4] = { 0, 1, 0, 0 };

    // At every baked frame (and at the clip end, the loop-wrap trap).
    for (int ci = 0; ci < 2; ++ci) {
        const Clip& clip = baked.clips[static_cast<size_t>(ci)];
        for (int f = 0; f < clip.frameCount; f += 3) {
            const double t = clip.duration * f / (clip.frameCount - 1);
            const auto ogre = ogreSkinnedPositions(entity, clip.name.toStdString(), static_cast<float>(t));
            std::vector<Mat3x4> mats;
            for (int b = 0; b < 2; ++b) mats.push_back(sample(tex, w, clip, b, t, false));
            for (int v = 0; v < 4; ++v) {
                float p[3];
                skinVertex(mats, bind[v], nullptr, indices, weights[v], p, nullptr);
                EXPECT_NEAR(p[0], ogre[static_cast<size_t>(v)].x, 1e-4f) << clip.name.toStdString() << " t=" << t << " v=" << v;
                EXPECT_NEAR(p[1], ogre[static_cast<size_t>(v)].y, 1e-4f);
                EXPECT_NEAR(p[2], ogre[static_cast<size_t>(v)].z, 1e-4f);
            }
        }
    }
    // The last frame is the clip END, not a wrapped frame 0.
    const auto endPose = ogreSkinnedPositions(entity, "Sway", 1.0f);
    std::vector<Mat3x4> endMats;
    for (int b = 0; b < 2; ++b) endMats.push_back(unpack(tex, w, b, baked.clips[0].frameCount - 1));
    float p[3];
    skinVertex(endMats, bind[3], nullptr, indices, weights[3], p, nullptr);
    EXPECT_NEAR(p[0], endPose[3].x, 1e-4f);
    EXPECT_NEAR(p[2], endPose[3].z, 1e-4f);
}

TEST_F(BoneTextureOgreTest, RestoresTheCallersAnimationState)
{
    auto* entity = createBlendedSkinEntity("BT_Restore");
    auto* states = entity->getAllAnimationStates();
    auto* sway = states->getAnimationState("Sway");
    auto* idle = states->getAnimationState("Idle");
    sway->setEnabled(false);
    idle->setEnabled(true);
    idle->setLoop(true);
    idle->setTimePosition(0.2f);
    idle->setWeight(0.5f);

    BakedClips out;
    QString err;
    ASSERT_TRUE(sampleEntity(entity, {}, &out, &err)) << err.toStdString();
    EXPECT_FALSE(sway->getEnabled());
    EXPECT_TRUE(idle->getEnabled());
    EXPECT_TRUE(idle->getLoop());
    EXPECT_FLOAT_EQ(idle->getTimePosition(), 0.2f);
    EXPECT_FLOAT_EQ(idle->getWeight(), 0.5f);
    EXPECT_TRUE(out.clips[1].loop) << "the clip's loop flag is recorded, not the sampling clamp";
}

// ===========================================================================
// Surface core (slice B): ids, encodings, report, refusals.
// ===========================================================================

#include "VATShaderEmitter.h"

TEST(BoneTextureSurface, ModeIdAndAliases)
{
    EXPECT_EQ(modeId(), QStringLiteral("bone-texture"));
    for (const char* id : { "bone-texture", "BONE_TEXTURE", "bones", "bta", " BoneTexture " })
        EXPECT_TRUE(isModeId(QString::fromLatin1(id))) << id;
    for (const char* id : { "skeletal", "rigid", "", "bone" })
        EXPECT_FALSE(isModeId(QString::fromLatin1(id))) << id;
}

TEST(BoneTextureSurface, EncodingsAreFloatOnly)
{
    QString e;
    ASSERT_TRUE(normaliseEncoding(QString(), &e));
    EXPECT_EQ(e, QStringLiteral("rgba32f"));
    ASSERT_TRUE(normaliseEncoding(QStringLiteral("exr"), &e));
    EXPECT_EQ(e, QStringLiteral("rgba32f")) << "the VAT 'exr' id maps to float32";
    ASSERT_TRUE(normaliseEncoding(QStringLiteral("Half"), &e));
    EXPECT_EQ(e, QStringLiteral("rgba16f"));
    EXPECT_FALSE(normaliseEncoding(QStringLiteral("rgba16"), &e))
        << "a 16-bit PNG cannot hold skinning matrices";
    EXPECT_FALSE(normaliseEncoding(QStringLiteral("rgba8"), &e));
}

TEST(BoneTextureSurface, RefusesBeforeTouchingAnything)
{
    BakeRequest req;
    req.outputDir = QStringLiteral("/nonexistent/should/not/be/created");
    req.encoding = QStringLiteral("rgba8");
    BakeReport r = bakeToDirectory(nullptr, req);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("encoding")));
    req.encoding.clear();
    r = bakeToDirectory(nullptr, req);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(QDir(QStringLiteral("/nonexistent/should")).exists());
}

TEST(BoneTextureSurface, ReportJsonCarriesSizesAndClips)
{
    BakeReport r;
    r.ok = true;
    r.encoding = QStringLiteral("rgba16f");
    r.files.ok = true;
    r.files.width = 210;
    r.files.height = 72;
    r.files.texturePath = QStringLiteral("/x/Rumba_bones.exr");
    r.boneCount = 70;
    r.totalFrames = 72;
    Clip c; c.name = QStringLiteral("mixamo.com"); c.frameCount = 72; c.duration = 2.37; c.fps = 30.0;
    r.clips = { c };
    r.warnings = { QStringLiteral("w") };
    const QJsonObject o = reportToJson(r);
    EXPECT_EQ(o[QStringLiteral("mode")].toString(), QStringLiteral("bone-texture"));
    EXPECT_EQ(o[QStringLiteral("boneCount")].toInt(), 70);
    EXPECT_DOUBLE_EQ(o[QStringLiteral("textureBytes")].toDouble(), 210.0 * 72 * 8) << "half = 8 bytes/texel";
    EXPECT_EQ(o[QStringLiteral("clips")].toArray().size(), 1);
    EXPECT_EQ(o[QStringLiteral("warnings")].toArray().size(), 1);

    BakeReport failed;
    failed.error = QStringLiteral("boom");
    const QJsonObject f = reportToJson(failed);
    EXPECT_FALSE(f[QStringLiteral("ok")].toBool());
    EXPECT_EQ(f[QStringLiteral("error")].toString(), QStringLiteral("boom"));
}

TEST(BoneTextureSurface, ShaderTemplatesAreGodotOnly)
{
    EXPECT_EQ(VATShaderEmitter::boneTextureEngines(), QStringList{ QStringLiteral("godot") });
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    EXPECT_TRUE(VATShaderEmitter::writeBoneTextureShaders(tmp.path(), { QStringLiteral("unity") }).isEmpty());
    EXPECT_TRUE(VATShaderEmitter::writeBoneTextureShaders(QString(), { QStringLiteral("godot") }).isEmpty());
    // A target that links the VAT shader collection must carry the bone
    // templates too: a dropped qrc entry has to fail here, not skip.
    if (!QFile::exists(QStringLiteral(":/vat-shaders/openvat.gdshader"))) {
        SUCCEED() << "vat_shaders.qrc not linked into this test binary";
        return;
    }
    ASSERT_TRUE(QFile::exists(QStringLiteral(":/vat-shaders/bone_texture.gdshader")));
    ASSERT_TRUE(QFile::exists(QStringLiteral(":/vat-shaders/BoneTextureMesh.gd")));
    EXPECT_EQ(VATShaderEmitter::boneTextureFiles({ QStringLiteral("godot") }),
              (QStringList{ QStringLiteral("bone_texture.gdshader"), QStringLiteral("BoneTextureMesh.gd") }));
    const QStringList written = VATShaderEmitter::writeBoneTextureShaders(tmp.path(), { QStringLiteral("GODOT") });
    ASSERT_EQ(written.size(), 3) << written.join(QLatin1Char(',')).toStdString();
    EXPECT_TRUE(QFileInfo::exists(QDir(tmp.path()).filePath(QStringLiteral("bone_texture.gdshader"))));
    EXPECT_TRUE(QFileInfo::exists(QDir(tmp.path()).filePath(QStringLiteral("BoneTextureMesh.gd"))));
}

TEST_F(BoneTextureOgreTest, BakeToDirectoryWritesTextureSidecarAndWarnsOnUnknownEngine)
{
    auto* entity = createBlendedSkinEntity("BT_Surface");
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    BakeRequest req;
    req.clips = { QStringLiteral("Idle") };
    req.fps = 10.0;
    req.outputDir = tmp.path();
    req.basename = QStringLiteral("hero char");   // sanitised to hero_char
    req.shaderEngines = { QStringLiteral("unreal") };
    req.exportMesh = false;
    const BakeReport r = bakeToDirectory(entity, req);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.encoding, QStringLiteral("rgba32f"));
    EXPECT_TRUE(r.files.texturePath.endsWith(QStringLiteral("hero_char_bones.exr")));
    EXPECT_EQ(r.boneCount, 2);
    ASSERT_EQ(r.clips.size(), 1u);
    EXPECT_EQ(r.clips[0].frameCount, 6);
    EXPECT_TRUE(r.meshPath.isEmpty());
    ASSERT_FALSE(r.warnings.isEmpty());
    EXPECT_TRUE(r.warnings.join(QLatin1Char(' ')).contains(QStringLiteral("unreal")));
}


// ---------------------------------------------------------------------------
// Review round (#1174): publication, half range, size preflight, masks.
// ---------------------------------------------------------------------------

TEST(BoneTextureWrite, HalfEncodingRefusesValuesPastTheHalfRange)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    BakedClips b;
    b.bones = { { QStringLiteral("root"), -1 } };
    Clip c; c.name = QStringLiteral("Far"); c.frameCount = 1;
    b.clips = { c };
    layoutClips(b.clips);
    b.frames = { { { translation(70000.0f, 0, 0) } } };
    const WriteResult r = write(b, tmp.path(), QStringLiteral("far"), QStringLiteral("rgba16f"));
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("rgba32f"))) << r.error.toStdString();
    EXPECT_TRUE(QDir(tmp.path()).entryList(QDir::Files).isEmpty());
    // float32 holds it fine.
    EXPECT_TRUE(write(b, tmp.path(), QStringLiteral("far"), QStringLiteral("rgba32f")).ok);
}

namespace {
QByteArray readAllOf(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// width/height from the EXR dataWindow attribute (box2i: xMin yMin xMax yMax).
QSize exrSize(const QByteArray& exr)
{
    const QByteArray tag("dataWindow\0box2i\0", 17);
    const int at = exr.indexOf(tag);
    if (at < 0) return {};
    const char* p = exr.constData() + at + tag.size() + 4;
    int32_t box[4];
    std::memcpy(box, p, sizeof(box));
    return QSize(box[2] - box[0] + 1, box[3] - box[1] + 1);
}

struct SidecarGuard {
    ~SidecarGuard() { detail::setSidecarWriteFailureForTest(false); }
};
} // namespace

TEST(BoneTextureWrite, FailedSidecarKeepsThePreviousPairIntact)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(write(taggedBake(2, 3, 2), tmp.path(), QStringLiteral("hero"), QStringLiteral("rgba32f")).ok);
    const QString texPath = QDir(tmp.path()).filePath(QStringLiteral("hero_bones.exr"));
    const QString jsonPath = QDir(tmp.path()).filePath(QStringLiteral("hero_bones.json"));
    const QByteArray oldTexture = readAllOf(texPath);
    const QByteArray oldSidecar = readAllOf(jsonPath);

    SidecarGuard guard;
    detail::setSidecarWriteFailureForTest(true);
    const WriteResult r = write(taggedBake(4, 5, 5), tmp.path(), QStringLiteral("hero"), QStringLiteral("rgba32f"));
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(readAllOf(texPath), oldTexture) << "the previous texture must be put back";
    EXPECT_EQ(readAllOf(jsonPath), oldSidecar) << "the previous sidecar must be untouched";
    for (const QString& f : QDir(tmp.path()).entryList(QDir::Files | QDir::Hidden))
        EXPECT_FALSE(f.endsWith(QStringLiteral(".tmp")) || f.endsWith(QStringLiteral(".lock"))) << f.toStdString();
}

TEST(BoneTextureWrite, FailedSidecarOnAFirstBakeLeavesNothing)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    SidecarGuard guard;
    detail::setSidecarWriteFailureForTest(true);
    EXPECT_FALSE(write(taggedBake(2, 3, 2), tmp.path(), QStringLiteral("hero"), QStringLiteral("rgba32f")).ok);
    EXPECT_TRUE(QDir(tmp.path()).entryList(QDir::Files).isEmpty());
}

TEST(BoneTextureWrite, UnreadableExistingTextureIsNotReplaced)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ASSERT_TRUE(write(taggedBake(2, 3, 2), tmp.path(), QStringLiteral("hero"), QStringLiteral("rgba32f")).ok);
    const QString texPath = QDir(tmp.path()).filePath(QStringLiteral("hero_bones.exr"));
    const QString jsonPath = QDir(tmp.path()).filePath(QStringLiteral("hero_bones.json"));
    const QByteArray oldSidecar = readAllOf(jsonPath);
    QFile::setPermissions(texPath, QFileDevice::Permissions());
    QFile probe(texPath);
    if (probe.open(QIODevice::ReadOnly)) {   // e.g. running as root
        QFile::setPermissions(texPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        SUCCEED() << "permissions are not enforced for this user";
        return;
    }
    const WriteResult r = write(taggedBake(4, 5, 5), tmp.path(), QStringLiteral("hero"), QStringLiteral("rgba32f"));
    QFile::setPermissions(texPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("cannot read"))) << r.error.toStdString();
    EXPECT_EQ(readAllOf(jsonPath), oldSidecar);
    EXPECT_EQ(exrSize(readAllOf(texPath)), QSize(6, 5)) << "old 2-bone texture still in place";
}

TEST(BoneTextureWrite, ConcurrentBakesLeaveAMatchingPair)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const BakedClips a = taggedBake(2, 3, 2);   // 6 x 5
    const BakedClips b = taggedBake(5, 7, 4);   // 15 x 11
    std::atomic<int> failures{0};
    auto worker = [&](const BakedClips& bake) {
        for (int i = 0; i < 15; ++i)
            if (!write(bake, tmp.path(), QStringLiteral("crowd"), QStringLiteral("rgba32f")).ok)
                ++failures;
    };
    std::thread t1(worker, std::cref(a));
    std::thread t2(worker, std::cref(b));
    t1.join();
    t2.join();
    EXPECT_EQ(failures.load(), 0);
    const QJsonObject tex = QJsonDocument::fromJson(
        readAllOf(QDir(tmp.path()).filePath(QStringLiteral("crowd_bones.json")))).object()
        [QStringLiteral("texture")].toObject();
    const QSize size = exrSize(readAllOf(QDir(tmp.path()).filePath(QStringLiteral("crowd_bones.exr"))));
    EXPECT_EQ(size, QSize(tex[QStringLiteral("width")].toInt(), tex[QStringLiteral("height")].toInt()))
        << "the sidecar must describe the texture next to it";
}

TEST(BoneTextureLayout, FrameCountSaturatesInsteadOfOverflowing)
{
    EXPECT_EQ(frameCountFor(1.0e6, 1.0e6), std::numeric_limits<int>::max());
    EXPECT_EQ(frameCountFor(1.0, 30.0), 31);
}

TEST_F(BoneTextureOgreTest, SizeLimitIsCheckedBeforeSampling)
{
    auto* entity = createBlendedSkinEntity("BT_Preflight");
    SampleOptions opts;
    opts.fps = 1.0e9;   // ~1e9 frames: must fail fast, not allocate
    BakedClips out;
    QString err;
    EXPECT_FALSE(sampleEntity(entity, opts, &out, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("lower the fps"))) << err.toStdString();
    EXPECT_TRUE(out.frames.empty());
    opts.fps = 10.0;
    opts.maxTextureSize = 5;   // 2 bones need 6 texels
    EXPECT_FALSE(sampleEntity(entity, opts, &out, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("texel-wide"))) << err.toStdString();
}

TEST_F(BoneTextureOgreTest, BlendMasksAreLiftedWhileSamplingAndRestored)
{
    auto* entity = createBlendedSkinEntity("BT_Mask");
    auto* sway = entity->getAnimationState("Sway");
    // Mask the Spine (handle 1) out entirely, as a layered graph would.
    sway->createBlendMask(2, 1.0f);
    sway->setBlendMaskEntry(1, 0.0f);

    BakedClips masked;
    QString err;
    SampleOptions opts;
    opts.clips = { QStringLiteral("Sway") };
    opts.fps = 4.0;
    ASSERT_TRUE(sampleEntity(entity, opts, &masked, &err)) << err.toStdString();
    ASSERT_TRUE(sway->hasBlendMask()) << "the caller's mask is restored";
    EXPECT_FLOAT_EQ(sway->getBlendMaskEntry(1), 0.0f);

    sway->destroyBlendMask();
    BakedClips plain;
    ASSERT_TRUE(sampleEntity(entity, opts, &plain, &err)) << err.toStdString();
    // The masked bake must equal the unmasked one (the mask was lifted).
    for (size_t f = 0; f < plain.frames[0].size(); ++f)
        for (size_t b = 0; b < 2; ++b)
            expectMatNear(masked.frames[0][f][b], plain.frames[0][f][b], 1e-6f);
}

TEST_F(BoneTextureOgreTest, RejectedRequestLeavesAnExistingBakeUntouched)
{
    auto* entity = createBlendedSkinEntity("BT_Untouched");
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    BakeRequest req;
    req.outputDir = tmp.path();
    req.basename = QStringLiteral("hero");
    req.fps = 10.0;
    req.exportMesh = false;
    ASSERT_TRUE(bakeToDirectory(entity, req).ok);
    const QStringList before = QDir(tmp.path()).entryList(QDir::Files);
    QFile j(QDir(tmp.path()).filePath(QStringLiteral("hero_bones.json")));
    ASSERT_TRUE(j.open(QIODevice::ReadOnly));
    const QByteArray sidecar = j.readAll();
    j.close();

    req.clips = { QStringLiteral("NoSuchClip") };
    req.exportMesh = true;
    const BakeReport r = bakeToDirectory(entity, req);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(QDir(tmp.path()).entryList(QDir::Files), before) << "no glb written for a rejected bake";
    ASSERT_TRUE(j.open(QIODevice::ReadOnly));
    EXPECT_EQ(j.readAll(), sidecar);
}

TEST_F(BoneTextureOgreTest, FailedMeshExportLeavesTheExistingBakeUntouched)
{
    auto* entity = createBlendedSkinEntity("BT_MeshFail");
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    // An existing bake: texture + sidecar + a .glb (stand-in bytes).
    BakeRequest req;
    req.outputDir = tmp.path();
    req.basename = QStringLiteral("hero");
    req.clips = { QStringLiteral("Idle") };
    req.fps = 10.0;
    req.exportMesh = false;
    ASSERT_TRUE(bakeToDirectory(entity, req).ok);
    const QString glbPath = QDir(tmp.path()).filePath(QStringLiteral("hero.glb"));
    {
        QFile f(glbPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("previous glb");
    }
    const QString texPath = QDir(tmp.path()).filePath(QStringLiteral("hero_bones.exr"));
    const QString jsonPath = QDir(tmp.path()).filePath(QStringLiteral("hero_bones.json"));
    const QByteArray oldTex = readAllOf(texPath), oldJson = readAllOf(jsonPath);

    struct Guard { ~Guard() { detail::setMeshExportFailureForTest(false); } } guard;
    detail::setMeshExportFailureForTest(true);
    req.clips = { QStringLiteral("Sway") };   // a different bake
    req.exportMesh = true;
    const BakeReport r = bakeToDirectory(entity, req);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("nothing was written"))) << r.error.toStdString();
    EXPECT_EQ(readAllOf(glbPath), QByteArray("previous glb"));
    EXPECT_EQ(readAllOf(texPath), oldTex);
    EXPECT_EQ(readAllOf(jsonPath), oldJson);
}
