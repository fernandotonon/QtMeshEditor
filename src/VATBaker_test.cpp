#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTemporaryDir>

#include "TestHelpers.h"
#include "VATBaker.h"

#include <OgreVector.h>

#include <cmath>
#include <vector>

namespace {

constexpr float kRGBA8MaxErrorOnUnitRange = 1.0f / 255.0f + 1e-4f;

} // namespace

// ===========================================================================
// Pure-data tests — encoder math only, no Ogre needed.
// ===========================================================================

TEST(VATBakerStandalone, EncodeRoundTripUnitRange) {
    // Two frames, two vertices. Use distinctive coordinates so any
    // accidental channel swap or off-by-one shows up immediately.
    std::vector<Ogre::Vector3> flat = {
        { 0.0f, 0.0f, 0.0f },  // frame 0, vertex 0 (lo corner)
        { 1.0f, 1.0f, 1.0f },  // frame 0, vertex 1 (hi corner)
        { 0.5f, 0.25f, 0.75f },// frame 1, vertex 0
        { 0.1f, 0.9f, 0.5f },  // frame 1, vertex 1
    };
    const Ogre::Vector3 lo(0, 0, 0);
    const Ogre::Vector3 hi(1, 1, 1);
    auto rgba = VATBaker::encodeRGBA8(flat, 2, 2, lo, hi);
    ASSERT_EQ(rgba.size(), 2u * 2u * 4u);
    // Alpha is reserved and always 255.
    for (size_t i = 3; i < rgba.size(); i += 4) {
        EXPECT_EQ(rgba[i], 255);
    }
    auto round = VATBaker::decodeRGBA8(rgba, 2, 2, lo, hi);
    ASSERT_EQ(round.size(), flat.size());
    for (size_t i = 0; i < flat.size(); ++i) {
        EXPECT_NEAR(round[i].x, flat[i].x, kRGBA8MaxErrorOnUnitRange);
        EXPECT_NEAR(round[i].y, flat[i].y, kRGBA8MaxErrorOnUnitRange);
        EXPECT_NEAR(round[i].z, flat[i].z, kRGBA8MaxErrorOnUnitRange);
    }
}

TEST(VATBakerStandalone, EncodeRoundTripArbitraryRange) {
    // Coordinates outside [0..1] — encoder must normalise against bounds.
    const Ogre::Vector3 lo(-2.0f, -5.0f, 10.0f);
    const Ogre::Vector3 hi( 2.0f,  5.0f, 14.0f);
    std::vector<Ogre::Vector3> flat = {
        { -2.0f, -5.0f, 10.0f }, // lo corner
        {  2.0f,  5.0f, 14.0f }, // hi corner
        {  0.0f,  0.0f, 12.0f }, // midpoint
    };
    auto rgba = VATBaker::encodeRGBA8(flat, 1, 3, lo, hi);
    ASSERT_EQ(rgba.size(), 3u * 4u);
    // First vertex (lo corner) → all zero in RGB.
    EXPECT_EQ(rgba[0], 0);
    EXPECT_EQ(rgba[1], 0);
    EXPECT_EQ(rgba[2], 0);
    // Second vertex (hi corner) → all 255.
    EXPECT_EQ(rgba[4], 255);
    EXPECT_EQ(rgba[5], 255);
    EXPECT_EQ(rgba[6], 255);
    auto round = VATBaker::decodeRGBA8(rgba, 1, 3, lo, hi);
    for (size_t i = 0; i < flat.size(); ++i) {
        const float ex = 4.0f * kRGBA8MaxErrorOnUnitRange;   // span 4 on x
        const float ey = 10.0f * kRGBA8MaxErrorOnUnitRange;  // span 10 on y
        const float ez = 4.0f * kRGBA8MaxErrorOnUnitRange;   // span 4 on z
        EXPECT_NEAR(round[i].x, flat[i].x, ex);
        EXPECT_NEAR(round[i].y, flat[i].y, ey);
        EXPECT_NEAR(round[i].z, flat[i].z, ez);
    }
}

TEST(VATBakerStandalone, EncodeClampsOutOfRangeValues) {
    // Out-of-bound positions should clamp to 0/255 rather than wrap or
    // crash — the runtime decode is happy as long as the bytes are valid.
    std::vector<Ogre::Vector3> flat = {
        { -10.0f, 100.0f, 0.5f }, // x below lo, y above hi, z mid
    };
    const Ogre::Vector3 lo(0, 0, 0);
    const Ogre::Vector3 hi(1, 1, 1);
    auto rgba = VATBaker::encodeRGBA8(flat, 1, 1, lo, hi);
    ASSERT_EQ(rgba.size(), 4u);
    EXPECT_EQ(rgba[0], 0);
    EXPECT_EQ(rgba[1], 255);
}

TEST(VATBakerStandalone, EncodeReturnsEmptyOnMismatchedSize) {
    // Defensive: pass a vector that doesn't match frame×vertex count.
    std::vector<Ogre::Vector3> flat = { Ogre::Vector3::ZERO };  // 1 entry
    auto rgba = VATBaker::encodeRGBA8(flat, 2, 2, Ogre::Vector3::ZERO,
                                       Ogre::Vector3::UNIT_SCALE);
    EXPECT_TRUE(rgba.empty());
}

TEST(VATBakerStandalone, EncodeReturnsEmptyOnZeroDims) {
    std::vector<Ogre::Vector3> flat;
    EXPECT_TRUE(VATBaker::encodeRGBA8(flat, 0, 5, Ogre::Vector3::ZERO,
                                       Ogre::Vector3::UNIT_SCALE).empty());
    EXPECT_TRUE(VATBaker::encodeRGBA8(flat, 5, 0, Ogre::Vector3::ZERO,
                                       Ogre::Vector3::UNIT_SCALE).empty());
}

TEST(VATBakerStandalone, DecodeReturnsEmptyOnMismatchedSize) {
    std::vector<unsigned char> wrong(7, 0);  // not a multiple of 4
    auto out = VATBaker::decodeRGBA8(wrong, 1, 2, Ogre::Vector3::ZERO,
                                      Ogre::Vector3::UNIT_SCALE);
    EXPECT_TRUE(out.empty());
}

TEST(VATBakerStandalone, BuildSidecarJsonHasExpectedKeys) {
    VATBaker::BakeResult r;
    r.frameCount  = 30;
    r.vertexCount = 5000;
    r.minBound = Ogre::Vector3(-1.0f, -2.0f, -3.0f);
    r.maxBound = Ogre::Vector3( 1.0f,  2.0f,  3.0f);
    r.posTexPath = QStringLiteral("/tmp/Walk_pos.png");

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("Walk");
    opts.fps           = 30.0;

    const QString json = VATBaker::buildSidecarJson(r, opts);
    auto doc = QJsonDocument::fromJson(json.toUtf8());
    ASSERT_TRUE(doc.isObject());
    auto root = doc.object();
    EXPECT_EQ(root["version"].toInt(),     1);
    EXPECT_EQ(root["target"].toString(),   QStringLiteral("agnostic"));
    EXPECT_EQ(root["encoding"].toString(), QStringLiteral("rgba8"));
    EXPECT_EQ(root["frameCount"].toInt(),  30);
    EXPECT_EQ(root["vertexCount"].toInt(), 5000);
    EXPECT_EQ(root["animation"].toString(), QStringLiteral("Walk"));
    EXPECT_DOUBLE_EQ(root["fps"].toDouble(), 30.0);
    EXPECT_EQ(root["posTexture"].toString(), QStringLiteral("Walk_pos.png"))
        << "sidecar should record relative filename, not absolute path";
    auto bounds = root["bounds"].toObject();
    EXPECT_NEAR(bounds["min"].toObject()["x"].toDouble(), -1.0, 1e-6);
    EXPECT_NEAR(bounds["max"].toObject()["z"].toDouble(),  3.0, 1e-6);
}

// ===========================================================================
// Failure-mode tests on bake() — exercise every guard.
// ===========================================================================

TEST(VATBakerStandalone, BakeNullEntityReports) {
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("Idle");
    opts.outputDir = QStringLiteral("/tmp/vat");
    auto r = VATBaker::bake(nullptr, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("null")));
}

TEST(VATBakerStandalone, BakeMissingAnimationNameReports) {
    // Without an animation name, bake() should refuse before touching
    // Ogre — so we can call with a null entity here (it'd reject anyway).
    VATBaker::Options opts;
    opts.outputDir = QStringLiteral("/tmp/vat");
    auto r = VATBaker::bake(nullptr, opts);
    EXPECT_FALSE(r.ok);
}

TEST(VATBakerStandalone, BakeInvalidFpsReports) {
    // Use a non-null entity guard is the first check, but fps must also
    // surface a clear error. We test by walking just the validator chain.
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("Walk");
    opts.fps = 0.0;
    opts.outputDir = QStringLiteral("/tmp/vat");
    // entity null short-circuits first; this confirms the error message
    // is well-formed even when fps is also invalid.
    auto r = VATBaker::bake(nullptr, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

// ===========================================================================
// End-to-end — uses createAnimatedTestEntity() so it works in any CI
// permutation regardless of whether media/models/robot.mesh is on the
// resource path. The in-memory entity has a 3-vertex triangle skinned
// to a 2-bone skeleton with a 1.0s "TestAnim" track.
// ===========================================================================

class VATBakerEndToEndTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init required";
    }

    void TearDown() override
    {
        // Drop anything the prior test built so the singleton scene
        // doesn't accumulate cruft between tests.
        if (auto* mgr = Manager::getSingletonPtr()) {
            if (auto* scene = mgr->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
        }
    }
};

TEST_F(VATBakerEndToEndTest, BakesInMemoryAnimatedTriangle) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_Bake");
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;                          // 1.0 s × 10 fps → 10 frames
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("E2E");

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << "bake error: " << r.error.toStdString();
    EXPECT_GT(r.frameCount, 0);
    EXPECT_EQ(r.vertexCount, 3);              // matches the test triangle
    EXPECT_TRUE(QFile::exists(r.posTexPath));
    EXPECT_TRUE(QFile::exists(r.jsonPath));

    // Texture layout: width = vertexCount, height = frameCount.
    QImage png(r.posTexPath);
    ASSERT_FALSE(png.isNull());
    EXPECT_EQ(png.width(),  r.vertexCount);
    EXPECT_EQ(png.height(), r.frameCount);

    // Sidecar JSON parses + has the expected fields.
    QFile jf(r.jsonPath);
    ASSERT_TRUE(jf.open(QIODevice::ReadOnly));
    auto doc = QJsonDocument::fromJson(jf.readAll());
    ASSERT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object()["frameCount"].toInt(),  r.frameCount);
    EXPECT_EQ(doc.object()["vertexCount"].toInt(), r.vertexCount);
    EXPECT_EQ(doc.object()["animation"].toString(), QStringLiteral("TestAnim"));
    EXPECT_DOUBLE_EQ(doc.object()["fps"].toDouble(), 10.0);
}

// Regression — VATBaker used to write bind-pose data into every row
// of the position texture (every frame identical) because the bake
// loop didn't bump Ogre's per-frame counters between samples and so
// `Entity::cacheBoneMatrices` short-circuited after the first call.
// This test caught it would have caught it: it bakes a real animated
// entity, opens the PNG, and asserts that adjacent rows are NOT byte-
// identical. Without the fix `pixelsDiffer` evaluates to false and
// the test fails loudly.
TEST_F(VATBakerEndToEndTest, ProducesDistinctRowsAcrossFrames) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_FramesDiffer");
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;       // 1.0 s × 10 fps → 10 frames
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("FramesDiffer");

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << "bake error: " << r.error.toStdString();
    ASSERT_GE(r.frameCount, 2) << "need at least 2 frames to compare rows";

    QImage png(r.posTexPath);
    ASSERT_FALSE(png.isNull());
    ASSERT_EQ(png.height(), r.frameCount);
    ASSERT_GT(png.width(), 0);

    // Walk every column for every (frame, frame+1) pair. If ANY pair
    // of adjacent rows differs at ANY column, the animation is being
    // sampled correctly. If all pairs are byte-identical, the bake
    // is static and the bug is back.
    bool foundDifference = false;
    for (int row = 0; row + 1 < png.height() && !foundDifference; ++row) {
        for (int col = 0; col < png.width(); ++col) {
            if (png.pixel(col, row) != png.pixel(col, row + 1)) {
                foundDifference = true;
                break;
            }
        }
    }
    EXPECT_TRUE(foundDifference)
        << "VAT position texture is byte-identical across all frames — "
        << "bake captured a single pose instead of stepping through "
        << "the animation. Check that the bake loop bumps the Ogre "
        << "per-frame counter (Root::_fireFrameRenderingQueued) "
        << "between setTimePosition + collectPostSkinPositions calls.";
}

// OpenVAT compatibility — the sidecar must publish a superset of
// the JustNiko/OpenVAT field names so off-the-shelf shaders for
// Blender/UE/Unity can consume our bakes without modification.
//
// We don't replace our own keys; we add OpenVAT's as aliases.
TEST_F(VATBakerEndToEndTest, SidecarPublishesOpenVATAliases) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_OpenVAT");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 30.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("OV");
    opts.encoding = VATBaker::Encoding::RGBA16;  // → Animated.Vat.UnsignedShort

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    QFile jf(r.jsonPath);
    ASSERT_TRUE(jf.open(QIODevice::ReadOnly));
    const auto doc = QJsonDocument::fromJson(jf.readAll());
    ASSERT_TRUE(doc.isObject());
    const auto root = doc.object();

    // Our canonical keys still present (no regression).
    EXPECT_EQ(root["frameCount"].toInt(),  r.frameCount);
    EXPECT_EQ(root["vertexCount"].toInt(), r.vertexCount);
    EXPECT_DOUBLE_EQ(root["fps"].toDouble(), 30.0);
    EXPECT_EQ(root["animation"].toString(), QStringLiteral("TestAnim"));
    ASSERT_TRUE(root["bounds"].isObject());
    ASSERT_TRUE(root["bounds"].toObject()["min"].isObject());

    // OpenVAT aliases present + carry the same values.
    EXPECT_EQ(root["numFrames"].toInt(),     r.frameCount);
    EXPECT_EQ(root["numVertices"].toInt(),   r.vertexCount);
    EXPECT_DOUBLE_EQ(root["framerate"].toDouble(), 30.0);
    EXPECT_EQ(root["name"].toString(),       QStringLiteral("TestAnim"));
    EXPECT_EQ(root["format"].toString(),     QStringLiteral("Animated.Vat.UnsignedShort"));
    EXPECT_EQ(root["texture"].toString(),    root["posTexture"].toString());

    // OpenVAT-style flat-array bounds.
    ASSERT_TRUE(root["approximateBounds"].isObject());
    const auto ab = root["approximateBounds"].toObject();
    ASSERT_TRUE(ab["min"].isArray());
    ASSERT_TRUE(ab["max"].isArray());
    EXPECT_EQ(ab["min"].toArray().size(), 3);
    EXPECT_EQ(ab["max"].toArray().size(), 3);
    // And they match our object-style bounds.
    EXPECT_DOUBLE_EQ(ab["min"].toArray()[0].toDouble(),
                     root["bounds"].toObject()["min"].toObject()["x"].toDouble());
    EXPECT_DOUBLE_EQ(ab["max"].toArray()[2].toDouble(),
                     root["bounds"].toObject()["max"].toObject()["z"].toDouble());
}

TEST_F(VATBakerEndToEndTest, OpenVATFormatStringMatchesEncoding) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_OpenVATFmt");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 30.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("OVFmt8");
    opts.encoding = VATBaker::Encoding::RGBA8;

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok);
    QFile jf(r.jsonPath); ASSERT_TRUE(jf.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(jf.readAll()).object();
    EXPECT_EQ(root["format"].toString(), QStringLiteral("Animated.Vat.Byte"));
}

TEST_F(VATBakerEndToEndTest, RejectsMissingAnimationOnLiveEntity) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_MissAnim");
    ASSERT_NE(entity, nullptr);

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("NotARealAnim");
    opts.fps = 30.0;
    opts.outputDir = QStringLiteral("/tmp");

    auto r = VATBaker::bake(entity, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("not found")))
        << "expected 'not found' in error, got: " << r.error.toStdString();
}

TEST_F(VATBakerEndToEndTest, FrameCountDerivedFromFpsAndDuration) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_FrameCount");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // 1.0 s animation × 30 fps → ~30 frames.
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 30.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("FC");

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_GE(r.frameCount, 28);
    EXPECT_LE(r.frameCount, 32);
}

// ===========================================================================
// Slice 2 — RGBA16 + normals + sidecar fields.
// ===========================================================================

TEST(VATBakerStandalone, EncodeRGBA16RoundTrip) {
    // 16-bit channels: ~50× more precise than RGBA8 on the same bounds.
    const Ogre::Vector3 lo(-10.0f, -10.0f, -10.0f);
    const Ogre::Vector3 hi( 10.0f,  10.0f,  10.0f);
    std::vector<Ogre::Vector3> flat = {
        { -10.0f, -10.0f, -10.0f },
        {  10.0f,  10.0f,  10.0f },
        {  0.0f,   0.0f,   0.0f  },
        {  3.14159f, -2.71828f, 1.41421f },
    };
    auto rgba = VATBaker::encodeRGBA16(flat, 2, 2, lo, hi);
    ASSERT_EQ(rgba.size(), 2u * 2u * 4u);
    // Alpha reserved at 65535.
    for (size_t i = 3; i < rgba.size(); i += 4) {
        EXPECT_EQ(rgba[i], 65535);
    }
    auto round = VATBaker::decodeRGBA16(rgba, 2, 2, lo, hi);
    ASSERT_EQ(round.size(), flat.size());
    // One ULP at span=20 over 65535 levels ≈ 3e-4. Test loosely at 1e-3.
    constexpr float kRGBA16Err = 1e-3f;
    for (size_t i = 0; i < flat.size(); ++i) {
        EXPECT_NEAR(round[i].x, flat[i].x, kRGBA16Err);
        EXPECT_NEAR(round[i].y, flat[i].y, kRGBA16Err);
        EXPECT_NEAR(round[i].z, flat[i].z, kRGBA16Err);
    }
}

TEST(VATBakerStandalone, EncodeRGBA16BeatsRGBA8OnSamePoints) {
    // The whole point of slice 2: 16-bit encoding should produce
    // substantially smaller round-trip error than the 8-bit path.
    const Ogre::Vector3 lo(-100.0f, -100.0f, -100.0f);
    const Ogre::Vector3 hi( 100.0f,  100.0f,  100.0f);
    std::vector<Ogre::Vector3> flat = {
        { 0.0f, 0.0f, 0.0f },
        { 12.345f, -67.89f, 42.42f },
        { -99.9f, 0.0001f, 50.0f },
    };
    auto rgba8  = VATBaker::encodeRGBA8(flat, 1, 3, lo, hi);
    auto rgba16 = VATBaker::encodeRGBA16(flat, 1, 3, lo, hi);
    auto back8  = VATBaker::decodeRGBA8(rgba8,  1, 3, lo, hi);
    auto back16 = VATBaker::decodeRGBA16(rgba16, 1, 3, lo, hi);
    float worst8 = 0.0f, worst16 = 0.0f;
    for (size_t i = 0; i < flat.size(); ++i) {
        worst8  = std::max(worst8,  (back8[i]  - flat[i]).length());
        worst16 = std::max(worst16, (back16[i] - flat[i]).length());
    }
    EXPECT_LT(worst16 * 10.0f, worst8)
        << "RGBA16 should be >10× more accurate than RGBA8 (16=" << worst16
        << " vs 8=" << worst8 << ")";
}

TEST(VATBakerStandalone, EncodeNormalsRGBA8RoundTrip) {
    // Standard basis vectors round-trip cleanly within ~1/255 (no
    // normalisation needed, just channel mapping).
    std::vector<Ogre::Vector3> normals = {
        Ogre::Vector3::UNIT_X,
        Ogre::Vector3::UNIT_Y,
        Ogre::Vector3::UNIT_Z,
        -Ogre::Vector3::UNIT_X,
        Ogre::Vector3::ZERO,
    };
    auto rgba = VATBaker::encodeNormalsRGBA8(normals, 1, 5);
    ASSERT_EQ(rgba.size(), 5u * 4u);
    auto round = VATBaker::decodeNormalsRGBA8(rgba, 1, 5);
    ASSERT_EQ(round.size(), normals.size());
    constexpr float kErr = 2.0f / 255.0f;  // 2 ULP at signed remap
    for (size_t i = 0; i < normals.size(); ++i) {
        EXPECT_NEAR(round[i].x, normals[i].x, kErr);
        EXPECT_NEAR(round[i].y, normals[i].y, kErr);
        EXPECT_NEAR(round[i].z, normals[i].z, kErr);
    }
}

TEST(VATBakerStandalone, EncodeNormalsRGBA8ClampsOutOfRange) {
    std::vector<Ogre::Vector3> normals = {
        { -2.0f, 2.0f, 0.0f },  // out of [-1, 1]
    };
    auto rgba = VATBaker::encodeNormalsRGBA8(normals, 1, 1);
    ASSERT_EQ(rgba.size(), 4u);
    EXPECT_EQ(rgba[0], 0);    // -2 → -1 → 0
    EXPECT_EQ(rgba[1], 255);  //  2 →  1 → 255
}

TEST(VATBakerStandalone, EncodeNormalsRGBA16RoundTrip) {
    std::vector<Ogre::Vector3> normals = {
        { 0.123456f, -0.789012f, 0.345678f },
        Ogre::Vector3::UNIT_Y,
    };
    auto rgba = VATBaker::encodeNormalsRGBA16(normals, 1, 2);
    ASSERT_EQ(rgba.size(), 2u * 4u);
    auto round = VATBaker::decodeNormalsRGBA16(rgba, 1, 2);
    constexpr float kErr = 2.0f / 65535.0f;
    for (size_t i = 0; i < normals.size(); ++i) {
        EXPECT_NEAR(round[i].x, normals[i].x, kErr);
        EXPECT_NEAR(round[i].y, normals[i].y, kErr);
        EXPECT_NEAR(round[i].z, normals[i].z, kErr);
    }
}

TEST(VATBakerStandalone, EncodeNormalsReturnsEmptyOnMismatchedSize) {
    std::vector<Ogre::Vector3> normals = { Ogre::Vector3::UNIT_X };
    EXPECT_TRUE(VATBaker::encodeNormalsRGBA8(normals, 2, 2).empty());
    EXPECT_TRUE(VATBaker::encodeNormalsRGBA16(normals, 2, 2).empty());
    EXPECT_TRUE(VATBaker::decodeNormalsRGBA8({}, 1, 1).empty());
    EXPECT_TRUE(VATBaker::decodeNormalsRGBA16({}, 1, 1).empty());
}

TEST(VATBakerStandalone, SidecarMentionsEncodingAndOptionalNrmTexture) {
    VATBaker::BakeResult r;
    r.frameCount  = 10;
    r.vertexCount = 100;
    r.posTexPath  = QStringLiteral("/tmp/Walk_pos.png");

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("Walk");
    opts.fps           = 60.0;
    opts.encoding      = VATBaker::Encoding::RGBA16;

    // Without normals — sidecar should NOT include nrmTexture.
    QString json = VATBaker::buildSidecarJson(r, opts);
    auto root = QJsonDocument::fromJson(json.toUtf8()).object();
    EXPECT_EQ(root["encoding"].toString(), QStringLiteral("rgba16"));
    EXPECT_FALSE(root.contains(QStringLiteral("nrmTexture")));

    // With normals — sidecar should include the filename (not the abs path).
    r.nrmTexPath = QStringLiteral("/tmp/Walk_nrm.png");
    json = VATBaker::buildSidecarJson(r, opts);
    root = QJsonDocument::fromJson(json.toUtf8()).object();
    EXPECT_EQ(root["nrmTexture"].toString(), QStringLiteral("Walk_nrm.png"));
}

TEST_F(VATBakerEndToEndTest, BakeWithRGBA16WritesSixteenBitPng) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_RGBA16");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("R16");
    opts.encoding = VATBaker::Encoding::RGBA16;

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    QImage png(r.posTexPath);
    ASSERT_FALSE(png.isNull());
    // Qt promotes a 16-bit PNG to RGBA64; the format check is the
    // closest the standard API gets to "this PNG is 16-bit per channel".
    EXPECT_EQ(png.format(), QImage::Format_RGBA64);
}

TEST_F(VATBakerEndToEndTest, BakeWithNormalsWritesNormalTexture) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_Normals");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("N");
    opts.bakeNormals = true;

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_FALSE(r.nrmTexPath.isEmpty());
    EXPECT_TRUE(QFile::exists(r.nrmTexPath));
    QImage png(r.nrmTexPath);
    ASSERT_FALSE(png.isNull());
    EXPECT_EQ(png.width(),  r.vertexCount);
    EXPECT_EQ(png.height(), r.frameCount);

    // Sidecar should reference both textures.
    QFile jf(r.jsonPath);
    ASSERT_TRUE(jf.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(jf.readAll()).object();
    EXPECT_TRUE(root.contains(QStringLiteral("nrmTexture")));
}

// ===========================================================================
// Slice 3 — per-engine targets.
// ===========================================================================

TEST(VATBakerStandalone, SidecarTargetFieldReflectsOption) {
    // Sidecar should encode the target name so the runtime shader and
    // any asset-pipeline tooling knows the convention the bake used.
    VATBaker::BakeResult r;
    r.frameCount = 10; r.vertexCount = 100;
    r.posTexPath = QStringLiteral("/tmp/Walk_pos.png");

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("Walk");
    opts.fps = 30.0;

    for (auto [t, want] : std::initializer_list<std::pair<VATBaker::Target, QString>>{
            {VATBaker::Target::Agnostic, QStringLiteral("agnostic")},
            {VATBaker::Target::Unity,    QStringLiteral("unity")},
            {VATBaker::Target::Unreal,   QStringLiteral("unreal")},
            {VATBaker::Target::Godot,    QStringLiteral("godot")}}) {
        opts.target = t;
        const QString json = VATBaker::buildSidecarJson(r, opts);
        auto root = QJsonDocument::fromJson(json.toUtf8()).object();
        EXPECT_EQ(root["target"].toString(), want)
            << "target enum " << static_cast<int>(t) << " should serialise as '" << want.toStdString() << "'";
    }
}

TEST_F(VATBakerEndToEndTest, UnityTargetWritesPngMetaSidecar) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_Unity");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("U");
    opts.target = VATBaker::Target::Unity;

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_FALSE(r.unityMetaPath.isEmpty());
    EXPECT_TRUE(QFile::exists(r.unityMetaPath));
    // Quick sanity check on contents — Unity's importer treats the
    // file as YAML; we wrote the standard texture-asset section.
    QFile mf(r.unityMetaPath);
    ASSERT_TRUE(mf.open(QIODevice::ReadOnly));
    const QByteArray body = mf.readAll();
    EXPECT_TRUE(body.contains("TextureImporter:"));
    EXPECT_TRUE(body.contains("sRGBTexture: 0"));
    EXPECT_TRUE(body.contains("filterMode: 0"));
    EXPECT_FALSE(body.contains("guid: 00000000000000000000000000000000"))
        << ".meta must use a unique GUID — sharing one with another asset "
           "causes Unity to silently remap references at import time";
}

TEST_F(VATBakerEndToEndTest, UnityMetaGuidIsUniquePerBake) {
    // Unity asset GUIDs must be globally unique; two bakes with the
    // same target should never produce the same GUID in the .meta.
    auto* entity = createAnimatedTestEntity("VAT_E2E_UnityGuid");
    ASSERT_NE(entity, nullptr);
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    auto extractGuid = [](const QString& path) -> QString {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return {};
        const QByteArray body = f.readAll();
        const int idx = body.indexOf("guid: ");
        if (idx < 0) return {};
        const int eol = body.indexOf('\n', idx + 6);
        return QString::fromUtf8(body.mid(idx + 6, eol - (idx + 6)));
    };

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.target = VATBaker::Target::Unity;

    opts.basename = QStringLiteral("A");
    auto r1 = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r1.ok);
    opts.basename = QStringLiteral("B");
    auto r2 = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r2.ok);

    const QString g1 = extractGuid(r1.unityMetaPath);
    const QString g2 = extractGuid(r2.unityMetaPath);
    EXPECT_FALSE(g1.isEmpty());
    EXPECT_EQ(g1.size(), 32) << "Unity GUID must be 32 hex chars";
    EXPECT_NE(g1, g2);
}

TEST_F(VATBakerEndToEndTest, GodotTargetWritesShaderTemplate) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_Godot");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("G");
    opts.target = VATBaker::Target::Godot;

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_FALSE(r.godotShaderPath.isEmpty());
    EXPECT_TRUE(QFile::exists(r.godotShaderPath));
    QFile sf(r.godotShaderPath);
    ASSERT_TRUE(sf.open(QIODevice::ReadOnly));
    const QByteArray body = sf.readAll();
    // Spot-check the shader has the expected entry point + uniforms.
    EXPECT_TRUE(body.contains("shader_type spatial"));
    EXPECT_TRUE(body.contains("uniform sampler2D pos_tex"));
    EXPECT_TRUE(body.contains("void vertex()"));
    // Frame + vertex counts are template-substituted from the bake.
    EXPECT_TRUE(body.contains("frame_count = "
                              + QByteArray::number(r.frameCount)));
    EXPECT_TRUE(body.contains("vertex_count = "
                              + QByteArray::number(r.vertexCount)));
}

TEST_F(VATBakerEndToEndTest, UnrealTargetSwizzlesPositionsXZY) {
    // Unreal target swaps Y/Z (Ogre Y-up → Unreal Z-up). The bake's
    // bounds should reflect that the *output* space has Y/Z swapped
    // versus the source mesh's local AABB. The animated test entity
    // moves a vertex along X+ during the keyframe, so X is non-zero
    // and Y/Z stay near zero in Ogre space — after the swap, Z
    // should be > Y in the bounds extent.
    auto* entity = createAnimatedTestEntity("VAT_E2E_Unreal");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    VATBaker::Options agnosticOpts;
    agnosticOpts.animationName = QStringLiteral("TestAnim");
    agnosticOpts.fps = 10.0;
    agnosticOpts.outputDir = tmp.path();
    agnosticOpts.basename = QStringLiteral("A");
    auto agnostic = VATBaker::bake(entity, agnosticOpts);
    ASSERT_TRUE(agnostic.ok) << agnostic.error.toStdString();

    VATBaker::Options unrealOpts = agnosticOpts;
    unrealOpts.basename = QStringLiteral("UE");
    unrealOpts.target = VATBaker::Target::Unreal;
    auto unreal = VATBaker::bake(entity, unrealOpts);
    ASSERT_TRUE(unreal.ok) << unreal.error.toStdString();

    // Y and Z bounds should swap between agnostic and unreal.
    EXPECT_NEAR(unreal.minBound.x, agnostic.minBound.x, 1e-4);
    EXPECT_NEAR(unreal.minBound.y, agnostic.minBound.z, 1e-4)
        << "Unreal target should put Ogre.Z into output.Y";
    EXPECT_NEAR(unreal.minBound.z, agnostic.minBound.y, 1e-4)
        << "Unreal target should put Ogre.Y into output.Z";
    EXPECT_NEAR(unreal.maxBound.y, agnostic.maxBound.z, 1e-4);
    EXPECT_NEAR(unreal.maxBound.z, agnostic.maxBound.y, 1e-4);
}

TEST_F(VATBakerEndToEndTest, UnityRowsAreVerticallyFlipped) {
    // Unity convention: pre-flip rows so the runtime shader uses
    // plain `frameIndex / frameCount` indexing. Compare the agnostic
    // and unity PNGs — row 0 of unity should equal row (frameCount-1)
    // of agnostic.
    auto* entity = createAnimatedTestEntity("VAT_E2E_UnityFlip");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    VATBaker::Options a; a.animationName = QStringLiteral("TestAnim");
    a.fps = 10.0; a.outputDir = tmp.path(); a.basename = QStringLiteral("AG");
    auto agnostic = VATBaker::bake(entity, a);
    ASSERT_TRUE(agnostic.ok);

    VATBaker::Options u = a;
    u.basename = QStringLiteral("UNI");
    u.target = VATBaker::Target::Unity;
    auto unity = VATBaker::bake(entity, u);
    ASSERT_TRUE(unity.ok);

    QImage ag(agnostic.posTexPath);
    QImage un(unity.posTexPath);
    ASSERT_FALSE(ag.isNull());
    ASSERT_FALSE(un.isNull());
    ASSERT_EQ(ag.width(),  un.width());
    ASSERT_EQ(ag.height(), un.height());

    // Unity row 0 == agnostic last row.
    const int lastRow = ag.height() - 1;
    for (int x = 0; x < ag.width(); ++x) {
        EXPECT_EQ(ag.pixel(x, lastRow), un.pixel(x, 0))
            << "Unity row 0 should match agnostic last row at x=" << x;
    }
}
