#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <OgreSceneNode.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "Manager.h"
#include "MeshImporterExporter.h"
#include <OgreException.h>
#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>

// Helper function to create required OGRE materials for tests
static void createOGREMaterials()
{
    Ogre::MaterialPtr baseWhiteMat = Ogre::MaterialManager::getSingleton().getByName(
        "BaseWhiteNoLighting", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!baseWhiteMat)
    {
        baseWhiteMat = Ogre::MaterialManager::getSingleton().create(
            "BaseWhiteNoLighting", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        baseWhiteMat->getTechnique(0)->getPass(0)->setDiffuse(1, 1, 1, 1);
        baseWhiteMat->getTechnique(0)->getPass(0)->setAmbient(1, 1, 1);
        baseWhiteMat->getTechnique(0)->getPass(0)->setSelfIllumination(1, 1, 1);
        baseWhiteMat->getTechnique(0)->setLightingEnabled(false);
    }

    Ogre::MaterialPtr baseWhiteMat2 = Ogre::MaterialManager::getSingleton().getByName(
        "BaseWhite", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!baseWhiteMat2)
    {
        baseWhiteMat2 = Ogre::MaterialManager::getSingleton().create(
            "BaseWhite", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        baseWhiteMat2->getTechnique(0)->getPass(0)->setDiffuse(1, 1, 1, 1);
        baseWhiteMat2->getTechnique(0)->getPass(0)->setAmbient(1, 1, 1);
    }
}

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
        createOGREMaterials();
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
    QString expected = "3DS (*.3ds);;Assimp Binary (*.assbin);;Collada (*.dae);;OBJ (*.obj);;OBJ without MTL (*.objnomtl);;Ogre Mesh (*.mesh);;Ogre Mesh v1.0+(*.mesh);;Ogre Mesh v1.10+(*.mesh);;Ogre Mesh v1.4+(*.mesh);;Ogre Mesh v1.7+(*.mesh);;Ogre Mesh v1.8+(*.mesh);;Ogre XML (*.mesh.xml);;PLY (*.ply);;PLY Binary (*.plyb);;STL (*.stl);;STL Binary (*.stlb);;STP (*.stp);;X (*.x);;glTF 1.0 (*.gltf);;glTF 1.0 Binary (*.glb);;glTF 2.0 (*.gltf2);;glTF 2.0 Binary (*.glb2)";

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
