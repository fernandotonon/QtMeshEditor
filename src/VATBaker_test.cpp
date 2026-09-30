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

// ===========================================================================
// #522 — the VAT mode family (skeletal / rigid / mesh-anim / morph),
// encodings and targets.
// ===========================================================================

#include "VertexAnimationManager.h"

#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreHardwareBufferManager.h>
#include <OgreKeyFrame.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgrePose.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSkeleton.h>
#include <OgreSkeletonManager.h>
#include <OgreSubMesh.h>

TEST(VATBakerStandalone, ModeIdsRoundTrip) {
    for (const QString& id : VATBaker::modeIds()) {
        VATBaker::Mode m;
        ASSERT_TRUE(VATBaker::modeFromId(id, &m)) << id.toStdString();
        EXPECT_EQ(VATBaker::modeId(m), id);
    }
    VATBaker::Mode m = VATBaker::Mode::Morph;
    EXPECT_FALSE(VATBaker::modeFromId(QStringLiteral("bogus"), &m));
    EXPECT_EQ(m, VATBaker::Mode::Morph) << "an unknown id must leave *out untouched";
    // Aliases + case-insensitivity.
    EXPECT_TRUE(VATBaker::modeFromId(QStringLiteral("MESH_ANIM"), &m));
    EXPECT_EQ(m, VATBaker::Mode::MeshAnim);
    EXPECT_TRUE(VATBaker::modeFromId(QStringLiteral("rbd"), &m));
    EXPECT_EQ(m, VATBaker::Mode::Rigid);
    EXPECT_EQ(VATBaker::modeIds().size(), 4);
}

TEST(VATBakerStandalone, EncodingIdsMapToBitDepths) {
    int bd = 0;
    EXPECT_TRUE(VATBaker::bitDepthFromEncodingId(QStringLiteral("rgba8"), &bd));  EXPECT_EQ(bd, 8);
    EXPECT_TRUE(VATBaker::bitDepthFromEncodingId(QStringLiteral("rgba16"), &bd)); EXPECT_EQ(bd, 16);
    EXPECT_TRUE(VATBaker::bitDepthFromEncodingId(QStringLiteral("EXR"), &bd));    EXPECT_EQ(bd, 32);
    EXPECT_FALSE(VATBaker::bitDepthFromEncodingId(QStringLiteral("jpeg"), &bd));
    EXPECT_EQ(bd, 32) << "unknown encoding must leave *out untouched";
    EXPECT_EQ(VATBaker::encodingId(8),  QStringLiteral("rgba8"));
    EXPECT_EQ(VATBaker::encodingId(16), QStringLiteral("rgba16"));
    EXPECT_EQ(VATBaker::encodingId(32), QStringLiteral("exr"));
}

TEST(VATBakerStandalone, TargetIdsValidated) {
    for (const QString& t : VATBaker::targetIds())
        EXPECT_TRUE(VATBaker::isValidTargetId(t)) << t.toStdString();
    EXPECT_TRUE(VATBaker::isValidTargetId(QString())) << "empty == agnostic";
    EXPECT_TRUE(VATBaker::isValidTargetId(QStringLiteral("Godot")));
    EXPECT_FALSE(VATBaker::isValidTargetId(QStringLiteral("blender")));
}

TEST(VATBakerStandalone, SidecarCarriesModeTargetAndRigidExtras) {
    VATBaker::BakeResult r;
    r.mode = VATBaker::Mode::Rigid;
    r.frameCount = 12;
    r.vertexCount = 2;
    r.chunkCount = 2;
    r.minBound = Ogre::Vector3(-1.0f, 0.0f, -1.0f);
    r.maxBound = Ogre::Vector3( 1.0f, 2.0f,  1.0f);
    VATBaker::RigidChunk a; a.name = QStringLiteral("body"); a.pivot = Ogre::Vector3(0, 1, 0);
    a.vertexStart = 0; a.vertexCount = 8; a.maxResidual = 0.001f;
    VATBaker::RigidChunk b; b.name = QStringLiteral("wheel"); b.pivot = Ogre::Vector3(1, 0, 0);
    b.vertexStart = 8; b.vertexCount = 16; b.maxResidual = 0.002f;
    r.chunks = { a, b };
    r.maxRigidResidual = 0.002f;

    VATBaker::Options opts;
    opts.animationName = QStringLiteral("Explode");
    opts.mode = VATBaker::Mode::Rigid;
    opts.bitDepth = 8;
    opts.target = QStringLiteral("godot");

    const auto root = QJsonDocument::fromJson(
        VATBaker::buildSidecarJson(r, opts).toUtf8()).object();
    ASSERT_TRUE(root.contains("os-remap")) << "the OpenVAT core shape must survive every mode";
    EXPECT_EQ(root["os-remap"].toObject()["Frames"].toInt(), 12);
    EXPECT_EQ(root["_mode"].toString(), QStringLiteral("rigid"));
    EXPECT_EQ(root["_target"].toString(), QStringLiteral("godot"));
    EXPECT_EQ(root["_bit_depth"].toInt(), 8);
    ASSERT_TRUE(root.contains("_rigid"));
    const auto rigid = root["_rigid"].toObject();
    EXPECT_EQ(rigid["chunk_count"].toInt(), 2);
    const auto chunks = rigid["chunks"].toArray();
    ASSERT_EQ(chunks.size(), 2);
    EXPECT_EQ(chunks[1].toObject()["name"].toString(), QStringLiteral("wheel"));
    EXPECT_EQ(chunks[1].toObject()["vertex_start"].toInt(), 8);
    EXPECT_DOUBLE_EQ(chunks[1].toObject()["pivot"].toArray()[0].toDouble(), 1.0);
    EXPECT_FALSE(root.contains("_morph_targets"));
}

TEST(VATBakerStandalone, SidecarDefaultsToSkeletalAgnostic) {
    VATBaker::BakeResult r;
    r.frameCount = 3; r.vertexCount = 3;
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("Walk");
    const auto root = QJsonDocument::fromJson(
        VATBaker::buildSidecarJson(r, opts).toUtf8()).object();
    EXPECT_EQ(root["_mode"].toString(), QStringLiteral("skeletal"));
    EXPECT_EQ(root["_target"].toString(), QStringLiteral("agnostic"));
    EXPECT_EQ(root["_bit_depth"].toInt(), 16);
    EXPECT_FALSE(root.contains("_rigid"));
    EXPECT_FALSE(root.contains("_track"));
}

TEST(VATBakerStandalone, RejectsUnknownTargetAndBitDepthBeforeTouchingTheEntity) {
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("Walk");
    opts.outputDir = QStringLiteral("/tmp");
    opts.target = QStringLiteral("blender");
    // A null entity is rejected first; the point is that these guards
    // exist on the pure path at all — exercised with a live entity below.
    auto r = VATBaker::bake(nullptr, opts);
    EXPECT_FALSE(r.ok);
}

namespace {

// Static (no skeleton) 3-vertex mesh with a morph target "Smile" that
// lifts vertex 0 by +1 in Y, and a "MorphAnim" weight clip ramping the
// target 0 → 1 over one second. Mirrors what MorphCommands +
// MorphAnimationManager::writeWeightKeyOn build in the editor.
Ogre::Entity* createMorphTestEntity(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name + "_mesh", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float verts[] = {
        0,0,0,  0,0,1,
        1,0,0,  0,0,1,
        0,1,0,  0,0,1,
    };
    vbuf->writeData(0, sizeof(verts), verts);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 3;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;

    // Pose on the shared geometry (handle 0).
    Ogre::Pose* pose = mesh->createPose(0, "Smile");
    pose->addVertex(0, Ogre::Vector3(0, 1, 0));

    auto* anim = mesh->createAnimation("MorphAnim", 1.0f);
    auto* track = anim->createVertexTrack(0, Ogre::VAT_POSE);
    track->createVertexPoseKeyFrame(0.0f)->addPoseReference(0, 0.0f);
    track->createVertexPoseKeyFrame(1.0f)->addPoseReference(0, 1.0f);

    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,2,2,2));
    mesh->_setBoundingSphereRadius(3.0);
    mesh->load();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode(name.c_str());
    auto* entity = sceneMgr->createEntity(name, mesh);
    node->attachObject(entity);
    return entity;
}

// Static 3-vertex mesh with a dense vertex-cache clip ("Wobble") built
// through VertexAnimationManager::buildClipFromFrames — the exact path
// the Alembic importer feeds. Every vertex moves +1 in X over 1 s.
Ogre::Entity* createMeshAnimTestEntity(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name + "_mesh", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float verts[] = {
        0,0,0,  0,0,1,
        1,0,0,  0,0,1,
        0,1,0,  0,0,1,
    };
    vbuf->writeData(0, sizeof(verts), verts);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 3;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;

    VertexAnimationManager::FrameSet fs;
    fs.vertexCount = 3;
    fs.fps = 10;
    for (int f = 0; f < 3; ++f) {
        VertexAnimationManager::FrameData fd;
        fd.time = 0.5f * f;
        const float dx = 0.5f * f;
        fd.positions = { 0 + dx, 0, 0,   1 + dx, 0, 0,   0 + dx, 1, 0 };
        fs.frames.push_back(fd);
    }
    if (!VertexAnimationManager::buildClipFromFrames(mesh.get(), QStringLiteral("Wobble"), fs))
        return nullptr;

    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,3,2,2));
    mesh->_setBoundingSphereRadius(4.0);
    mesh->load();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode(name.c_str());
    auto* entity = sceneMgr->createEntity(name, mesh);
    node->attachObject(entity);
    return entity;
}

// Two rigid pieces: submesh 0 (4 verts, bound 100 % to the static Root
// bone) and submesh 1 (4 verts, bound 100 % to the Child bone), each
// owning its vertex data. "Swing" moves the Child bone by +0.5 X and
// 30° about Y at t = 0.5 s and back at t = 1 s — so piece 1 is an
// exactly rigid motion the Horn fit must recover with ~zero residual.
Ogre::Entity* createRigidTestEntity(const std::string& name)
{
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        name + "_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* rootBone = skel->createBone("Root", 0);
    rootBone->setPosition(Ogre::Vector3(0, 0, 0));
    auto* childBone = skel->createBone("Child", 1);
    childBone->setPosition(Ogre::Vector3(0, 1, 0));
    rootBone->addChild(childBone);
    skel->setBindingPose();

    auto* anim = skel->createAnimation("Swing", 1.0f);
    auto* track = anim->createNodeTrack(1);
    track->setAssociatedNode(childBone);
    auto* kf0 = track->createNodeKeyFrame(0.0f);
    kf0->setTranslate(Ogre::Vector3::ZERO);
    kf0->setRotation(Ogre::Quaternion::IDENTITY);
    kf0->setScale(Ogre::Vector3::UNIT_SCALE);
    auto* kf1 = track->createNodeKeyFrame(0.5f);
    kf1->setTranslate(Ogre::Vector3(0.5f, 0, 0));
    kf1->setRotation(Ogre::Quaternion(Ogre::Radian(Ogre::Degree(30)), Ogre::Vector3::UNIT_Y));
    kf1->setScale(Ogre::Vector3::UNIT_SCALE);
    auto* kf2 = track->createNodeKeyFrame(1.0f);
    kf2->setTranslate(Ogre::Vector3::ZERO);
    kf2->setRotation(Ogre::Quaternion::IDENTITY);
    kf2->setScale(Ogre::Vector3::UNIT_SCALE);

    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name + "_mesh", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto makePiece = [&](const char* pieceName, float baseX, float baseY, unsigned short bone) {
        Ogre::SubMesh* sub = mesh->createSubMesh(pieceName);
        sub->useSharedVertices = false;
        sub->vertexData = new Ogre::VertexData();
        auto* decl = sub->vertexData->vertexDeclaration;
        size_t offset = 0;
        decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
        offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
        decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
        auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            decl->getVertexSize(0), 4, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        // A non-coplanar tetrahedron so the rotation is fully determined.
        float verts[] = {
            baseX + 0, baseY + 0, 0,      0,0,1,
            baseX + 1, baseY + 0, 0,      0,0,1,
            baseX + 0, baseY + 1, 0,      0,0,1,
            baseX + 0, baseY + 0, 1,      0,0,1,
        };
        vbuf->writeData(0, sizeof(verts), verts);
        sub->vertexData->vertexBufferBinding->setBinding(0, vbuf);
        sub->vertexData->vertexCount = 4;
        auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            Ogre::HardwareIndexBuffer::IT_16BIT, 12, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        uint16_t idx[] = {0,1,2, 0,1,3, 0,2,3, 1,2,3};
        ibuf->writeData(0, sizeof(idx), idx);
        sub->indexData->indexBuffer = ibuf;
        sub->indexData->indexCount = 12;
        Ogre::VertexBoneAssignment vba;
        vba.boneIndex = bone;
        vba.weight = 1.0f;
        for (unsigned short v = 0; v < 4; ++v) {
            vba.vertexIndex = v;
            sub->addBoneAssignment(vba);
        }
    };
    makePiece("base", 0.0f, 0.0f, 0);
    makePiece("arm",  0.0f, 1.0f, 1);

    mesh->_notifySkeleton(skel);
    mesh->_setBounds(Ogre::AxisAlignedBox(-2,-2,-2,3,3,3));
    mesh->_setBoundingSphereRadius(5.0);
    mesh->load();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode(name.c_str());
    auto* entity = sceneMgr->createEntity(name, mesh);
    node->attachObject(entity);
    return entity;
}

// Decode the 16-bit position texel for (column, frameRow) against the
// sidecar bounds — what a consumer shader does.
Ogre::Vector3 decodePos16(const QImage& img, int col, int row,
                          const Ogre::Vector3& lo, const Ogre::Vector3& hi)
{
    const auto* px = reinterpret_cast<const uint16_t*>(img.constScanLine(row)) + col * 4;
    auto d = [&](int c, float l, float h) { return l + (px[c] / 65535.0f) * (h - l); };
    return Ogre::Vector3(d(0, lo.x, hi.x), d(1, lo.y, hi.y), d(2, lo.z, hi.z));
}

} // namespace

TEST_F(VATBakerEndToEndTest, SkeletalSidecarTagsModeAndKeepsLegacyOutputShape) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_Mode_Skel");
    ASSERT_NE(entity, nullptr);
    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("Legacy");
    // Default mode == Skeletal: bit-identical files to the pre-#522 baker.
    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.mode, VATBaker::Mode::Skeletal);
    EXPECT_TRUE(r.posTexPath.endsWith(QStringLiteral("Legacy_pos.png")));
    QFile jf(r.jsonPath); ASSERT_TRUE(jf.open(QIODevice::ReadOnly));
    const auto root = QJsonDocument::fromJson(jf.readAll()).object();
    EXPECT_EQ(root["_mode"].toString(), QStringLiteral("skeletal"));
    EXPECT_EQ(root["_target"].toString(), QStringLiteral("agnostic"));
    EXPECT_TRUE(root.contains("os-remap"));
}

TEST_F(VATBakerEndToEndTest, Rgba8EncodingWritesEightBitPng) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_Rgba8");
    ASSERT_NE(entity, nullptr);
    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("Eight");
    opts.bitDepth = 8;
    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    QImage img(r.posTexPath);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.depth(), 32) << "rgba8 must be an 8-bit-per-channel PNG";
    EXPECT_EQ(img.width(), r.vertexCount);
    EXPECT_EQ(img.height(), r.frameCount * 2);
    QFile jf(r.jsonPath); ASSERT_TRUE(jf.open(QIODevice::ReadOnly));
    EXPECT_EQ(QJsonDocument::fromJson(jf.readAll()).object()["_bit_depth"].toInt(), 8);
}

TEST_F(VATBakerEndToEndTest, SkeletalModeRefusesVertexOnlyEntity) {
    auto* entity = createMorphTestEntity("VAT_E2E_SkelOnMorph");
    ASSERT_NE(entity, nullptr);
    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("MorphAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    auto r = VATBaker::bake(entity, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("skeleton"))) << r.error.toStdString();
}

TEST_F(VATBakerEndToEndTest, MorphModeBakesWeightClipToVertexPositions) {
    auto* entity = createMorphTestEntity("VAT_E2E_Morph");
    ASSERT_NE(entity, nullptr);
    ASSERT_FALSE(entity->hasSkeleton());
    ASSERT_TRUE(entity->hasVertexAnimation());

    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.mode = VATBaker::Mode::Morph;
    opts.animationName = QStringLiteral("MorphAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("Face");
    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.vertexCount, 3);
    ASSERT_GE(r.frameCount, 2);
    ASSERT_EQ(r.morphTargets, QStringList{QStringLiteral("Smile")});
    EXPECT_EQ(r.trackId, QStringLiteral("MorphAnim"));

    QImage img(r.posTexPath);
    ASSERT_FALSE(img.isNull());
    ASSERT_EQ(img.width(), 3);
    ASSERT_EQ(img.height(), r.frameCount * 2);
    // Vertex 0 starts at the origin and ends lifted to y = 1 — the
    // "Smile" delta at full weight. Vertex 1 never moves.
    const Ogre::Vector3 v0First = decodePos16(img, 0, 0, r.minBound, r.maxBound);
    const Ogre::Vector3 v0Last  = decodePos16(img, 0, r.frameCount - 1, r.minBound, r.maxBound);
    const Ogre::Vector3 v1First = decodePos16(img, 1, 0, r.minBound, r.maxBound);
    const Ogre::Vector3 v1Last  = decodePos16(img, 1, r.frameCount - 1, r.minBound, r.maxBound);
    EXPECT_NEAR(v0First.y, 0.0f, 0.01f);
    EXPECT_NEAR(v0Last.y,  1.0f, 0.01f) << "morph weight 1.0 must land the pose delta";
    EXPECT_NEAR((v1Last - v1First).length(), 0.0f, 0.01f);

    QFile jf(r.jsonPath); ASSERT_TRUE(jf.open(QIODevice::ReadOnly));
    const auto root = QJsonDocument::fromJson(jf.readAll()).object();
    EXPECT_EQ(root["_mode"].toString(), QStringLiteral("morph"));
    EXPECT_EQ(root["_morph_targets"].toArray()[0].toString(), QStringLiteral("Smile"));
}

TEST_F(VATBakerEndToEndTest, MorphModeRefusesMeshWithoutPoses) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_MorphNoPose");
    ASSERT_NE(entity, nullptr);
    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.mode = VATBaker::Mode::Morph;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    auto r = VATBaker::bake(entity, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("morph targets"))) << r.error.toStdString();
}

TEST_F(VATBakerEndToEndTest, MeshAnimModeBakesVertexCacheClip) {
    auto* entity = createMeshAnimTestEntity("VAT_E2E_MeshAnim");
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasVertexAnimation());

    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.mode = VATBaker::Mode::MeshAnim;
    opts.animationName = QStringLiteral("Wobble");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("Cache");
    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.vertexCount, 3);
    EXPECT_EQ(r.trackId, QStringLiteral("Wobble"));
    ASSERT_GE(r.frameCount, 2);

    QImage img(r.posTexPath);
    ASSERT_FALSE(img.isNull());
    // Every vertex slides +1 in X across the clip.
    for (int col = 0; col < 3; ++col) {
        const Ogre::Vector3 first = decodePos16(img, col, 0, r.minBound, r.maxBound);
        const Ogre::Vector3 last  = decodePos16(img, col, r.frameCount - 1, r.minBound, r.maxBound);
        EXPECT_NEAR(last.x - first.x, 1.0f, 0.02f) << "column " << col;
        EXPECT_NEAR(last.y - first.y, 0.0f, 0.02f) << "column " << col;
    }
    QFile jf(r.jsonPath); ASSERT_TRUE(jf.open(QIODevice::ReadOnly));
    const auto root = QJsonDocument::fromJson(jf.readAll()).object();
    EXPECT_EQ(root["_mode"].toString(), QStringLiteral("mesh-anim"));
    EXPECT_EQ(root["_track"].toString(), QStringLiteral("Wobble"));
}

TEST_F(VATBakerEndToEndTest, MeshAnimModeRefusesSkeletalClip) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_MeshAnimOnSkel");
    ASSERT_NE(entity, nullptr);
    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.mode = VATBaker::Mode::MeshAnim;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    auto r = VATBaker::bake(entity, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("vertex"))) << r.error.toStdString();
}

TEST_F(VATBakerEndToEndTest, RigidModeRecoversPerChunkTransforms) {
    auto* entity = createRigidTestEntity("VAT_E2E_Rigid");
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());

    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.mode = VATBaker::Mode::Rigid;
    opts.animationName = QStringLiteral("Swing");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("Pieces");
    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.mode, VATBaker::Mode::Rigid);
    ASSERT_EQ(r.chunkCount, 2);
    ASSERT_EQ(r.chunks.size(), 2u);
    EXPECT_EQ(r.vertexCount, 2) << "rigid texture width == chunk count";
    EXPECT_EQ(r.chunks[0].name, QStringLiteral("base"));
    EXPECT_EQ(r.chunks[1].name, QStringLiteral("arm"));
    EXPECT_EQ(r.chunks[0].vertexStart, 0);
    EXPECT_EQ(r.chunks[1].vertexStart, 4);
    EXPECT_EQ(r.chunks[1].vertexCount, 4);
    // Each piece is bound 100 % to one bone, so the motion is exactly
    // rigid and the Horn fit must reproduce every vertex.
    EXPECT_LT(r.chunks[0].maxResidual, 1e-3f);
    EXPECT_LT(r.chunks[1].maxResidual, 1e-3f) << "arm chunk is not fitted rigidly";
    EXPECT_LT(r.maxRigidResidual, 1e-3f);
    // Bind pivot = centroid of the piece's tetrahedron.
    EXPECT_NEAR(r.chunks[1].pivot.x, 0.25f, 1e-4f);
    EXPECT_NEAR(r.chunks[1].pivot.y, 1.25f, 1e-4f);

    QImage img(r.posTexPath);
    ASSERT_FALSE(img.isNull());
    EXPECT_EQ(img.width(), 2);
    EXPECT_EQ(img.height(), r.frameCount * 2);
    ASSERT_GE(r.frameCount, 5);
    // The static base chunk's quaternion stays identity on every frame
    // ((q+1)/2 → x,y,z = 0.5, w = 1.0), the arm chunk's does not mid-clip.
    auto quatAt = [&](int col, int f) {
        const auto* px = reinterpret_cast<const uint16_t*>(img.constScanLine(r.frameCount + f)) + col * 4;
        return Ogre::Quaternion(px[3] / 65535.0f * 2 - 1, px[0] / 65535.0f * 2 - 1,
                                px[1] / 65535.0f * 2 - 1, px[2] / 65535.0f * 2 - 1);
    };
    const int mid = r.frameCount / 2;
    const Ogre::Quaternion baseMid = quatAt(0, mid);
    EXPECT_NEAR(std::abs(baseMid.w), 1.0f, 2e-3f);
    const Ogre::Quaternion armMid = quatAt(1, mid);
    EXPECT_LT(std::abs(armMid.w), 0.999f) << "arm must be rotated mid-clip";
    EXPECT_GT(std::abs(armMid.y), 0.05f) << "the swing is about Y";
    // Pivot position of the arm moves in +X mid-clip.
    const Ogre::Vector3 armP0 = decodePos16(img, 1, 0, r.minBound, r.maxBound);
    const Ogre::Vector3 armPm = decodePos16(img, 1, mid, r.minBound, r.maxBound);
    EXPECT_GT(armPm.x - armP0.x, 0.1f);

    QFile jf(r.jsonPath); ASSERT_TRUE(jf.open(QIODevice::ReadOnly));
    const auto root = QJsonDocument::fromJson(jf.readAll()).object();
    EXPECT_EQ(root["_mode"].toString(), QStringLiteral("rigid"));
    EXPECT_EQ(root["_rigid"].toObject()["chunk_count"].toInt(), 2);
    EXPECT_EQ(root["_rigid"].toObject()["chunks"].toArray()[1].toObject()["name"].toString(),
              QStringLiteral("arm"));
}

TEST_F(VATBakerEndToEndTest, RigidModeWritesRgbaExrWhenRequested) {
    auto* entity = createRigidTestEntity("VAT_E2E_RigidExr");
    ASSERT_NE(entity, nullptr);
    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.mode = VATBaker::Mode::Rigid;
    opts.animationName = QStringLiteral("Swing");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("PiecesExr");
    opts.bitDepth = 32;
    auto r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_TRUE(r.posTexPath.endsWith(QStringLiteral("_pos.exr")));
    EXPECT_TRUE(QFile::exists(r.posTexPath));
}

TEST_F(VATBakerEndToEndTest, RigidModeRefusesSharedVertexData) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_RigidShared");
    ASSERT_NE(entity, nullptr);
    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.mode = VATBaker::Mode::Rigid;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    auto r = VATBaker::bake(entity, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("shared"))) << r.error.toStdString();
}

TEST_F(VATBakerEndToEndTest, LiveEntityRejectsUnknownTarget) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_BadTarget");
    ASSERT_NE(entity, nullptr);
    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("TestAnim");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.target = QStringLiteral("blender");
    auto r = VATBaker::bake(entity, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("target"))) << r.error.toStdString();
    EXPECT_FALSE(QFile::exists(QDir(tmp.path()).filePath(QStringLiteral("TestAnim_pos.png"))))
        << "nothing must be written on a refused bake";
}

// A rigged entity that ALSO carries a vertex clip: skeletal mode must
// refuse the vertex clip's name (review finding — the state lookup alone
// would have accepted it and mislabelled the bake), and mesh-anim mode
// must bake it with the skeleton left in bind pose.
TEST_F(VATBakerEndToEndTest, SkeletalModeRefusesVertexClipOnRiggedEntity) {
    auto* entity = createAnimatedTestEntity("VAT_E2E_RiggedPlusVertex");
    ASSERT_NE(entity, nullptr);
    Ogre::MeshPtr mesh = entity->getMesh();
    VertexAnimationManager::FrameSet fs;
    fs.vertexCount = 3;
    fs.fps = 10;
    for (int f = 0; f < 3; ++f) {
        VertexAnimationManager::FrameData fd;
        fd.time = 0.5f * f;
        const float dz = 0.5f * f;
        fd.positions = { 0, 0, dz,   1, 0, dz,   0, 1, dz };
        fs.frames.push_back(fd);
    }
    ASSERT_TRUE(VertexAnimationManager::buildClipFromFrames(mesh.get(), QStringLiteral("VCache"), fs));
    entity->refreshAvailableAnimationState();
    ASSERT_TRUE(entity->getAllAnimationStates()->hasAnimationState("VCache"));

    QTemporaryDir tmp;
    VATBaker::Options opts;
    opts.animationName = QStringLiteral("VCache");
    opts.fps = 10.0;
    opts.outputDir = tmp.path();
    opts.basename = QStringLiteral("Mislabel");
    auto r = VATBaker::bake(entity, opts);          // default mode == Skeletal
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains(QStringLiteral("mesh-anim"))) << r.error.toStdString();

    opts.mode = VATBaker::Mode::MeshAnim;
    opts.basename = QStringLiteral("RiggedCache");
    r = VATBaker::bake(entity, opts);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.mode, VATBaker::Mode::MeshAnim);
    QImage img(r.posTexPath);
    ASSERT_FALSE(img.isNull());
    // Every vertex slides +1 in Z across the clip; the skeleton (bind
    // pose) adds nothing.
    for (int col = 0; col < 3; ++col) {
        const Ogre::Vector3 first = decodePos16(img, col, 0, r.minBound, r.maxBound);
        const Ogre::Vector3 last  = decodePos16(img, col, r.frameCount - 1, r.minBound, r.maxBound);
        EXPECT_NEAR(last.z - first.z, 1.0f, 0.02f) << "column " << col;
        EXPECT_NEAR(last.x - first.x, 0.0f, 0.02f) << "column " << col;
    }
}
