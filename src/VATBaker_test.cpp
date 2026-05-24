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
#include <cstring>

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
    // BakeResult.minBound/maxBound are the already-rounded OpenVAT
    // bounds (snapped by bake() before encoding the texture and emitting
    // the sidecar — so the texture and JSON agree to the bit). Feed the
    // formatter rounded values and verify they survive verbatim.
    VATBaker::BakeResult r;
    r.frameCount  = 30;
    r.vertexCount = 5000;
    r.minBound = Ogre::Vector3(-1.3f, -2.4f, -3.5f);
    r.maxBound = Ogre::Vector3( 1.3f,  2.4f,  3.5f);
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

    // BakeResult bounds carry through verbatim — no second rounding.
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

    // Scan the position half (rows 0..frameCount-1) for any adjacent
    // pair of rows that differs in any byte. We compare raw scanline
    // bytes rather than QImage::pixel() — pixel() truncates 16-bit
    // RGBX64 channels to 8-bit QRgb, which could hide sub-byte motion
    // when bounds are wide. A bake that genuinely steps through the
    // animation will always produce row-to-row deltas in 16-bit space.
    const qsizetype stride = png.bytesPerLine();
    bool foundDifference = false;
    for (int row = 0; row + 1 < r.frameCount && !foundDifference; ++row) {
        const uchar* a = png.constScanLine(row);
        const uchar* b = png.constScanLine(row + 1);
        if (std::memcmp(a, b, static_cast<size_t>(stride)) != 0) {
            foundDifference = true;
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

    // The bit-depth extension field tells consumers whether to look
    // for `_pos.png` (16) or `_pos.exr` (32) next to this sidecar.
    ASSERT_TRUE(root.contains("_bit_depth"));
    EXPECT_EQ(root["_bit_depth"].toInt(), 16) << "default bake should be uint16";
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

    // The sidecar's Min/Max arrays MUST equal `r.minBound`/`r.maxBound`
    // exactly — that's the contract guaranteeing the texture (encoded
    // against `r.minBound`/`r.maxBound`) decodes correctly through the
    // sidecar that a consumer reads. Any drift between the two is a
    // P1 correctness bug.
    for (int i = 0; i < 3; ++i) {
        const float reportedMin = (i == 0) ? r.minBound.x
                                : (i == 1) ? r.minBound.y
                                           : r.minBound.z;
        const float reportedMax = (i == 0) ? r.maxBound.x
                                : (i == 1) ? r.maxBound.y
                                           : r.maxBound.z;
        const float sidecarMin = minArr[i].toString().toFloat();
        const float sidecarMax = maxArr[i].toString().toFloat();
        EXPECT_FLOAT_EQ(sidecarMin, reportedMin)
            << "sidecar Min[" << i << "] must equal BakeResult.minBound";
        EXPECT_FLOAT_EQ(sidecarMax, reportedMax)
            << "sidecar Max[" << i << "] must equal BakeResult.maxBound";
        // Both must land on a multiple of 0.1 (within FP).
        EXPECT_LT(std::abs(sidecarMin * 10.0f - std::round(sidecarMin * 10.0f)), 1e-3f)
            << "Min[" << i << "] not on a 0.1 grid: " << sidecarMin;
        EXPECT_LT(std::abs(sidecarMax * 10.0f - std::round(sidecarMax * 10.0f)), 1e-3f)
            << "Max[" << i << "] not on a 0.1 grid: " << sidecarMax;
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

TEST_F(VATBakerEndToEndTest, OpenVAT32BitWritesEXRAndTagsSidecar) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_OpenVAT_32");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 30.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("OV32");
    opts.bitDepth = 32;

    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    // 32-bit mode writes an EXR alongside the sidecar, NOT the PNG.
    EXPECT_TRUE(r.posTexPath.endsWith(QStringLiteral("_pos.exr")))
        << r.posTexPath.toStdString();
    EXPECT_TRUE(QFile::exists(r.posTexPath));

    // Sidecar must declare the bit depth so consumers know which
    // file to look for and how to interpret texel values (raw vs
    // remap-via-bounds).
    QFile jf(r.jsonPath);
    ASSERT_TRUE(jf.open(QIODevice::ReadOnly));
    const auto doc = QJsonDocument::fromJson(jf.readAll());
    ASSERT_TRUE(doc.isObject());
    EXPECT_EQ(doc.object()["_bit_depth"].toInt(), 32);
}

TEST_F(VATBakerEndToEndTest, RejectsMissingAnimationOnLiveEntity) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_MissAnim");
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("NotARealAnim");
    opts.fps = 30.0;
    opts.outputDir = tmp.path();

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
