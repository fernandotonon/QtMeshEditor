#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QJsonArray>

#include "UvUnwrap.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <OgreHardwareBufferManager.h>
#include <OgreHardwareIndexBuffer.h>
#include <OgreHardwareVertexBuffer.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>
#include <OgreSceneNode.h>
#include <OgreSceneManager.h>
#include <OgreEntity.h>

#include <vector>
#include <string>

// ── Helpers ──────────────────────────────────────────────────────────────────

// Build a procedurally-tessellated N×N plane mesh WITH a FLOAT2 UV0
// channel. Positions interleaved with UV0 in a single binding. The
// existing UvUnwrap_test only covers the no-UV plane (hasUv0==false),
// so this fixture is what drives infoForEntity's uv0Coverage compute
// loop and the unwrapEntityToFile snapshot/restore round trip.
static Ogre::MeshPtr createPlaneWithUv0(const std::string& name, int n = 8)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    sub->useSharedVertices = true;
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    size_t off = 0;
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    off += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, off, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES, 0);

    const int side = n + 1;
    const size_t vertCount = static_cast<size_t>(side) * side;
    std::vector<float> verts;
    verts.reserve(vertCount * 5);
    for (int y = 0; y < side; ++y) {
        for (int x = 0; x < side; ++x) {
            verts.push_back(static_cast<float>(x));
            verts.push_back(static_cast<float>(y));
            verts.push_back(0.0f);
            // UV0 spanning [0,1]×[0,1] so coverage ≈ 1.0
            verts.push_back(static_cast<float>(x) / static_cast<float>(n));
            verts.push_back(static_cast<float>(y) / static_cast<float>(n));
        }
    }
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), vertCount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    vbuf->writeData(0, verts.size() * sizeof(float), verts.data());
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = vertCount;

    std::vector<uint16_t> indices;
    indices.reserve(static_cast<size_t>(n) * n * 6);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const auto a = static_cast<uint16_t>(y * side + x);
            const auto b = static_cast<uint16_t>(a + 1);
            const auto c = static_cast<uint16_t>(a + side);
            const auto d = static_cast<uint16_t>(c + 1);
            indices.push_back(a); indices.push_back(c); indices.push_back(b);
            indices.push_back(b); indices.push_back(c); indices.push_back(d);
        }
    }
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, indices.size(),
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    ibuf->writeData(0, indices.size() * sizeof(uint16_t), indices.data());
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount  = indices.size();

    mesh->_setBounds(Ogre::AxisAlignedBox(0, 0, 0, n, n, 0));
    mesh->_setBoundingSphereRadius(static_cast<float>(n) * 1.5f);
    mesh->load();
    return mesh;
}

// ── Fixture ──────────────────────────────────────────────────────────────────

class UvUnwrapCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "GL context unavailable";
    }
    void TearDown() override { Manager::kill(); }

    Ogre::Entity* attach(Ogre::MeshPtr mesh, const std::string& tag) {
        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(tag + "_node");
        auto* entity = sceneMgr->createEntity(tag + "_entity", mesh);
        node->attachObject(entity);
        return entity;
    }
};

// ── infoForEntity: the UV0-coverage compute loop (mesh WITH UV0) ──────────────

TEST_F(UvUnwrapCoverageTest, InfoComputesUv0Coverage) {
    auto mesh = createPlaneWithUv0("UvCov_info");
    auto* entity = attach(mesh, "UvCov_info");

    const auto info = UvUnwrap::infoForEntity(entity);
    ASSERT_EQ(info.size(), 1);
    EXPECT_EQ(info[0].submeshIndex, 0);
    EXPECT_EQ(info[0].triangleCount, 128);
    EXPECT_GT(info[0].vertexCount, 0);
    EXPECT_TRUE(info[0].hasUv0);           // this mesh HAS UV0
    EXPECT_GE(info[0].uvChannelCount, 1);
    // UVs span [0,1]×[0,1] → coverage close to full.
    EXPECT_GT(info[0].uv0Coverage, 0.5);
    EXPECT_LE(info[0].uv0Coverage, 1.0001);
}

TEST_F(UvUnwrapCoverageTest, InfoNullEntityReturnsEmpty) {
    const auto info = UvUnwrap::infoForEntity(nullptr);
    EXPECT_TRUE(info.isEmpty());
}

// ── unwrapEntityToFile: snapshot / unwrap / export / restore round trip ───────

TEST_F(UvUnwrapCoverageTest, UnwrapToFileMeshRoundTripAndRestore) {
    auto mesh = createPlaneWithUv0("UvCov_tofile_mesh");
    auto* entity = attach(mesh, "UvCov_tofile_mesh");

    // Snapshot the live entity's info before the export.
    const auto before = UvUnwrap::infoForEntity(entity);
    ASSERT_EQ(before.size(), 1);
    const int beforeVerts = before[0].vertexCount;

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath("unwrapped.mesh");

    UvUnwrapOptions opts;
    opts.resolution = 256;
    opts.padding    = 2;
    opts.channel    = 0;

    const auto report = UvUnwrap::unwrapEntityToFile(entity, out, opts);
    EXPECT_TRUE(report.applied) << report.error.toStdString();
    EXPECT_TRUE(report.error.isEmpty());

    // Output file actually written and non-empty.
    QFileInfo fi(out);
    EXPECT_TRUE(fi.exists());
    EXPECT_GT(fi.size(), 0);

    // The LIVE entity must be bit-identical (restore worked): the
    // info on the live entity is unchanged after the export.
    const auto after = UvUnwrap::infoForEntity(entity);
    ASSERT_EQ(after.size(), 1);
    EXPECT_EQ(after[0].vertexCount, beforeVerts);
    EXPECT_EQ(after[0].triangleCount, before[0].triangleCount);
}

TEST_F(UvUnwrapCoverageTest, UnwrapToFileObjExtensionMapping) {
    auto mesh = createPlaneWithUv0("UvCov_tofile_obj");
    auto* entity = attach(mesh, "UvCov_tofile_obj");

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath("unwrapped.obj");

    const auto report = UvUnwrap::unwrapEntityToFile(entity, out);
    // .obj is one of the mapped extensions; expect a successful export.
    EXPECT_TRUE(report.applied) << report.error.toStdString();
    if (report.applied) {
        EXPECT_TRUE(QFileInfo(out).exists());
    }
    // Live entity still intact.
    EXPECT_EQ(UvUnwrap::infoForEntity(entity).size(), 1);
}

TEST_F(UvUnwrapCoverageTest, UnwrapToFileUnknownExtensionFallsBackToMesh) {
    auto mesh = createPlaneWithUv0("UvCov_tofile_unknown");
    auto* entity = attach(mesh, "UvCov_tofile_unknown");

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // Unknown extension → falls back to the Ogre Mesh filter branch.
    const QString out = dir.filePath("unwrapped.xyzzy");

    const auto report = UvUnwrap::unwrapEntityToFile(entity, out);
    // The fallback path runs; whether the exporter accepts the odd
    // extension is exporter-dependent, but the call must not crash and
    // must leave the live entity intact.
    EXPECT_EQ(UvUnwrap::infoForEntity(entity).size(), 1);
    (void)report;
}

// ── unwrapEntityToFile: error / empty-path / null guards ──────────────────────

TEST_F(UvUnwrapCoverageTest, UnwrapToFileNullEntity) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto report = UvUnwrap::unwrapEntityToFile(nullptr, dir.filePath("x.mesh"));
    EXPECT_FALSE(report.applied);
    EXPECT_EQ(report.error, QStringLiteral("null entity / no mesh"));
}

TEST_F(UvUnwrapCoverageTest, UnwrapToFileEmptyOutputPath) {
    auto mesh = createPlaneWithUv0("UvCov_emptypath");
    auto* entity = attach(mesh, "UvCov_emptypath");

    const auto report = UvUnwrap::unwrapEntityToFile(entity, QString());
    EXPECT_FALSE(report.applied);
    EXPECT_EQ(report.error, QStringLiteral("output path required"));
    // Guard hit before any mutation; live entity intact.
    EXPECT_EQ(UvUnwrap::infoForEntity(entity).size(), 1);
}

// ── reportToJson: field mapping ───────────────────────────────────────────────

TEST_F(UvUnwrapCoverageTest, ReportToJsonFieldMappingApplied) {
    UvUnwrapReport r;
    r.meshName           = "TheMesh";
    r.submeshCount       = 3;
    r.verticesBefore     = 100;
    r.verticesAfter      = 137;
    r.trianglesProcessed = 64;
    r.atlasWidth         = 1024;
    r.atlasHeight        = 512;
    r.chartCount         = 7;
    r.utilization        = 0.85;
    r.applied            = true;

    const QJsonObject obj = UvUnwrap::reportToJson(r);
    EXPECT_EQ(obj["mesh"].toString(), QStringLiteral("TheMesh"));
    EXPECT_TRUE(obj["applied"].toBool());
    EXPECT_EQ(obj["submeshCount"].toInt(), 3);
    EXPECT_EQ(obj["verticesBefore"].toInt(), 100);
    EXPECT_EQ(obj["verticesAfter"].toInt(), 137);
    EXPECT_EQ(obj["trianglesProcessed"].toInt(), 64);
    EXPECT_EQ(obj["atlasWidth"].toInt(), 1024);
    EXPECT_EQ(obj["atlasHeight"].toInt(), 512);
    EXPECT_EQ(obj["chartCount"].toInt(), 7);
    EXPECT_NEAR(obj["utilization"].toDouble(), 0.85, 1e-9);
    // No error set → key omitted.
    EXPECT_FALSE(obj.contains("error"));
}

TEST_F(UvUnwrapCoverageTest, ReportToJsonIncludesErrorWhenSet) {
    UvUnwrapReport r;
    r.applied = false;
    r.error   = QStringLiteral("boom");
    const QJsonObject obj = UvUnwrap::reportToJson(r);
    EXPECT_FALSE(obj["applied"].toBool());
    ASSERT_TRUE(obj.contains("error"));
    EXPECT_EQ(obj["error"].toString(), QStringLiteral("boom"));
}

// ── reportToText: applied + failed branches ──────────────────────────────────

TEST_F(UvUnwrapCoverageTest, ReportToTextAppliedBranch) {
    UvUnwrapReport r;
    r.meshName           = "PlaneMesh";
    r.submeshCount       = 1;
    r.verticesBefore     = 81;
    r.verticesAfter      = 90;
    r.trianglesProcessed = 128;
    r.atlasWidth         = 512;
    r.atlasHeight        = 512;
    r.chartCount         = 2;
    r.utilization        = 0.5;
    r.applied            = true;

    const QString txt = UvUnwrap::reportToText(r);
    EXPECT_TRUE(txt.contains("UV Unwrap"));
    EXPECT_TRUE(txt.contains("PlaneMesh"));
    EXPECT_TRUE(txt.contains("512"));
    EXPECT_TRUE(txt.contains("Charts"));
    EXPECT_TRUE(txt.contains("%"));
}

TEST_F(UvUnwrapCoverageTest, ReportToTextFailedBranchWithError) {
    UvUnwrapReport r;
    r.applied = false;
    r.error   = QStringLiteral("xatlas exploded");
    const QString txt = UvUnwrap::reportToText(r);
    EXPECT_TRUE(txt.contains("failed"));
    EXPECT_TRUE(txt.contains("xatlas exploded"));
}

TEST_F(UvUnwrapCoverageTest, ReportToTextFailedBranchUnknownError) {
    UvUnwrapReport r;
    r.applied = false;  // error left empty → "(unknown)"
    const QString txt = UvUnwrap::reportToText(r);
    EXPECT_TRUE(txt.contains("failed"));
    EXPECT_TRUE(txt.contains("(unknown)"));
}

// ── infoToJson: per-submesh array + top-level file key ────────────────────────

TEST_F(UvUnwrapCoverageTest, InfoToJsonFieldMapping) {
    QList<UvUnwrap::UvInfo> list;
    UvUnwrap::UvInfo a;
    a.submeshIndex = 0; a.vertexCount = 81; a.triangleCount = 128;
    a.uvChannelCount = 1; a.hasUv0 = true; a.uv0Coverage = 0.9;
    UvUnwrap::UvInfo b;
    b.submeshIndex = 1; b.vertexCount = 40; b.triangleCount = 60;
    b.uvChannelCount = 0; b.hasUv0 = false; b.uv0Coverage = 0.0;
    list << a << b;

    const QJsonObject obj = UvUnwrap::infoToJson(QStringLiteral("model.fbx"), list);
    EXPECT_EQ(obj["file"].toString(), QStringLiteral("model.fbx"));
    ASSERT_TRUE(obj["submeshes"].isArray());
    const QJsonArray arr = obj["submeshes"].toArray();
    ASSERT_EQ(arr.size(), 2);

    const QJsonObject e0 = arr[0].toObject();
    EXPECT_EQ(e0["submeshIndex"].toInt(), 0);
    EXPECT_EQ(e0["vertexCount"].toInt(), 81);
    EXPECT_EQ(e0["triangleCount"].toInt(), 128);
    EXPECT_EQ(e0["uvChannelCount"].toInt(), 1);
    EXPECT_TRUE(e0["hasUv0"].toBool());
    EXPECT_NEAR(e0["uv0Coverage"].toDouble(), 0.9, 1e-9);

    const QJsonObject e1 = arr[1].toObject();
    EXPECT_EQ(e1["submeshIndex"].toInt(), 1);
    EXPECT_FALSE(e1["hasUv0"].toBool());
}

TEST_F(UvUnwrapCoverageTest, InfoToJsonEmptyList) {
    const QJsonObject obj = UvUnwrap::infoToJson(QStringLiteral("empty.mesh"), {});
    EXPECT_EQ(obj["file"].toString(), QStringLiteral("empty.mesh"));
    ASSERT_TRUE(obj["submeshes"].isArray());
    EXPECT_EQ(obj["submeshes"].toArray().size(), 0);
}

// ── infoToText: populated + empty branches ────────────────────────────────────

TEST_F(UvUnwrapCoverageTest, InfoToTextPopulated) {
    QList<UvUnwrap::UvInfo> list;
    UvUnwrap::UvInfo a;
    a.submeshIndex = 0; a.vertexCount = 81; a.triangleCount = 128;
    a.uvChannelCount = 1; a.hasUv0 = true; a.uv0Coverage = 0.75;
    list << a;

    const QString txt = UvUnwrap::infoToText(QStringLiteral("model.glb"), list);
    EXPECT_TRUE(txt.contains("UV info"));
    EXPECT_TRUE(txt.contains("model.glb"));
    EXPECT_TRUE(txt.contains("verts=81"));
    EXPECT_TRUE(txt.contains("tris=128"));
    EXPECT_TRUE(txt.contains("uv0=yes"));
}

TEST_F(UvUnwrapCoverageTest, InfoToTextHasUv0NoBranch) {
    QList<UvUnwrap::UvInfo> list;
    UvUnwrap::UvInfo a;  // hasUv0 defaults to false
    a.submeshIndex = 0; a.vertexCount = 10; a.triangleCount = 4;
    list << a;
    const QString txt = UvUnwrap::infoToText(QStringLiteral("noUv.obj"), list);
    EXPECT_TRUE(txt.contains("uv0=no"));
}

TEST_F(UvUnwrapCoverageTest, InfoToTextEmpty) {
    const QString txt = UvUnwrap::infoToText(QStringLiteral("empty.mesh"), {});
    EXPECT_TRUE(txt.contains("empty.mesh"));
    EXPECT_TRUE(txt.contains("(no submeshes)"));
}
