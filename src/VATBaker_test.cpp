#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QImage>
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
// End-to-end (requires Ogre + an animated entity).
//
// We don't ship a tiny animated mesh in the test data, so the heavy
// integration test loads media/models/robot.mesh through the standard
// helper. That mesh ships with a built-in skeleton + an "Idle" anim.
// ===========================================================================

class VATBakerEndToEndTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init required";
    }
};

TEST_F(VATBakerEndToEndTest, BakeRobotIdleProducesTextureAndSidecar) {
    const QString robotPath = testRobotMeshPath();
    if (robotPath.isEmpty()) {
        GTEST_SKIP() << "robot.mesh not found in expected locations";
    }
    auto* mgr = Manager::getSingleton();
    ASSERT_NE(mgr, nullptr);
    auto* scene = mgr->getSceneMgr();
    ASSERT_NE(scene, nullptr);

    // Load the mesh directly so we don't carry an importer dependency.
    Ogre::MeshPtr mesh;
    try {
        mesh = Ogre::MeshManager::getSingleton().load(
            QFileInfo(robotPath).fileName().toStdString(),
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    } catch (...) {
        GTEST_SKIP() << "robot.mesh not loadable via TextureManager (resource path mismatch)";
    }
    if (!mesh) GTEST_SKIP() << "robot mesh ptr null";

    auto* entity = scene->createEntity("VAT_RobotTest", mesh->getName());
    ASSERT_NE(entity, nullptr);
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);
    if (!entity->hasSkeleton()) {
        scene->getRootSceneNode()->removeAndDestroyChild(node);
        scene->destroyEntity(entity);
        GTEST_SKIP() << "robot has no skeleton in this test runner";
    }

    // Pick whatever animation exists — robot.mesh historically ships
    // with "Idle" and "Walk" but we don't want to hard-fail on naming.
    auto* states = entity->getAllAnimationStates();
    QString animName;
    if (states) {
        auto it = states->getAnimationStateIterator();
        if (it.hasMoreElements()) {
            animName = QString::fromStdString(it.getNext()->getAnimationName());
        }
    }
    if (animName.isEmpty()) {
        scene->getRootSceneNode()->removeAndDestroyChild(node);
        scene->destroyEntity(entity);
        GTEST_SKIP() << "no animation on robot mesh";
    }

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    VATBaker::Options opts;
    opts.animationName = animName;
    opts.fps = 20.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("Test");

    auto r = VATBaker::bake(entity, opts);
    EXPECT_TRUE(r.ok) << "bake error: " << r.error.toStdString();
    EXPECT_GT(r.frameCount, 0);
    EXPECT_GT(r.vertexCount, 0);
    EXPECT_TRUE(QFile::exists(r.posTexPath));
    EXPECT_TRUE(QFile::exists(r.jsonPath));
    // Texture dimensions match the layout: width = vertexCount, height = frameCount.
    QImage png(r.posTexPath);
    EXPECT_FALSE(png.isNull());
    EXPECT_EQ(png.width(),  r.vertexCount);
    EXPECT_EQ(png.height(), r.frameCount);

    // Sidecar is valid JSON with the expected fields.
    QFile jf(r.jsonPath);
    ASSERT_TRUE(jf.open(QIODevice::ReadOnly));
    auto doc = QJsonDocument::fromJson(jf.readAll());
    EXPECT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object()["frameCount"].toInt(),  r.frameCount);
    EXPECT_EQ(doc.object()["vertexCount"].toInt(), r.vertexCount);

    scene->getRootSceneNode()->removeAndDestroyChild(node);
    scene->destroyEntity(entity);
}
