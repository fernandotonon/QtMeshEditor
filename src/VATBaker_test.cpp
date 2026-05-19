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

// ===========================================================================
// Validation guards on VATBaker::bake() — exercised without an Ogre scene
// so they run on every CI permutation.
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
    VATBaker::Options opts;
    opts.outputDir = QStringLiteral("/tmp/vat");
    auto r = VATBaker::bake(nullptr, opts);
    EXPECT_FALSE(r.ok);
}

TEST(VATBakerStandalone, BakeInvalidFpsReports) {
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("Walk");
    opts.fps = 0.0;
    opts.outputDir = QStringLiteral("/tmp/vat");
    auto r = VATBaker::bake(nullptr, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

TEST(VATBakerStandalone, BuildSidecarJsonProducesOsRemap) {
    // Sanity-check the sidecar emitter without running a bake. Round
    // numbers so we can test the rounding-outward behavior cleanly.
    VATBaker::BakeResult r;
    r.frameCount  = 30;
    r.vertexCount = 5000;
    r.minBound = Ogre::Vector3(-1.23f, -2.34f, -3.45f);
    r.maxBound = Ogre::Vector3( 1.23f,  2.34f,  3.45f);
    r.posTexPath = QStringLiteral("/tmp/Walk_pos.png");

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("Walk");
    opts.fps           = 30.0;

    const QString json = VATBaker::buildSidecarJson(r, opts);
    auto doc = QJsonDocument::fromJson(json.toUtf8());
    ASSERT_TRUE(doc.isObject());
    const auto root = doc.object();
    ASSERT_TRUE(root.contains("os-remap"));
    const auto os = root["os-remap"].toObject();
    EXPECT_EQ(os["Frames"].toInt(), 30);
    ASSERT_TRUE(os["Min"].isArray());
    ASSERT_TRUE(os["Max"].isArray());

    const auto minArr = os["Min"].toArray();
    const auto maxArr = os["Max"].toArray();
    ASSERT_EQ(minArr.size(), 3);
    ASSERT_EQ(maxArr.size(), 3);
    ASSERT_TRUE(minArr[0].isString())
        << "OpenVAT shaders expect string-formatted Min[i]";

    // Rounding outward to nearest 0.1:
    //   min.x = -1.23 → floor(-12.3)/10 = -1.3
    //   max.x =  1.23 →  ceil( 12.3)/10 =  1.3
    EXPECT_FLOAT_EQ(minArr[0].toString().toFloat(), -1.3f);
    EXPECT_FLOAT_EQ(maxArr[0].toString().toFloat(),  1.3f);
}

// ===========================================================================
// End-to-end — needs Ogre + an animated entity.
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

    // Filename convention.
    EXPECT_TRUE(r.jsonPath.endsWith(QStringLiteral("-remap_info.json")))
        << "got: " << r.jsonPath.toStdString();

    // Texture is packed: height = 2 × frameCount.
    QImage png(r.posTexPath);
    ASSERT_FALSE(png.isNull());
    EXPECT_EQ(png.width(),  r.vertexCount);
    EXPECT_EQ(png.height(), r.frameCount * 2)
        << "OpenVAT texture must be 2× frame height (top=positions, bottom=normals)";
}

// Regression — VATBaker used to write bind-pose data into every row of
// the position texture because the bake loop didn't bump Ogre's per-
// frame counters between samples. This test catches a return of that
// bug by asserting that adjacent rows are NOT byte-identical.
TEST_F(VATBakerEndToEndTest, ProducesDistinctRowsAcrossFrames) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_FramesDiffer");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("FramesDiffer");

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_GE(r.frameCount, 2) << "need at least 2 frames to compare rows";

    QImage png(r.posTexPath);
    ASSERT_FALSE(png.isNull());
    ASSERT_EQ(png.height(), r.frameCount * 2);
    ASSERT_GT(png.width(), 0);

    // Scan the position half (rows 0 .. frameCount-1) for any adjacent
    // pair of rows that differs in any column.
    bool foundDifference = false;
    for (int row = 0; row + 1 < r.frameCount && !foundDifference; ++row) {
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

// The OpenVAT sidecar must match the canonical sharpen3d/openvat shape
// so off-the-shelf reference shaders consume it unmodified.
TEST_F(VATBakerEndToEndTest, OpenVATSidecarMatchesReferenceShape) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_OpenVAT_Side");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 30.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("OV");

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    QFile jf(r.jsonPath);
    ASSERT_TRUE(jf.open(QIODevice::ReadOnly));
    const auto doc = QJsonDocument::fromJson(jf.readAll());
    ASSERT_TRUE(doc.isObject());
    const auto root = doc.object();

    ASSERT_TRUE(root.contains("os-remap"));
    EXPECT_FALSE(root.contains("frameCount"))
        << "OpenVAT sidecar should not leak QtMeshEditor-specific keys";
    EXPECT_FALSE(root.contains("approximateBounds"));

    const auto osRemap = root["os-remap"].toObject();
    EXPECT_EQ(osRemap["Frames"].toInt(), r.frameCount);

    const auto minArr = osRemap["Min"].toArray();
    const auto maxArr = osRemap["Max"].toArray();
    ASSERT_EQ(minArr.size(), 3);
    ASSERT_EQ(maxArr.size(), 3);
    ASSERT_TRUE(minArr[0].isString());

    // 8 decimal places in stringified form.
    const QString minX = minArr[0].toString();
    const int dotPos = minX.indexOf(QChar('.'));
    ASSERT_GT(dotPos, -1) << "Min[0] should contain a decimal point: " << minX.toStdString();
    EXPECT_EQ(minX.size() - dotPos - 1, 8) << minX.toStdString();
}

TEST_F(VATBakerEndToEndTest, OpenVATBoundsRoundedOutwardToTenth) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_OpenVAT_Rnd");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 30.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("OVRnd");

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    QFile jf(r.jsonPath); ASSERT_TRUE(jf.open(QIODevice::ReadOnly));
    const auto root = QJsonDocument::fromJson(jf.readAll()).object();
    const auto osRemap = root["os-remap"].toObject();
    const auto minArr = osRemap["Min"].toArray();
    const auto maxArr = osRemap["Max"].toArray();

    for (int i = 0; i < 3; ++i) {
        const float actualMin = (i == 0) ? r.minBound.x
                              : (i == 1) ? r.minBound.y
                                         : r.minBound.z;
        const float actualMax = (i == 0) ? r.maxBound.x
                              : (i == 1) ? r.maxBound.y
                                         : r.maxBound.z;
        const float roundedMin = minArr[i].toString().toFloat();
        const float roundedMax = maxArr[i].toString().toFloat();
        EXPECT_LE(roundedMin, actualMin + 1e-6f)
            << "Min[" << i << "] (" << roundedMin
            << ") must be at-or-below actual bound (" << actualMin << ")";
        EXPECT_GE(roundedMax, actualMax - 1e-6f)
            << "Max[" << i << "] (" << roundedMax
            << ") must be at-or-above actual bound (" << actualMax << ")";
        EXPECT_LT(std::abs(roundedMin * 10.0f - std::round(roundedMin * 10.0f)), 1e-3f)
            << "Min[" << i << "] not on a 0.1 grid: " << roundedMin;
        EXPECT_LT(std::abs(roundedMax * 10.0f - std::round(roundedMax * 10.0f)), 1e-3f)
            << "Max[" << i << "] not on a 0.1 grid: " << roundedMax;
    }
}

TEST_F(VATBakerEndToEndTest, OpenVATTextureIs16BitRgb) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_OpenVAT_Px");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 30.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("OVPx");

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    // Qt promotes 16-bit PNG to RGBA64 / RGBX64; either is acceptable
    // (RGBX64 is the OpenVAT shape we ask Qt to write, RGBA64 is what
    // Qt may upgrade to depending on the platform decoder).
    QImage img(r.posTexPath);
    ASSERT_FALSE(img.isNull());
    const auto fmt = img.format();
    EXPECT_TRUE(fmt == QImage::Format_RGBX64 ||
                fmt == QImage::Format_RGBA64 ||
                fmt == QImage::Format_RGBA64_Premultiplied)
        << "expected 16-bit format, got " << static_cast<int>(fmt);
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
