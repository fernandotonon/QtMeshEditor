#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <OgreSceneNode.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "Manager.h"
#include "MeshImporterExporter.h"
#include <OgreException.h>
#include "TestHelpers.h"

class MeshImporterExporterTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        try {
            Manager::getSingleton();  // headless — no render window needed
        } catch (const Ogre::Exception& e) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
        }
        createStandardOgreMaterials();
    }

    void TearDown() override {
        Manager::kill();

        if (app) {
            app->processEvents();
        }
        QThread::msleep(50);
    }
};

TEST(MeshImporterExporterStandaloneTest, FormatFileURI_ValidURIAndFormat_ReturnsFormattedURI) {
    QString uri = "/path/to/file.obj";
    QString format = "Ogre XML (*.mesh.xml)";
    QString expected = "/path/to/file.obj.mesh.xml";

    QString result = MeshImporterExporter::formatFileURI(uri, format);

    EXPECT_EQ(result, expected);
}

TEST(MeshImporterExporterStandaloneTest, FormatFileURI_URIWithExtension_ReturnsURIWithoutChanges) {
    QString uri = "/path/to/file.mesh.xml";
    QString format = "Ogre XML (*.mesh.xml)";
    QString expected = "/path/to/file.mesh.xml";

    QString result = MeshImporterExporter::formatFileURI(uri, format);

    EXPECT_EQ(result, expected);
}

TEST(MeshImporterExporterStandaloneTest, FormatFileURI_URIWithoutExtension_ReturnsURIWithFormatExtension) {
    QString uri = "/path/to/file";
    QString format = "Ogre XML (*.mesh.xml)";
    QString expected = "/path/to/file.mesh.xml";

    QString result = MeshImporterExporter::formatFileURI(uri, format);

    EXPECT_EQ(result, expected);
}

TEST(MeshImporterExporterStandaloneTest, FormatFileURI_URIWithExtensionAndNoFormat_ReturnsURIWithoutChanges) {
    QString uri = "/path/to/file.mesh.xml";
    QString expected = "/path/to/file.mesh.xml";

    QString result = MeshImporterExporter::formatFileURI(uri, nullptr);

    EXPECT_EQ(result, expected);
}

TEST(MeshImporterExporterStandaloneTest, FormatFileURI_URIWithoutExtensionAndNoFormat_ReturnsURIWithoutChanges) {
    QString uri = "/path/to/file";
    QString expected = "/path/to/file";

    QString result = MeshImporterExporter::formatFileURI(uri, nullptr);

    EXPECT_EQ(result, expected);
}

TEST(MeshImporterExporterStandaloneTest, FormatFileURI_NULLURI_ReturnsEmptyString) {
    QString format = "Ogre XML (*.mesh.xml)";

    QString result = MeshImporterExporter::formatFileURI(nullptr, format);

    EXPECT_EQ(result, "");
}

TEST(MeshImporterExporterStandaloneTest, FormatFileURI_EmptyURI_ReturnsEmptyString) {
    QString format = "Ogre XML (*.mesh.xml)";

    QString result = MeshImporterExporter::formatFileURI("", format);

    EXPECT_EQ(result, "");
}

TEST(MeshImporterExporterStandaloneTest, FormatFileURI_UnknownFormat_ReturnsURIWithoutChanges) {
    QString uri = "/path/to/file.obj";
    QString format = "Unknown Format";
    QString expected = "/path/to/file.obj";

    QString result = MeshImporterExporter::formatFileURI(uri, format);

    EXPECT_EQ(result, expected);
}

TEST(MeshImporterExporterStandaloneTest, ExportFileDialogFilter_ReturnsFilterString) {
    QString expected = "3DS (*.3ds);;Assimp Binary (*.assbin);;Collada (*.dae);;OBJ (*.obj);;OBJ without MTL (*.objnomtl);;Ogre Mesh (*.mesh);;Ogre Mesh v1.0+(*.mesh);;Ogre Mesh v1.10+(*.mesh);;Ogre Mesh v1.4+(*.mesh);;Ogre Mesh v1.7+(*.mesh);;Ogre Mesh v1.8+(*.mesh);;Ogre XML (*.mesh.xml);;PLY (*.ply);;STL (*.stl);;X (*.x);;glTF 2.0 (*.gltf2);;glTF 2.0 Binary (*.glb2)";

    QString result = MeshImporterExporter::exportFileDialogFilter();

    EXPECT_EQ(result, expected);
}

TEST(MeshImporterExporterStandaloneTest, Exporter_NullSceneNode_ReturnMinusOne) {
    EXPECT_EQ(MeshImporterExporter::exporter(nullptr, "", ""), -1);
}

TEST_F(MeshImporterExporterTest, Exporter_EmptyUri_ReturnMinusOne) {
    QString uri = "";
    auto sceneNodeName = "MeshImporterExporterTestSceneNode";
    auto sn = Manager::getSingleton()->addSceneNode(sceneNodeName);

    EXPECT_EQ(MeshImporterExporter::exporter(sn, uri, "Ogre Mesh (*.mesh)"), -1);
}

TEST_F(MeshImporterExporterTest, Exporter_ValidSceneNodeAndUri_ReturnMinusOne) {
    QString uri = "/path/to/exported.mesh";
    QString format = "Ogre Mesh (*.mesh)";
    auto sceneNodeName = "MeshImporterExporterTestSceneNode";
    auto sn = Manager::getSingleton()->addSceneNode(sceneNodeName);

    EXPECT_EQ(MeshImporterExporter::exporter(sn, uri, format), -1);
}

TEST_F(MeshImporterExporterTest, Importer_ValidMesh) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList validUri{"", "./media/models/Twist Dance.fbx"};
    MeshImporterExporter::importer(validUri);
    auto sn = Manager::getSingleton()->getSceneNodes().last();

    EXPECT_EQ(MeshImporterExporter::exporter(sn, "./exported.mesh", "Ogre Mesh (*.mesh)"), 0);
    EXPECT_EQ(MeshImporterExporter::exporter(sn, "./exported.mesh.xml", "Ogre XML (*.mesh.xml)"), 0);
    EXPECT_EQ(MeshImporterExporter::exporter(sn, "./exported.x", "X (*.x)"), 0);

    // Test import ogre xml
    QStringList ogreXMLURL{"./exported.mesh.xml"};
    MeshImporterExporter::importer(ogreXMLURL);

    // Clean up
    QFile::remove("./exported.mesh");
    QFile::remove("./exported.material");
    QFile::remove("./exported.mesh.xml");
    QFile::remove("./exported.skeleton.xml");
    QFile::remove("./exported.x");
}

// ── Round-trip tests using Rumba Dancing.fbx ──────────────────────

TEST_F(MeshImporterExporterTest, Importer_RumbaDancingFBX) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);

    auto nodes = Manager::getSingleton()->getSceneNodes();
    ASSERT_FALSE(nodes.isEmpty());

    auto* sn = nodes.last();
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    ASSERT_TRUE(sceneMgr->hasEntity(sn->getName()));
    auto* entity = sceneMgr->getEntity(sn->getName());
    EXPECT_TRUE(entity->hasSkeleton());
}

TEST_F(MeshImporterExporterTest, ExportImport_OgreMesh_RoundTrip) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    // Import FBX
    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();

    // Export to .mesh
    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./roundtrip.mesh", "Ogre Mesh (*.mesh)"), 0);

    // Reimport
    QStringList reimport{"./roundtrip.mesh"};
    MeshImporterExporter::importer(reimport);
    EXPECT_GT(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);

    // Clean up
    QFile::remove("./roundtrip.mesh");
    QFile::remove("./roundtrip.material");
    QFile::remove("./roundtrip.skeleton");
}

TEST_F(MeshImporterExporterTest, ExportImport_OgreXML_RoundTrip) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();

    // Export to .mesh.xml
    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./roundtrip.mesh.xml", "Ogre XML (*.mesh.xml)"), 0);

    // Reimport — should preserve skeleton
    QStringList reimport{"./roundtrip.mesh.xml"};
    MeshImporterExporter::importer(reimport);
    EXPECT_GT(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);

    auto* reimportedSn = Manager::getSingleton()->getSceneNodes().last();
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    if (sceneMgr->hasEntity(reimportedSn->getName())) {
        auto* entity = sceneMgr->getEntity(reimportedSn->getName());
        EXPECT_TRUE(entity->hasSkeleton());
    }

    // Clean up
    QFile::remove("./roundtrip.mesh.xml");
    QFile::remove("./roundtrip.skeleton.xml");
    QFile::remove("./roundtrip.material");
}

TEST_F(MeshImporterExporterTest, ExportImport_Collada_RoundTrip) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();

    // Export to .dae
    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./roundtrip.dae", "Collada (*.dae)"), 0);

    // Reimport
    QStringList reimport{"./roundtrip.dae"};
    MeshImporterExporter::importer(reimport);
    EXPECT_GT(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);

    // Clean up
    QFile::remove("./roundtrip.dae");
    QFile::remove("./roundtrip.material");
}

TEST_F(MeshImporterExporterTest, ExportImport_X_RoundTrip) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();

    // Export to .x
    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./roundtrip.x", "X (*.x)"), 0);

    // Reimport
    QStringList reimport{"./roundtrip.x"};
    MeshImporterExporter::importer(reimport);
    EXPECT_GT(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);

    // Clean up
    QFile::remove("./roundtrip.x");
    QFile::remove("./roundtrip.material");
}

TEST_F(MeshImporterExporterTest, ExportImport_glTF2_RoundTrip) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();

    // Export to .gltf2
    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./roundtrip.gltf2", "glTF 2.0 (*.gltf2)"), 0);

    // Reimport
    QStringList reimport{"./roundtrip.gltf2"};
    MeshImporterExporter::importer(reimport);
    EXPECT_GT(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);

    // Clean up
    QFile::remove("./roundtrip.gltf2");
    QFile::remove("./roundtrip.material");
}
