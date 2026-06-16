// Coverage tests for MeshImporterExporter::exporter(SceneNode*, uri, format) '.mesh'
// branch and MeshImporterExporter::importer(QStringList) '.mesh' branch.
//
// Distinct filename + distinct TEST suite names (MeshImporterExporterCoverageTest /
// MeshImporterExporterCoverageStandaloneTest) from the existing
// MeshImporterExporter_test.cpp so there is no ODR / duplicate-registration clash.
//
// Primary ask: the round-trip via exporter()/importer() (NOT raw MeshSerializer) —
// export createInMemoryTriangleMesh (3 verts, 1 submesh) to .mesh, destroy the node,
// drop the cached mesh, re-import from disk and assert vertexCount == 3 and
// getNumSubMeshes() == 1 on the reimported entity. We also iterate every version
// string in the exporter's versionMap so all 6 version-int branches execute.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QFile>
#include <QString>
#include <QStringList>

#include <OgreSceneNode.h>
#include <OgreEntity.h>
#include <OgreSubMesh.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgreVertexIndexData.h>
#include <OgreMaterialManager.h>

#include "Manager.h"
#include "SelectionSet.h"
#include "MeshImporterExporter.h"
#include "TestHelpers.h"

namespace {

class MeshImporterExporterCoverageTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    QTemporaryDir tempDir;

    void SetUp() override {
        SelectionSet::kill();
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles()) << "GL/hardware buffers required (Xvfb in CI)";
        createStandardOgreMaterials();
        ASSERT_TRUE(tempDir.isValid());
    }

    void TearDown() override {
        SelectionSet::kill();
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(50);
    }

    // Creates a node + entity from an in-memory triangle mesh. The entity is named
    // after the scene node (Manager::createEntity convention) so the exporter's
    // hasEntity(sn->getName()) lookup succeeds.
    Ogre::SceneNode* makeNodeWithTriangle(const QString& nodeName,
                                          const std::string& meshName)
    {
        Ogre::MeshPtr mesh = createInMemoryTriangleMesh(meshName);
        EXPECT_TRUE(bool(mesh));
        if (!mesh) return nullptr;
        Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode(nodeName);
        EXPECT_NE(node, nullptr);
        if (!node) return nullptr;
        Ogre::Entity* en = Manager::getSingleton()->createEntity(node, mesh);
        EXPECT_NE(en, nullptr);
        if (!en) return nullptr;
        return node;
    }

    // Drops a mesh from MeshManager by resource name + group so a later import
    // reads the bytes from disk instead of returning the cached in-memory mesh.
    static void dropCachedMesh(const QString& meshFilePath)
    {
        const QFileInfo fi(QFileInfo(meshFilePath).absoluteFilePath());
        const Ogre::String resName = fi.fileName().toStdString();
        const Ogre::String group = fi.absolutePath().toStdString();
        if (auto existing = Ogre::MeshManager::getSingleton().getByName(
                resName, group))
        {
            Ogre::MeshManager::getSingleton().remove(existing);
        }
    }

    // Locates the first reimported Entity in the scene.
    static Ogre::Entity* firstSceneEntity()
    {
        auto* manager = Manager::getSingleton();
        for (auto* node : manager->getSceneNodes()) {
            for (auto* obj : node->getAttachedObjects()) {
                if (obj->getMovableType() == "Entity")
                    return static_cast<Ogre::Entity*>(obj);
            }
        }
        return nullptr;
    }
};

// ── Primary round-trip: exporter() → importer() via the real API ──────────────

TEST_F(MeshImporterExporterCoverageTest, RoundTrip_TriangleMesh_DefaultMeshFormat)
{
    const QString uri = tempDir.filePath("rt_default.mesh");

    Ogre::SceneNode* node = makeNodeWithTriangle("RtDefaultNode", "rt_default_mesh");
    ASSERT_NE(node, nullptr);

    // Export through the public exporter() '.mesh' branch (version 0).
    ASSERT_EQ(MeshImporterExporter::exporter(node, uri, "Ogre Mesh (*.mesh)"), 0);
    ASSERT_TRUE(QFileInfo::exists(uri));

    // Tear down the source so the reimport must read from disk.
    Manager::getSingleton()->destroySceneNode(node);
    dropCachedMesh(uri);
    ASSERT_TRUE(Manager::getSingleton()->getSceneNodes().isEmpty());

    // Reimport through the public importer() '.mesh' branch.
    MeshImporterExporter::importer(QStringList{uri});

    ASSERT_FALSE(Manager::getSingleton()->getSceneNodes().isEmpty());
    Ogre::Entity* imported = firstSceneEntity();
    ASSERT_NE(imported, nullptr);

    Ogre::MeshPtr importedMesh = imported->getMesh();
    ASSERT_TRUE(bool(importedMesh));
    ASSERT_NE(importedMesh->sharedVertexData, nullptr);
    EXPECT_EQ(importedMesh->sharedVertexData->vertexCount, 3u);
    EXPECT_EQ(importedMesh->getNumSubMeshes(), 1u);
    EXPECT_GE(imported->getNumSubEntities(), 1u);
}

// Iterate every version string so all 6 versionMap branches (the int 0..5) execute
// in the exporter. Each one must produce a loadable .mesh that round-trips.
TEST_F(MeshImporterExporterCoverageTest, RoundTrip_AllMeshVersionStrings)
{
    const QStringList versionFormats = {
        "Ogre Mesh (*.mesh)",       // version 0
        "Ogre Mesh v1.10+(*.mesh)", // version 1
        "Ogre Mesh v1.8+(*.mesh)",  // version 2
        "Ogre Mesh v1.7+(*.mesh)",  // version 3
        "Ogre Mesh v1.4+(*.mesh)",  // version 4
        "Ogre Mesh v1.0+(*.mesh)",  // version 5
    };

    int idx = 0;
    for (const QString& fmt : versionFormats) {
        const QString tag = QString::number(idx++);
        const QString nodeName = "VerNode" + tag;
        const std::string meshName = ("ver_mesh_" + tag).toStdString();
        const QString uri = tempDir.filePath("ver_" + tag + ".mesh");

        Ogre::SceneNode* node = makeNodeWithTriangle(nodeName, meshName);
        ASSERT_NE(node, nullptr) << "format: " << fmt.toStdString();

        EXPECT_EQ(MeshImporterExporter::exporter(node, uri, fmt), 0)
            << "export failed for format: " << fmt.toStdString();
        EXPECT_TRUE(QFileInfo::exists(uri))
            << "no file written for format: " << fmt.toStdString();

        Manager::getSingleton()->destroySceneNode(node);
        dropCachedMesh(uri);

        MeshImporterExporter::importer(QStringList{uri});

        Ogre::Entity* imported = firstSceneEntity();
        ASSERT_NE(imported, nullptr) << "reimport failed for format: " << fmt.toStdString();
        Ogre::MeshPtr importedMesh = imported->getMesh();
        ASSERT_TRUE(bool(importedMesh));
        ASSERT_NE(importedMesh->sharedVertexData, nullptr);
        EXPECT_EQ(importedMesh->sharedVertexData->vertexCount, 3u)
            << "format: " << fmt.toStdString();
        EXPECT_EQ(importedMesh->getNumSubMeshes(), 1u)
            << "format: " << fmt.toStdString();

        // Clean the imported node/mesh between iterations so each round-trip is
        // isolated and firstSceneEntity() picks up the next one.
        for (auto* n : Manager::getSingleton()->getSceneNodes())
            Manager::getSingleton()->destroySceneNode(n);
        dropCachedMesh(uri);
    }
}

// ── exporter() '.mesh' branch: sidecar .material is written ────────────────────

TEST_F(MeshImporterExporterCoverageTest, Exporter_MeshFormat_WritesSidecarMaterial)
{
    const QString uri = tempDir.filePath("sidecar_out.mesh");

    Ogre::MeshPtr mesh = createInMemoryTriangleMesh("sidecar_out_mesh");
    ASSERT_TRUE(bool(mesh));
    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("SidecarOutNode");
    ASSERT_NE(node, nullptr);
    Ogre::Entity* en = Manager::getSingleton()->createEntity(node, mesh);
    ASSERT_NE(en, nullptr);

    auto mat = Ogre::MaterialManager::getSingleton().create(
        "CoverageSidecarMat", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_TRUE(bool(mat));
    mat->getTechnique(0)->getPass(0)->setDiffuse(0.2f, 0.4f, 0.6f, 1.0f);
    mat->compile();
    en->getSubEntity(0)->setMaterial(mat);
    en->getMesh()->getSubMesh(0)->setMaterialName("CoverageSidecarMat");

    ASSERT_EQ(MeshImporterExporter::exporter(node, uri, "Ogre Mesh (*.mesh)"), 0);
    EXPECT_TRUE(QFileInfo::exists(uri));

    // exportMaterial writes a .material next to the mesh (basename + ".material").
    const QString sidecar = tempDir.filePath("sidecar_out.material");
    EXPECT_TRUE(QFileInfo::exists(sidecar))
        << "exporter() .mesh branch should write a sidecar .material";
}

// Reimport picks up the sidecar material script written by exporter() (not BaseWhite),
// mirroring Importer_MeshLoadsSidecarMaterialScript but driving BOTH sides through
// the public exporter()/importer() entry points.
TEST_F(MeshImporterExporterCoverageTest, RoundTrip_SidecarMaterial_AppliedOnReimport)
{
    const QString uri = tempDir.filePath("sidecar_rt.mesh");

    Ogre::MeshPtr mesh = createInMemoryTriangleMesh("sidecar_rt_mesh");
    ASSERT_TRUE(bool(mesh));
    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("SidecarRtNode");
    ASSERT_NE(node, nullptr);
    Ogre::Entity* en = Manager::getSingleton()->createEntity(node, mesh);
    ASSERT_NE(en, nullptr);

    auto mat = Ogre::MaterialManager::getSingleton().create(
        "CoverageSidecarRtMat", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_TRUE(bool(mat));
    mat->getTechnique(0)->getPass(0)->setDiffuse(0.9f, 0.1f, 0.1f, 1.0f);
    mat->compile();
    en->getSubEntity(0)->setMaterial(mat);
    en->getMesh()->getSubMesh(0)->setMaterialName("CoverageSidecarRtMat");

    ASSERT_EQ(MeshImporterExporter::exporter(node, uri, "Ogre Mesh (*.mesh)"), 0);
    ASSERT_TRUE(QFileInfo::exists(uri));

    // Tear down + drop the in-memory material so reimport must parse the sidecar.
    Manager::getSingleton()->destroySceneNode(node);
    dropCachedMesh(uri);
    mat.reset();
    if (Ogre::MaterialManager::getSingleton().getByName(
            "CoverageSidecarRtMat",
            Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME))
    {
        Ogre::MaterialManager::getSingleton().remove(
            "CoverageSidecarRtMat",
            Ogre::ResourceGroupManager::AUTODETECT_RESOURCE_GROUP_NAME);
    }

    MeshImporterExporter::importer(QStringList{uri});

    Ogre::Entity* imported = firstSceneEntity();
    ASSERT_NE(imported, nullptr);
    ASSERT_GE(imported->getNumSubEntities(), 1u);
    const Ogre::String importedMat = imported->getSubEntity(0)->getMaterialName();
    EXPECT_NE(importedMat, "BaseWhite");
    EXPECT_EQ(importedMat, "CoverageSidecarRtMat");
}

// ── importer() '.mesh' branch: MeshManager remove-then-load (replaced file) ─────
// The importer drops any cached mesh by name+group before loading, so re-importing
// the SAME path after the bytes on disk changed picks up the new vertex count.
TEST_F(MeshImporterExporterCoverageTest, Importer_MeshFormat_RemoveThenLoad_PicksUpReplacedFile)
{
    const QString uri = tempDir.filePath("replaced.mesh");

    // First export: a 3-vertex triangle mesh.
    Ogre::SceneNode* node = makeNodeWithTriangle("ReplacedNodeA", "replaced_mesh_a");
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(MeshImporterExporter::exporter(node, uri, "Ogre Mesh (*.mesh)"), 0);
    Manager::getSingleton()->destroySceneNode(node);
    dropCachedMesh(uri);

    // Import once — populates the MeshManager cache under name="replaced.mesh".
    MeshImporterExporter::importer(QStringList{uri});
    Ogre::Entity* first = firstSceneEntity();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->getMesh()->sharedVertexData->vertexCount, 3u);

    // Clear the scene but DELIBERATELY leave the MeshManager cache populated to
    // exercise the importer's remove-then-load guard against a replaced file.
    for (auto* n : Manager::getSingleton()->getSceneNodes())
        Manager::getSingleton()->destroySceneNode(n);

    // Overwrite the on-disk file with a different mesh (welded cube, 8 verts).
    {
        Ogre::MeshPtr cube = createInMemoryWeldedCube("replaced_mesh_cube");
        ASSERT_TRUE(bool(cube));
        Ogre::SceneNode* cubeNode = Manager::getSingleton()->addSceneNode("ReplacedNodeB");
        ASSERT_NE(cubeNode, nullptr);
        Ogre::Entity* cubeEn = Manager::getSingleton()->createEntity(cubeNode, cube);
        ASSERT_NE(cubeEn, nullptr);
        ASSERT_EQ(MeshImporterExporter::exporter(cubeNode, uri, "Ogre Mesh (*.mesh)"), 0);
        Manager::getSingleton()->destroySceneNode(cubeNode);
    }

    // Re-import the SAME path. The importer must remove the stale cache entry and
    // load the replaced bytes (8 verts), not return the cached 3-vert mesh.
    MeshImporterExporter::importer(QStringList{uri});
    Ogre::Entity* second = firstSceneEntity();
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(bool(second->getMesh()));
    ASSERT_NE(second->getMesh()->getNumSubMeshes(), 0u);
    // The cube submesh uses its own vertexData (not shared) — assert via the submesh.
    Ogre::SubMesh* sm = second->getMesh()->getSubMesh(0);
    ASSERT_NE(sm, nullptr);
    ASSERT_NE(sm->vertexData, nullptr);
    EXPECT_EQ(sm->vertexData->vertexCount, 8u)
        << "importer should reload the replaced file, not return the cached mesh";
}

// importer() creates exactly one scene node + entity for a single .mesh path, and
// applyNormalMapsToEntity runs without error (no normal map present → no-op path).
TEST_F(MeshImporterExporterCoverageTest, Importer_MeshFormat_CreatesSingleNodeAndEntity)
{
    const QString uri = tempDir.filePath("single.mesh");

    Ogre::SceneNode* node = makeNodeWithTriangle("SingleNode", "single_src_mesh");
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(MeshImporterExporter::exporter(node, uri, "Ogre Mesh (*.mesh)"), 0);
    Manager::getSingleton()->destroySceneNode(node);
    dropCachedMesh(uri);

    MeshImporterExporter::importer(QStringList{uri});

    EXPECT_EQ(Manager::getSingleton()->getSceneNodes().size(), 1);
    Ogre::Entity* imported = firstSceneEntity();
    ASSERT_NE(imported, nullptr);
    // The created scene node is named after the file's baseName ("single").
    bool foundSingle = false;
    for (auto* n : Manager::getSingleton()->getSceneNodes())
        if (n->getName() == "single") foundSingle = true;
    EXPECT_TRUE(foundSingle) << "importer names the node after the file basename";
}

// ── error / edge guards on the exporter() .mesh branch ─────────────────────────

TEST_F(MeshImporterExporterCoverageTest, Exporter_MeshFormat_NodeWithoutEntity_ReturnsMinusOne)
{
    const QString uri = tempDir.filePath("no_entity.mesh");
    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("NoEntityMeshNode");
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(MeshImporterExporter::exporter(node, uri, "Ogre Mesh (*.mesh)"), -1);
    EXPECT_FALSE(QFileInfo::exists(uri));
}

TEST_F(MeshImporterExporterCoverageTest, Exporter_MeshFormat_EmptyUri_ReturnsMinusOne)
{
    Ogre::SceneNode* node = makeNodeWithTriangle("EmptyUriMeshNode", "empty_uri_mesh");
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(MeshImporterExporter::exporter(node, QString(), "Ogre Mesh (*.mesh)"), -1);
}

} // namespace
