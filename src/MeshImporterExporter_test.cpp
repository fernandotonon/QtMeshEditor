#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <OgreSceneNode.h>
#include <OgreSkeleton.h>
#include <OgreAnimation.h>
#include <OgreBone.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "OgreXML/OgreXMLSkeletonSerializer.h"
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

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
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
    QString expected = "3DS (*.3ds);;Assimp Binary (*.assbin);;Collada (*.dae);;FBX Binary (*.fbx);;OBJ (*.obj);;OBJ without MTL (*.objnomtl);;Ogre Mesh (*.mesh);;Ogre Mesh v1.0+(*.mesh);;Ogre Mesh v1.10+(*.mesh);;Ogre Mesh v1.4+(*.mesh);;Ogre Mesh v1.7+(*.mesh);;Ogre Mesh v1.8+(*.mesh);;Ogre XML (*.mesh.xml);;PLY (*.ply);;STL (*.stl);;X (*.x);;glTF 2.0 (*.gltf2);;glTF 2.0 Binary (*.glb2)";

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

// ── Regression: XML skeleton animation track-to-bone mapping ─────
// Verifies that XMLSkeletonSerializer creates animation tracks keyed
// by bone handle (not sequential index).  A mismatch causes
// Animation::apply(Skeleton*,...) to animate the wrong bones.

TEST_F(MeshImporterExporterTest, XMLSkeletonSerializer_TrackHandlesMatchBoneHandles) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    // Import FBX with skeleton
    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();

    // Export to Ogre XML (produces .skeleton.xml)
    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./tracktest.mesh.xml", "Ogre XML (*.mesh.xml)"), 0);

    // Re-import the skeleton XML into a fresh skeleton
    auto skelPtr = Ogre::SkeletonManager::getSingleton().create(
        "tracktest_verify.skeleton.xml", "General");
    Ogre::XMLSkeletonSerializer xmlSS;
    xmlSS.importSkeleton("./tracktest.skeleton.xml", skelPtr.get());

    ASSERT_GT(skelPtr->getNumAnimations(), 0u);

    auto* anim = skelPtr->getAnimation(static_cast<unsigned short>(0));
    for (const auto& [trackHandle, track] : anim->_getNodeTrackList())
    {
        // The track handle must match the associated bone's handle.
        // If they differ, Animation::apply(Skeleton*,...) will apply
        // keyframes to the wrong bone.
        auto* bone = dynamic_cast<Ogre::Bone*>(track->getAssociatedNode());
        ASSERT_NE(bone, nullptr) << "Track " << trackHandle << " has no associated bone";
        EXPECT_EQ(trackHandle, bone->getHandle())
            << "Track handle " << trackHandle
            << " does not match bone '" << bone->getName()
            << "' handle " << bone->getHandle();
    }

    // Clean up
    Ogre::SkeletonManager::getSingleton().remove(skelPtr);
    QFile::remove("./tracktest.mesh.xml");
    QFile::remove("./tracktest.skeleton.xml");
    QFile::remove("./tracktest.material");
}

// ── Export round-trip tests for additional formats ────────────────

TEST_F(MeshImporterExporterTest, ExportImport_OBJ_RoundTrip) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();

    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./roundtrip_obj.obj", "OBJ (*.obj)"), 0);

    QStringList reimport{"./roundtrip_obj.obj"};
    MeshImporterExporter::importer(reimport);
    EXPECT_GT(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);

    QFile::remove("./roundtrip_obj.obj");
    QFile::remove("./roundtrip_obj.material");
    QFile::remove("./roundtrip_obj.mtl");
}

TEST_F(MeshImporterExporterTest, ExportImport_STL_RoundTrip) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();

    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./roundtrip_stl.stl", "STL (*.stl)"), 0);

    QStringList reimport{"./roundtrip_stl.stl"};
    MeshImporterExporter::importer(reimport);
    EXPECT_GT(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);

    QFile::remove("./roundtrip_stl.stl");
    QFile::remove("./roundtrip_stl.material");
}

TEST_F(MeshImporterExporterTest, ExportImport_PLY_RoundTrip) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();

    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./roundtrip_ply.ply", "PLY (*.ply)"), 0);

    QStringList reimport{"./roundtrip_ply.ply"};
    MeshImporterExporter::importer(reimport);
    EXPECT_GT(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);

    QFile::remove("./roundtrip_ply.ply");
    QFile::remove("./roundtrip_ply.material");
}

// ── Export material test ─────────────────────────────────────────

TEST_F(MeshImporterExporterTest, ExportMaterial) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();

    // Export to Ogre mesh (produces .material file)
    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./matexport.mesh", "Ogre Mesh (*.mesh)"), 0);
    EXPECT_TRUE(QFile::exists("./matexport.material"));

    QFile::remove("./matexport.mesh");
    QFile::remove("./matexport.material");
    QFile::remove("./matexport.skeleton");
}

// ── Error handling tests ─────────────────────────────────────────

TEST_F(MeshImporterExporterTest, Importer_NonExistentFile) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();
    QStringList uri{"/tmp/definitely_nonexistent_xyz.mesh"};
    MeshImporterExporter::importer(uri);
    // No new nodes should be added for a non-existent file
    EXPECT_EQ(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);
}

TEST_F(MeshImporterExporterTest, Exporter_InvalidPath) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();

    // Export to a path that doesn't exist (nested directories)
    int result = MeshImporterExporter::exporter(sn, "/nonexistent_dir_xyz/sub/file.mesh", "Ogre Mesh (*.mesh)");
    // Should fail or handle gracefully
    EXPECT_NE(result, 0);
}

// ── GLB export round-trip ────────────────────────────────────────

TEST_F(MeshImporterExporterTest, ExportImport_GLB2_RoundTrip) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();

    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./roundtrip.glb2", "glTF 2.0 Binary (*.glb2)"), 0);

    QStringList reimport{"./roundtrip.glb2"};
    MeshImporterExporter::importer(reimport);
    EXPECT_GT(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);

    QFile::remove("./roundtrip.glb2");
    QFile::remove("./roundtrip.material");
}

// ── 3DS export round-trip ────────────────────────────────────────

TEST_F(MeshImporterExporterTest, ExportImport_3DS_RoundTrip) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();

    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./roundtrip.3ds", "3DS (*.3ds)"), 0);

    QStringList reimport{"./roundtrip.3ds"};
    MeshImporterExporter::importer(reimport);
    EXPECT_GT(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);

    QFile::remove("./roundtrip.3ds");
    QFile::remove("./roundtrip.material");
}

// ── In-memory mesh export tests ──────────────────────────────────

TEST_F(MeshImporterExporterTest, ExportInMemoryMesh_OBJ) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemoryTriangleMesh("ExportOBJTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportOBJNode");
    auto* entity = sceneMgr->createEntity("ExportOBJEntity", mesh);
    node->attachObject(entity);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./inmem_export.obj", "OBJ (*.obj)"), 0);
    EXPECT_TRUE(QFile::exists("./inmem_export.obj"));

    QFile::remove("./inmem_export.obj");
    QFile::remove("./inmem_export.material");
    QFile::remove("./inmem_export.mtl");
}

TEST_F(MeshImporterExporterTest, ExportInMemoryMesh_STL) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemoryTriangleMesh("ExportSTLTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportSTLNode");
    auto* entity = sceneMgr->createEntity("ExportSTLEntity", mesh);
    node->attachObject(entity);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./inmem_export.stl", "STL (*.stl)"), 0);
    EXPECT_TRUE(QFile::exists("./inmem_export.stl"));

    QFile::remove("./inmem_export.stl");
    QFile::remove("./inmem_export.material");
}

TEST_F(MeshImporterExporterTest, ExportInMemoryMesh_glTF2) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemoryTriangleMesh("ExportGLTFTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportGLTFNode");
    auto* entity = sceneMgr->createEntity("ExportGLTFEntity", mesh);
    node->attachObject(entity);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./inmem_export.gltf2", "glTF 2.0 (*.gltf2)"), 0);
    EXPECT_TRUE(QFile::exists("./inmem_export.gltf2"));

    QFile::remove("./inmem_export.gltf2");
    QFile::remove("./inmem_export.material");
}

TEST_F(MeshImporterExporterTest, ExportInMemoryMesh_FBX) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemoryTriangleMesh("ExportFBXTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportFBXNode");
    auto* entity = sceneMgr->createEntity("ExportFBXEntity", mesh);
    node->attachObject(entity);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./inmem_export.fbx", "FBX Binary (*.fbx)"), 0);
    EXPECT_TRUE(QFile::exists("./inmem_export.fbx"));

    QFile::remove("./inmem_export.fbx");
    QFile::remove("./inmem_export.material");
}

TEST_F(MeshImporterExporterTest, ExportInMemoryMesh_OgreMesh) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemoryTriangleMesh("ExportMeshTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportMeshNode");
    auto* entity = sceneMgr->createEntity("ExportMeshEntity", mesh);
    node->attachObject(entity);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./inmem_export.mesh", "Ogre Mesh (*.mesh)"), 0);
    EXPECT_TRUE(QFile::exists("./inmem_export.mesh"));

    QFile::remove("./inmem_export.mesh");
    QFile::remove("./inmem_export.material");
}

TEST_F(MeshImporterExporterTest, ExportInMemoryMesh_OgreXML) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemoryTriangleMesh("ExportXMLTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportXMLNode");
    auto* entity = sceneMgr->createEntity("ExportXMLEntity", mesh);
    node->attachObject(entity);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./inmem_export.mesh.xml", "Ogre XML (*.mesh.xml)"), 0);
    EXPECT_TRUE(QFile::exists("./inmem_export.mesh.xml"));

    QFile::remove("./inmem_export.mesh.xml");
    QFile::remove("./inmem_export.skeleton.xml");
    QFile::remove("./inmem_export.material");
}

TEST_F(MeshImporterExporterTest, ImportMultipleFiles) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();
    QStringList files{"./media/models/Rumba Dancing.fbx", "./media/models/Twist Dance.fbx"};
    MeshImporterExporter::importer(files);
    // Both files should be imported
    EXPECT_GE(Manager::getSingleton()->getSceneNodes().size(), nodesBefore + 2);
}

TEST(MeshImporterExporterStandaloneTest, GetSupportedExportFormats) {
    QString filter = MeshImporterExporter::exportFileDialogFilter();
    EXPECT_TRUE(filter.contains("*.obj"));
    EXPECT_TRUE(filter.contains("*.stl"));
    EXPECT_TRUE(filter.contains("*.dae"));
    EXPECT_TRUE(filter.contains("*.mesh"));
    EXPECT_TRUE(filter.contains("*.gltf2"));
    EXPECT_TRUE(filter.contains("*.glb2"));
    EXPECT_TRUE(filter.contains("*.mesh.xml"));
}

// ── FBX round-trip with skeleton ─────────────────────────────────

TEST_F(MeshImporterExporterTest, ExportImport_FBX_WithSkeleton_RoundTrip) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    // Import FBX with skeleton
    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();

    // Verify skeleton exists before export
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    ASSERT_TRUE(sceneMgr->hasEntity(sn->getName()));
    auto* entity = sceneMgr->getEntity(sn->getName());
    ASSERT_TRUE(entity->hasSkeleton());

    // Export to FBX (exercises FBXExporter code path with skeleton data)
    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./roundtrip_skel.fbx", "FBX Binary (*.fbx)"), 0);
    EXPECT_TRUE(QFile::exists("./roundtrip_skel.fbx"));

    // Reimport the exported FBX
    QStringList reimport{"./roundtrip_skel.fbx"};
    MeshImporterExporter::importer(reimport);
    EXPECT_GT(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);

    // Verify skeleton was preserved after reimport
    auto* reimportedNode = Manager::getSingleton()->getSceneNodes().last();
    if (sceneMgr->hasEntity(reimportedNode->getName())) {
        auto* reimportedEntity = sceneMgr->getEntity(reimportedNode->getName());
        EXPECT_TRUE(reimportedEntity->hasSkeleton());
    }

    // Clean up
    QFile::remove("./roundtrip_skel.fbx");
    QFile::remove("./roundtrip_skel.material");
}

// ── Collada round-trip with skeleton ─────────────────────────────

TEST_F(MeshImporterExporterTest, ExportImport_Collada_WithSkeleton_RoundTrip) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();
    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();

    // Verify skeleton
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    ASSERT_TRUE(sceneMgr->hasEntity(sn->getName()));
    auto* entity = sceneMgr->getEntity(sn->getName());
    ASSERT_TRUE(entity->hasSkeleton());

    // Export to Collada (exercises buildAiScene with skeleton)
    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./roundtrip_skel.dae", "Collada (*.dae)"), 0);
    EXPECT_TRUE(QFile::exists("./roundtrip_skel.dae"));

    // Reimport
    QStringList reimport{"./roundtrip_skel.dae"};
    MeshImporterExporter::importer(reimport);
    EXPECT_GT(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);

    // Verify skeleton was preserved after reimport
    auto* reimportedNode = Manager::getSingleton()->getSceneNodes().last();
    if (sceneMgr->hasEntity(reimportedNode->getName())) {
        auto* reimportedEntity = sceneMgr->getEntity(reimportedNode->getName());
        EXPECT_TRUE(reimportedEntity->hasSkeleton());
    }

    // Clean up
    QFile::remove("./roundtrip_skel.dae");
    QFile::remove("./roundtrip_skel.material");
}

// ── In-memory skeleton mesh export (no animations) ──────────────

TEST_F(MeshImporterExporterTest, ExportInMemorySkeletonMesh_OBJ) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemorySkeletonMesh("ExportSkelOBJ");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportSkelOBJNode");
    auto* entity = sceneMgr->createEntity("ExportSkelOBJEntity", mesh);
    node->attachObject(entity);

    ASSERT_TRUE(entity->hasSkeleton());
    ASSERT_EQ(MeshImporterExporter::exporter(node, "./inmem_skel.obj", "OBJ (*.obj)"), 0);
    EXPECT_TRUE(QFile::exists("./inmem_skel.obj"));

    QFile::remove("./inmem_skel.obj");
    QFile::remove("./inmem_skel.material");
    QFile::remove("./inmem_skel.mtl");
}

TEST_F(MeshImporterExporterTest, ExportInMemorySkeletonMesh_FBX) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemorySkeletonMesh("ExportSkelFBX");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportSkelFBXNode");
    auto* entity = sceneMgr->createEntity("ExportSkelFBXEntity", mesh);
    node->attachObject(entity);

    ASSERT_TRUE(entity->hasSkeleton());
    ASSERT_EQ(MeshImporterExporter::exporter(node, "./inmem_skel.fbx", "FBX Binary (*.fbx)"), 0);
    EXPECT_TRUE(QFile::exists("./inmem_skel.fbx"));

    QFile::remove("./inmem_skel.fbx");
    QFile::remove("./inmem_skel.material");
}

TEST_F(MeshImporterExporterTest, ExportInMemorySkeletonMesh_Collada) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemorySkeletonMesh("ExportSkelDAE");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportSkelDAENode");
    auto* entity = sceneMgr->createEntity("ExportSkelDAEEntity", mesh);
    node->attachObject(entity);

    ASSERT_TRUE(entity->hasSkeleton());
    ASSERT_EQ(MeshImporterExporter::exporter(node, "./inmem_skel.dae", "Collada (*.dae)"), 0);
    EXPECT_TRUE(QFile::exists("./inmem_skel.dae"));

    QFile::remove("./inmem_skel.dae");
    QFile::remove("./inmem_skel.material");
}

TEST_F(MeshImporterExporterTest, ExportInMemorySkeletonMesh_glTF2) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemorySkeletonMesh("ExportSkelGLTF");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportSkelGLTFNode");
    auto* entity = sceneMgr->createEntity("ExportSkelGLTFEntity", mesh);
    node->attachObject(entity);

    ASSERT_TRUE(entity->hasSkeleton());
    ASSERT_EQ(MeshImporterExporter::exporter(node, "./inmem_skel.gltf2", "glTF 2.0 (*.gltf2)"), 0);
    EXPECT_TRUE(QFile::exists("./inmem_skel.gltf2"));

    QFile::remove("./inmem_skel.gltf2");
    QFile::remove("./inmem_skel.material");
}

TEST_F(MeshImporterExporterTest, ExportInMemorySkeletonMesh_OgreMesh) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemorySkeletonMesh("ExportSkelMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportSkelMeshNode");
    auto* entity = sceneMgr->createEntity("ExportSkelMeshEntity", mesh);
    node->attachObject(entity);

    ASSERT_TRUE(entity->hasSkeleton());
    ASSERT_EQ(MeshImporterExporter::exporter(node, "./inmem_skel.mesh", "Ogre Mesh (*.mesh)"), 0);
    EXPECT_TRUE(QFile::exists("./inmem_skel.mesh"));

    QFile::remove("./inmem_skel.mesh");
    QFile::remove("./inmem_skel.material");
    QFile::remove("./inmem_skel.skeleton");
}

TEST_F(MeshImporterExporterTest, ExportInMemorySkeletonMesh_OgreXML) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemorySkeletonMesh("ExportSkelXML");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportSkelXMLNode");
    auto* entity = sceneMgr->createEntity("ExportSkelXMLEntity", mesh);
    node->attachObject(entity);

    ASSERT_TRUE(entity->hasSkeleton());
    ASSERT_EQ(MeshImporterExporter::exporter(node, "./inmem_skel.mesh.xml", "Ogre XML (*.mesh.xml)"), 0);
    EXPECT_TRUE(QFile::exists("./inmem_skel.mesh.xml"));

    QFile::remove("./inmem_skel.mesh.xml");
    QFile::remove("./inmem_skel.skeleton.xml");
    QFile::remove("./inmem_skel.material");
}

// ── In-memory animated entity export (skeleton + animations) ────

TEST_F(MeshImporterExporterTest, ExportAnimatedEntity_OBJ) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("ExportAnimOBJ");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());
    auto* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./anim_export.obj", "OBJ (*.obj)"), 0);
    EXPECT_TRUE(QFile::exists("./anim_export.obj"));

    QFile::remove("./anim_export.obj");
    QFile::remove("./anim_export.material");
    QFile::remove("./anim_export.mtl");
}

TEST_F(MeshImporterExporterTest, ExportAnimatedEntity_FBX) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("ExportAnimFBX");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());
    auto* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./anim_export.fbx", "FBX Binary (*.fbx)"), 0);
    EXPECT_TRUE(QFile::exists("./anim_export.fbx"));

    QFile::remove("./anim_export.fbx");
    QFile::remove("./anim_export.material");
}

TEST_F(MeshImporterExporterTest, ExportAnimatedEntity_Collada) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("ExportAnimDAE");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());
    auto* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./anim_export.dae", "Collada (*.dae)"), 0);
    EXPECT_TRUE(QFile::exists("./anim_export.dae"));

    QFile::remove("./anim_export.dae");
    QFile::remove("./anim_export.material");
}

TEST_F(MeshImporterExporterTest, ExportAnimatedEntity_glTF2) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("ExportAnimGLTF");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());
    auto* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./anim_export.gltf2", "glTF 2.0 (*.gltf2)"), 0);
    EXPECT_TRUE(QFile::exists("./anim_export.gltf2"));

    QFile::remove("./anim_export.gltf2");
    QFile::remove("./anim_export.material");
}

TEST_F(MeshImporterExporterTest, ExportAnimatedEntity_OgreMesh) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("ExportAnimMesh");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());
    auto* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./anim_export.mesh", "Ogre Mesh (*.mesh)"), 0);
    EXPECT_TRUE(QFile::exists("./anim_export.mesh"));

    QFile::remove("./anim_export.mesh");
    QFile::remove("./anim_export.material");
    QFile::remove("./anim_export.skeleton");
}

TEST_F(MeshImporterExporterTest, ExportAnimatedEntity_OgreXML) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto* entity = createAnimatedTestEntity("ExportAnimXML");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());
    auto* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./anim_export.mesh.xml", "Ogre XML (*.mesh.xml)"), 0);
    EXPECT_TRUE(QFile::exists("./anim_export.mesh.xml"));

    QFile::remove("./anim_export.mesh.xml");
    QFile::remove("./anim_export.skeleton.xml");
    QFile::remove("./anim_export.material");
}

// ── OgreXML reimport with skeleton verification ─────────────────

TEST_F(MeshImporterExporterTest, ImportOgreXML_SkeletonXMLSerializerPath) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    // Import FBX with skeleton
    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();

    // Export to Ogre XML
    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./xmlskel_test.mesh.xml", "Ogre XML (*.mesh.xml)"), 0);
    EXPECT_TRUE(QFile::exists("./xmlskel_test.mesh.xml"));
    EXPECT_TRUE(QFile::exists("./xmlskel_test.skeleton.xml"));

    int nodesBefore = Manager::getSingleton()->getSceneNodes().size();

    // Reimport the Ogre XML -- exercises XMLSkeletonSerializer path
    QStringList reimport{"./xmlskel_test.mesh.xml"};
    MeshImporterExporter::importer(reimport);
    EXPECT_GT(Manager::getSingleton()->getSceneNodes().size(), nodesBefore);

    // The reimported entity should have a skeleton
    auto* reimportedSn = Manager::getSingleton()->getSceneNodes().last();
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    if (sceneMgr->hasEntity(reimportedSn->getName())) {
        auto* reimportedEntity = sceneMgr->getEntity(reimportedSn->getName());
        EXPECT_TRUE(reimportedEntity->hasSkeleton());
        if (reimportedEntity->hasSkeleton()) {
            // Verify the skeleton has animations
            EXPECT_GT(reimportedEntity->getSkeleton()->getNumAnimations(), 0u);
        }
    }

    // Clean up
    QFile::remove("./xmlskel_test.mesh.xml");
    QFile::remove("./xmlskel_test.skeleton.xml");
    QFile::remove("./xmlskel_test.material");
}

// ── OBJ without MTL export format ───────────────────────────────

TEST_F(MeshImporterExporterTest, ExportOBJNoMTL) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemoryTriangleMesh("ExportOBJNoMTLTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportOBJNoMTLNode");
    auto* entity = sceneMgr->createEntity("ExportOBJNoMTLEntity", mesh);
    node->attachObject(entity);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./nomtl_export.objnomtl", "OBJ without MTL (*.objnomtl)"), 0);
    EXPECT_TRUE(QFile::exists("./nomtl_export.objnomtl"));

    // Clean up
    QFile::remove("./nomtl_export.objnomtl");
    QFile::remove("./nomtl_export.material");
    QFile::remove("./nomtl_export.mtl");
}

TEST_F(MeshImporterExporterTest, ExportOBJNoMTL_FromImportedMesh) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();

    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./nomtl_imported.objnomtl", "OBJ without MTL (*.objnomtl)"), 0);
    EXPECT_TRUE(QFile::exists("./nomtl_imported.objnomtl"));

    // Clean up
    QFile::remove("./nomtl_imported.objnomtl");
    QFile::remove("./nomtl_imported.material");
    QFile::remove("./nomtl_imported.mtl");
}

// ── Assimp Binary export format ─────────────────────────────────

TEST_F(MeshImporterExporterTest, ExportAssimpBinary) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    auto mesh = createInMemoryTriangleMesh("ExportAssbinTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ExportAssbinNode");
    auto* entity = sceneMgr->createEntity("ExportAssbinEntity", mesh);
    node->attachObject(entity);

    ASSERT_EQ(MeshImporterExporter::exporter(node, "./assbin_export.assbin", "Assimp Binary (*.assbin)"), 0);
    EXPECT_TRUE(QFile::exists("./assbin_export.assbin"));

    // Clean up
    QFile::remove("./assbin_export.assbin");
    QFile::remove("./assbin_export.material");
}

TEST_F(MeshImporterExporterTest, ExportAssimpBinary_FromImportedMesh) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";
    }

    QStringList uri{"./media/models/Rumba Dancing.fbx"};
    MeshImporterExporter::importer(uri);
    auto* sn = Manager::getSingleton()->getSceneNodes().last();

    ASSERT_EQ(MeshImporterExporter::exporter(sn, "./assbin_imported.assbin", "Assimp Binary (*.assbin)"), 0);
    EXPECT_TRUE(QFile::exists("./assbin_imported.assbin"));

    // Clean up
    QFile::remove("./assbin_imported.assbin");
    QFile::remove("./assbin_imported.material");
}

