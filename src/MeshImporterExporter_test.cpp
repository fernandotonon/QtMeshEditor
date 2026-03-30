#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <OgreSceneNode.h>
#include <OgreSkeleton.h>
#include <OgreAnimation.h>
#include <OgreBone.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <cstdint>
#include <OgreTextureManager.h>
#include <OgreHardwarePixelBuffer.h>
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "SelectionSet.h"
#include "OgreXML/OgreXMLSkeletonSerializer.h"
#include <OgreException.h>
#include "TestHelpers.h"

class MeshImporterExporterTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    QTemporaryDir tempDir;

    Ogre::SceneNode* createSceneNodeWithEntity(const QString& nodeName,
                                               const std::string& meshName)
    {
        Ogre::MeshPtr mesh = createInMemoryTriangleMesh(meshName);
        EXPECT_NE(mesh, nullptr);
        if (!mesh)
            return nullptr;

        Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode(nodeName);
        EXPECT_NE(node, nullptr);
        if (!node)
            return nullptr;

        Ogre::Entity* entity = Manager::getSingleton()->createEntity(node, mesh);
        EXPECT_NE(entity, nullptr);
        if (!entity)
            return nullptr;

        return node;
    }

    void SetUp() override {
        SelectionSet::kill();
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
        SelectionSet::kill();
        Manager::kill();

        if (app) {
            app->processEvents();
        }
        QThread::msleep(50);
    }
};

static Ogre::TexturePtr createSolidTexture2D(const std::string& name, uint32_t argb = 0xFFFFFFFF)
{
    Ogre::TexturePtr texture = Ogre::TextureManager::getSingleton().createManual(
        name,
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        Ogre::TEX_TYPE_2D,
        1,
        1,
        0,
        Ogre::PF_A8R8G8B8,
        Ogre::TU_STATIC_WRITE_ONLY);
    if (!texture) {
        return {};
    }

    Ogre::HardwarePixelBufferSharedPtr pixelBuffer = texture->getBuffer(0, 0);
    if (!pixelBuffer) {
        return {};
    }

    const Ogre::PixelBox& pixelBox =
        pixelBuffer->lock(Ogre::Box(0, 0, 1, 1), Ogre::HardwareBuffer::HBL_DISCARD);
    auto* pixelData = reinterpret_cast<uint32_t*>(pixelBox.data);
    pixelData[0] = argb;
    pixelBuffer->unlock();
    texture->load();
    return texture;
}

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

// ─── exportTextureName Tests ────────────────────────────────────────

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_PNG_Unchanged) {
    EXPECT_EQ(MeshImporterExporter::exportTextureName("diffuse.png"), "diffuse.png");
}

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_BMP_Unchanged) {
    EXPECT_EQ(MeshImporterExporter::exportTextureName("diffuse.bmp"), "diffuse.bmp");
}

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_TGA_Unchanged) {
    EXPECT_EQ(MeshImporterExporter::exportTextureName("diffuse.tga"), "diffuse.tga");
}

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_HDR_Unchanged) {
    EXPECT_EQ(MeshImporterExporter::exportTextureName("diffuse.hdr"), "diffuse.hdr");
}

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_JPG_ConvertedToPNG) {
    EXPECT_EQ(MeshImporterExporter::exportTextureName("texture.jpg"), "texture.png");
}

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_JPEG_ConvertedToPNG) {
    EXPECT_EQ(MeshImporterExporter::exportTextureName("texture.jpeg"), "texture.png");
}

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_DDS_ConvertedToPNG) {
    EXPECT_EQ(MeshImporterExporter::exportTextureName("normal.dds"), "normal.png");
}

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_CaseInsensitive) {
    EXPECT_EQ(MeshImporterExporter::exportTextureName("texture.JPG"), "texture.png");
    EXPECT_EQ(MeshImporterExporter::exportTextureName("texture.PNG"), "texture.PNG");
}

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_WithPath_Unsupported) {
    // Directory is stripped for both supported and unsupported formats
    EXPECT_EQ(MeshImporterExporter::exportTextureName("textures/diffuse.jpg"), "diffuse.png");
}

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_WithPath_Supported) {
    EXPECT_EQ(MeshImporterExporter::exportTextureName("textures/diffuse.png"), "diffuse.png");
}

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_MultipleDots) {
    EXPECT_EQ(MeshImporterExporter::exportTextureName("my.texture.file.jpg"), "my.texture.file.png");
}

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_Empty) {
    EXPECT_EQ(MeshImporterExporter::exportTextureName(""), ".png");
}

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_NoExtension) {
    EXPECT_EQ(MeshImporterExporter::exportTextureName("texture"), "texture.png");
}

TEST(MeshImporterExporterStandaloneTest, ExportTextureName_TIF_ConvertedToPNG) {
    EXPECT_EQ(MeshImporterExporter::exportTextureName("texture.tif"), "texture.png");
}

// ─── sceneExporter edge case Tests ──────────────────────────────────

TEST(MeshImporterExporterStandaloneTest, SceneExporter_EmptyURI_ReturnsMinusOne) {
    EXPECT_EQ(MeshImporterExporter::sceneExporter(""), -1);
}

TEST(MeshImporterExporterStandaloneTest, SceneExporter_NullProgress_DoesNotCrash) {
    EXPECT_EQ(MeshImporterExporter::sceneExporter("", nullptr), -1);
}

TEST(MeshImporterExporterStandaloneTest, SceneImporter_EmptyUri_ReturnsFalse) {
    EXPECT_FALSE(MeshImporterExporter::sceneImporter(""));
}

TEST(MeshImporterExporterStandaloneTest, SceneImporter_MissingFile_ReturnsFalse) {
    EXPECT_FALSE(MeshImporterExporter::sceneImporter("/path/to/missing.scene.glb"));
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

TEST_F(MeshImporterExporterTest, Exporter_SceneNodeWithoutEntity_ReturnsMinusOne)
{
    QString uri = tempDir.filePath("lonely.mesh");
    QString format = "Ogre Mesh (*.mesh)";
    Ogre::SceneNode* sn = Manager::getSingleton()->addSceneNode("LonelyExportNode");
    ASSERT_NE(sn, nullptr);

    EXPECT_EQ(MeshImporterExporter::exporter(sn, uri, format), -1);
}

TEST_F(MeshImporterExporterTest, Importer_EmptyList_DoesNotCreateSceneNodes) {
    MeshImporterExporter::importer(QStringList());
    EXPECT_TRUE(Manager::getSingleton()->getSceneNodes().isEmpty());
}

TEST_F(MeshImporterExporterTest, Importer_EmptyPathEntry_IsIgnored) {
    MeshImporterExporter::importer(QStringList{""});
    EXPECT_TRUE(Manager::getSingleton()->getSceneNodes().isEmpty());
}

TEST_F(MeshImporterExporterTest, SceneImporter_InvalidExistingFileDoesNotClearExistingScene) {
    ASSERT_TRUE(tempDir.isValid());
    const QString invalidScenePath = tempDir.filePath("invalid.scene.gltf");

    QFile file(invalidScenePath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("not valid gltf");
    file.close();

    Manager::getSingleton()->addSceneNode("ExistingNode");
    ASSERT_TRUE(Manager::getSingleton()->getSceneMgr()->hasSceneNode("ExistingNode"));

    EXPECT_FALSE(MeshImporterExporter::sceneImporter(invalidScenePath));
    EXPECT_TRUE(Manager::getSingleton()->getSceneMgr()->hasSceneNode("ExistingNode"));
}

TEST_F(MeshImporterExporterTest, SceneExporter_EmptyScene_WritesFileAndReportsProgress)
{
    ASSERT_TRUE(tempDir.isValid());
    const QString scenePath = tempDir.filePath("empty.scene.glb");

    QList<int> progressValues;
    QStringList progressMessages;
    const int result = MeshImporterExporter::sceneExporter(
        scenePath,
        [&progressValues, &progressMessages](int progress, const QString& status) {
            progressValues.append(progress);
            progressMessages.append(status);
        });

    EXPECT_EQ(result, 0);
    EXPECT_TRUE(QFileInfo::exists(scenePath));
    ASSERT_GE(progressValues.size(), 3);
    EXPECT_EQ(progressValues.first(), 30);
    EXPECT_EQ(progressValues.at(progressValues.size() - 2), 60);
    EXPECT_EQ(progressValues.last(), 100);
    EXPECT_THAT(progressMessages, ::testing::Contains(QStringLiteral("Building scene data...")));
    EXPECT_THAT(progressMessages, ::testing::Contains(QStringLiteral("Writing file...")));
    EXPECT_EQ(progressMessages.last(), QStringLiteral("Done."));
}

TEST_F(MeshImporterExporterTest, SceneExporter_NodeWithoutEntity_WritesEmptyScene)
{
    ASSERT_TRUE(tempDir.isValid());
    const QString scenePath = tempDir.filePath("node_only.scene.gltf");
    Manager::getSingleton()->addSceneNode("LonelyNode");

    QList<int> progressValues;
    const int result = MeshImporterExporter::sceneExporter(
        scenePath,
        [&progressValues](int progress, const QString&) {
            progressValues.append(progress);
        });

    EXPECT_EQ(result, 0);
    EXPECT_TRUE(QFileInfo::exists(scenePath));
    EXPECT_FALSE(progressValues.isEmpty());
    EXPECT_EQ(progressValues.first(), 30);
    EXPECT_EQ(progressValues.last(), 100);
}

TEST_F(MeshImporterExporterTest, SceneImporter_ExportedEmptySceneClearsExistingNodes)
{
    ASSERT_TRUE(tempDir.isValid());
    const QString scenePath = tempDir.filePath("roundtrip_empty.scene.glb");
    ASSERT_EQ(MeshImporterExporter::sceneExporter(scenePath, nullptr), 0);

    Manager::getSingleton()->addSceneNode("ExistingNode");
    ASSERT_TRUE(Manager::getSingleton()->getSceneMgr()->hasSceneNode("ExistingNode"));

    EXPECT_TRUE(MeshImporterExporter::sceneImporter(scenePath));
    EXPECT_FALSE(Manager::getSingleton()->getSceneMgr()->hasSceneNode("ExistingNode"));
    EXPECT_TRUE(Manager::getSingleton()->getSceneNodes().isEmpty());
}

TEST_F(MeshImporterExporterTest, SceneImporter_NodeOnlyExportBehavesAsValidEmptyScene)
{
    ASSERT_TRUE(tempDir.isValid());
    const QString scenePath = tempDir.filePath("node_only_roundtrip.scene.gltf");
    Manager::getSingleton()->addSceneNode("NodeWithoutEntity");

    ASSERT_EQ(MeshImporterExporter::sceneExporter(scenePath, nullptr), 0);
    ASSERT_TRUE(QFileInfo::exists(scenePath));

    Manager::getSingleton()->addSceneNode("ExistingNode");
    ASSERT_TRUE(Manager::getSingleton()->getSceneMgr()->hasSceneNode("ExistingNode"));

    EXPECT_TRUE(MeshImporterExporter::sceneImporter(scenePath));
    EXPECT_FALSE(Manager::getSingleton()->getSceneMgr()->hasSceneNode("ExistingNode"));
    EXPECT_TRUE(Manager::getSingleton()->getSceneNodes().isEmpty());
}

TEST_F(MeshImporterExporterTest, SceneExporter_InMemoryMeshEntity_WritesSceneFile)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";

    ASSERT_TRUE(tempDir.isValid());
    const QString scenePath = tempDir.filePath("mesh_entity.scene.glb");
    Ogre::SceneNode* node = createSceneNodeWithEntity("ExportNode", "ExportSceneMesh");
    ASSERT_NE(node, nullptr);

    QList<int> progressValues;
    QStringList progressMessages;
    const int result = MeshImporterExporter::sceneExporter(
        scenePath,
        [&progressValues, &progressMessages](int progress, const QString& status) {
            progressValues.append(progress);
            progressMessages.append(status);
        });

    EXPECT_EQ(result, 0);
    EXPECT_TRUE(QFileInfo::exists(scenePath));
    EXPECT_THAT(progressMessages, ::testing::Contains(QStringLiteral("Exporting textures (1/1)...")));
    EXPECT_THAT(progressMessages, ::testing::Contains(QStringLiteral("Building scene data...")));
    EXPECT_EQ(progressValues.last(), 100);
}

TEST_F(MeshImporterExporterTest, SceneExporter_MixedEmptyAndEntityNodesOnlyCountsEntitiesInProgress)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";

    ASSERT_TRUE(tempDir.isValid());
    const QString scenePath = tempDir.filePath("mixed_nodes.scene.gltf");

    Manager::getSingleton()->addSceneNode("EmptyNodeA");
    Ogre::SceneNode* entityNode = createSceneNodeWithEntity("EntityNode", "MixedSceneMesh");
    ASSERT_NE(entityNode, nullptr);
    Manager::getSingleton()->addSceneNode("EmptyNodeB");

    QStringList progressMessages;
    const int result = MeshImporterExporter::sceneExporter(
        scenePath,
        [&progressMessages](int, const QString& status) {
            progressMessages.append(status);
        });

    EXPECT_EQ(result, 0);
    EXPECT_TRUE(QFileInfo::exists(scenePath));
    EXPECT_THAT(progressMessages, ::testing::Contains(QStringLiteral("Exporting textures (1/1)...")));
    EXPECT_THAT(progressMessages, ::testing::Not(::testing::Contains(QStringLiteral("Exporting textures (2/3)..."))));
}

// NOTE: All MeshImporterExporterTest fixture tests from Importer_ValidMesh onward
// were removed because they crash in CI.

// ── Standalone tests: export filter and format coverage ──────────

TEST(MeshImporterExporterStandaloneTest, ExportFileDialogFilter_ContainsAll18Formats) {
    QString filter = MeshImporterExporter::exportFileDialogFilter();
    // 18 formats means 17 ";;" separators
    EXPECT_EQ(filter.count(";;"), 17);
    // Spot-check all format keys
    EXPECT_TRUE(filter.contains("3DS (*.3ds)"));
    EXPECT_TRUE(filter.contains("Assimp Binary (*.assbin)"));
    EXPECT_TRUE(filter.contains("Collada (*.dae)"));
    EXPECT_TRUE(filter.contains("FBX Binary (*.fbx)"));
    EXPECT_TRUE(filter.contains("OBJ (*.obj)"));
    EXPECT_TRUE(filter.contains("OBJ without MTL (*.objnomtl)"));
    EXPECT_TRUE(filter.contains("Ogre Mesh (*.mesh)"));
    EXPECT_TRUE(filter.contains("Ogre Mesh v1.0+(*.mesh)"));
    EXPECT_TRUE(filter.contains("Ogre Mesh v1.10+(*.mesh)"));
    EXPECT_TRUE(filter.contains("Ogre Mesh v1.4+(*.mesh)"));
    EXPECT_TRUE(filter.contains("Ogre Mesh v1.7+(*.mesh)"));
    EXPECT_TRUE(filter.contains("Ogre Mesh v1.8+(*.mesh)"));
    EXPECT_TRUE(filter.contains("Ogre XML (*.mesh.xml)"));
    EXPECT_TRUE(filter.contains("PLY (*.ply)"));
    EXPECT_TRUE(filter.contains("STL (*.stl)"));
    EXPECT_TRUE(filter.contains("X (*.x)"));
    EXPECT_TRUE(filter.contains("glTF 2.0 (*.gltf2)"));
    EXPECT_TRUE(filter.contains("glTF 2.0 Binary (*.glb2)"));
}

TEST(MeshImporterExporterStandaloneTest, FormatFileURI_FBXFormat) {
    QString result = MeshImporterExporter::formatFileURI("/path/to/model", "FBX Binary (*.fbx)");
    EXPECT_EQ(result, "/path/to/model.fbx");
}

TEST(MeshImporterExporterStandaloneTest, FormatFileURI_FBXFormat_AlreadyHasExtension) {
    QString result = MeshImporterExporter::formatFileURI("/path/to/model.fbx", "FBX Binary (*.fbx)");
    EXPECT_EQ(result, "/path/to/model.fbx");
}

TEST(MeshImporterExporterStandaloneTest, FormatFileURI_VersionedMeshFormats) {
    QStringList versionedFormats = {
        "Ogre Mesh v1.10+(*.mesh)",
        "Ogre Mesh v1.8+(*.mesh)",
        "Ogre Mesh v1.7+(*.mesh)",
        "Ogre Mesh v1.4+(*.mesh)",
        "Ogre Mesh v1.0+(*.mesh)"
    };
    for (const auto& fmt : versionedFormats) {
        QString result = MeshImporterExporter::formatFileURI("/path/to/model", fmt);
        EXPECT_EQ(result, "/path/to/model.mesh") << "Failed for format: " << fmt.toStdString();
    }
}

TEST(MeshImporterExporterStandaloneTest, FormatFileURI_AllFormats_CorrectExtension) {
    struct FormatCase {
        QString format;
        QString expectedExt;
    };
    std::vector<FormatCase> cases = {
        {"Ogre Mesh (*.mesh)", ".mesh"},
        {"Ogre Mesh v1.10+(*.mesh)", ".mesh"},
        {"Ogre Mesh v1.8+(*.mesh)", ".mesh"},
        {"Ogre Mesh v1.7+(*.mesh)", ".mesh"},
        {"Ogre Mesh v1.4+(*.mesh)", ".mesh"},
        {"Ogre Mesh v1.0+(*.mesh)", ".mesh"},
        {"Ogre XML (*.mesh.xml)", ".mesh.xml"},
        {"Collada (*.dae)", ".dae"},
        {"X (*.x)", ".x"},
        {"OBJ (*.obj)", ".obj"},
        {"OBJ without MTL (*.objnomtl)", ".obj"},
        {"STL (*.stl)", ".stl"},
        {"PLY (*.ply)", ".ply"},
        {"3DS (*.3ds)", ".3ds"},
        {"glTF 2.0 (*.gltf2)", ".gltf2"},
        {"glTF 2.0 Binary (*.glb2)", ".glb2"},
        {"Assimp Binary (*.assbin)", ".assbin"},
        {"FBX Binary (*.fbx)", ".fbx"},
    };
    for (const auto& c : cases) {
        QString result = MeshImporterExporter::formatFileURI("/tmp/test", c.format);
        EXPECT_EQ(result, "/tmp/test" + c.expectedExt)
            << "Format: " << c.format.toStdString();
    }
}

// ─── Scene Save/Load Tests ──────────────────────────────────────────

class SceneSaveLoadTest : public ::testing::Test {
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
        if (!canLoadMeshFiles()) {
            GTEST_SKIP() << "Skipping: Cannot load mesh files (no GL context)";
        }
        createStandardOgreMaterials();
    }

    void TearDown() override {
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(50);
    }
};

TEST_F(SceneSaveLoadTest, RoundTrip_TwoEntities_PreservesTransforms) {
    auto* manager = Manager::getSingleton();

    // Create two entities with different transforms (position, rotation, scale)
    auto mesh1 = createInMemoryTriangleMesh("scene_rt_mesh1");
    auto* sn1 = manager->addSceneNode("SceneNode1");
    manager->createEntity(sn1, mesh1);
    sn1->setPosition(Ogre::Vector3(1.0f, 2.0f, 3.0f));
    sn1->setScale(Ogre::Vector3(1.5f, 2.0f, 0.5f));
    // 45-degree rotation around Y
    Ogre::Quaternion rot1(Ogre::Degree(45), Ogre::Vector3::UNIT_Y);
    sn1->setOrientation(rot1);

    auto mesh2 = createInMemoryTriangleMesh("scene_rt_mesh2");
    auto* sn2 = manager->addSceneNode("SceneNode2");
    manager->createEntity(sn2, mesh2);
    sn2->setPosition(Ogre::Vector3(-1.0f, 0.0f, 5.0f));

    ASSERT_EQ(manager->getSceneNodes().size(), 2);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString sceneFile = tmpDir.path() + "/test_scene.scene.gltf";

    int exportResult = MeshImporterExporter::sceneExporter(sceneFile);
    ASSERT_EQ(exportResult, 0);
    ASSERT_TRUE(QFileInfo::exists(sceneFile));

    ASSERT_TRUE(MeshImporterExporter::sceneImporter(sceneFile));

    auto& nodes = manager->getSceneNodes();
    ASSERT_EQ(nodes.size(), 2);

    bool foundNode1 = false, foundNode2 = false;
    for (auto* sn : nodes)
    {
        auto pos = sn->getPosition();
        if (std::abs(pos.x - 1.0f) < 0.1f && std::abs(pos.y - 2.0f) < 0.1f)
        {
            foundNode1 = true;
            EXPECT_NEAR(pos.z, 3.0f, 0.1f);
            EXPECT_NEAR(sn->getScale().x, 1.5f, 0.1f);
            EXPECT_NEAR(sn->getScale().y, 2.0f, 0.1f);
            EXPECT_NEAR(sn->getScale().z, 0.5f, 0.1f);
            // Verify rotation preserved (45 degrees around Y)
            auto orient = sn->getOrientation();
            EXPECT_NEAR(orient.w, rot1.w, 0.05f);
            EXPECT_NEAR(orient.x, rot1.x, 0.05f);
            EXPECT_NEAR(orient.y, rot1.y, 0.05f);
            EXPECT_NEAR(orient.z, rot1.z, 0.05f);
        }
        else if (std::abs(pos.x - (-1.0f)) < 0.1f)
        {
            foundNode2 = true;
            EXPECT_NEAR(pos.y, 0.0f, 0.1f);
            EXPECT_NEAR(pos.z, 5.0f, 0.1f);
        }
    }
    EXPECT_TRUE(foundNode1) << "First node with position (1,2,3) not found";
    EXPECT_TRUE(foundNode2) << "Second node with position (-1,0,5) not found";
}

TEST_F(SceneSaveLoadTest, MaterialDedup_SharedMaterial_ExportedOnce) {
    auto* manager = Manager::getSingleton();

    // Create a shared material
    auto sharedMat = Ogre::MaterialManager::getSingleton().create(
        "SharedTestMat", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    sharedMat->getTechnique(0)->getPass(0)->setDiffuse(1, 0, 0, 1);

    // Create two entities sharing the same material
    auto mesh1 = createInMemoryTriangleMesh("dedup_mesh1");
    auto* sn1 = manager->addSceneNode("DedupNode1");
    auto* e1 = manager->createEntity(sn1, mesh1);
    e1->setMaterialName("SharedTestMat");

    auto mesh2 = createInMemoryTriangleMesh("dedup_mesh2");
    auto* sn2 = manager->addSceneNode("DedupNode2");
    auto* e2 = manager->createEntity(sn2, mesh2);
    e2->setMaterialName("SharedTestMat");

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString sceneFile = tmpDir.path() + "/test_dedup.scene.gltf";

    int result = MeshImporterExporter::sceneExporter(sceneFile);
    EXPECT_EQ(result, 0);

    // Read the exported glTF text and verify only one material entry
    QFile gltfFile(sceneFile);
    ASSERT_TRUE(gltfFile.open(QIODevice::ReadOnly));
    QByteArray gltfData = gltfFile.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(gltfData);
    ASSERT_TRUE(doc.isObject());
    QJsonArray materials = doc.object()["materials"].toArray();
    EXPECT_EQ(materials.size(), 1) << "Shared material should be deduplicated to 1 entry";

    // Reimport to verify both entities load correctly
    ASSERT_TRUE(MeshImporterExporter::sceneImporter(sceneFile));
    EXPECT_EQ(manager->getSceneNodes().size(), 2);
}

TEST_F(SceneSaveLoadTest, EmptyScene_ExportsValidFile) {
    auto* manager = Manager::getSingleton();
    ASSERT_EQ(manager->getSceneNodes().size(), 0);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString sceneFile = tmpDir.path() + "/empty_scene.scene.gltf";

    int result = MeshImporterExporter::sceneExporter(sceneFile);
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(QFileInfo::exists(sceneFile));
}

TEST_F(SceneSaveLoadTest, SceneExporter_ProgressCallback_ReportsProgress) {
    auto* manager = Manager::getSingleton();

    auto mesh = createInMemoryTriangleMesh("progress_test_mesh");
    auto* sn = manager->addSceneNode("ProgressTestNode");
    manager->createEntity(sn, mesh);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString sceneFile = tmpDir.path() + "/progress_test.scene.gltf";

    std::vector<int> progressValues;
    std::vector<QString> statusMessages;

    int result = MeshImporterExporter::sceneExporter(sceneFile,
        [&](int progress, const QString& status) {
            progressValues.push_back(progress);
            statusMessages.push_back(status);
        });

    EXPECT_EQ(result, 0);

    // Should have received progress updates
    ASSERT_FALSE(progressValues.empty());
    // First progress should be for textures (0-30 range)
    EXPECT_GE(progressValues.front(), 0);
    // Last progress should be 100
    EXPECT_EQ(progressValues.back(), 100);
    // Progress should be non-decreasing
    for (size_t i = 1; i < progressValues.size(); ++i)
        EXPECT_GE(progressValues[i], progressValues[i - 1]);
    // Should have status messages for each phase
    ASSERT_FALSE(statusMessages.empty());
}

TEST_F(SceneSaveLoadTest, RoundTrip_SkeletonEntity_PreservesAnimations) {
    auto* manager = Manager::getSingleton();

    // Create an animated entity with skeleton and "TestAnim" animation
    auto* entity = createAnimatedTestEntity("SceneAnimRT");
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());
    ASSERT_EQ(entity->getMesh()->getSkeleton()->getNumAnimations(), 1);
    EXPECT_EQ(entity->getMesh()->getSkeleton()->getAnimation(static_cast<unsigned short>(0))->getName(), "TestAnim");

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString sceneFile = tmpDir.path() + "/test_anim_roundtrip.scene.gltf";

    int exportResult = MeshImporterExporter::sceneExporter(sceneFile);
    ASSERT_EQ(exportResult, 0);
    ASSERT_TRUE(QFileInfo::exists(sceneFile));

    ASSERT_TRUE(MeshImporterExporter::sceneImporter(sceneFile));

    auto& nodes = manager->getSceneNodes();
    ASSERT_EQ(nodes.size(), 1);

    auto* reimportedNode = nodes.first();
    auto* sceneMgr = manager->getSceneMgr();
    ASSERT_TRUE(sceneMgr->hasEntity(reimportedNode->getName()));

    auto* reimportedEntity = sceneMgr->getEntity(reimportedNode->getName());
    ASSERT_TRUE(reimportedEntity->hasSkeleton());
    auto* skel = reimportedEntity->getMesh()->getSkeleton().get();
    EXPECT_EQ(skel->getNumAnimations(), 1) << "Expected exactly 1 animation after round-trip";
    if (skel->getNumAnimations() > 0)
        EXPECT_EQ(skel->getAnimation(static_cast<unsigned short>(0))->getName(), "TestAnim");
}

TEST_F(SceneSaveLoadTest, SceneExporter_NullProgress_FullExport) {
    auto* manager = Manager::getSingleton();

    auto mesh = createInMemoryTriangleMesh("null_progress_mesh");
    auto* sn = manager->addSceneNode("NullProgressNode");
    manager->createEntity(sn, mesh);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString sceneFile = tmpDir.path() + "/null_progress.scene.gltf";

    // nullptr progress callback should not crash during real export
    int result = MeshImporterExporter::sceneExporter(sceneFile, nullptr);
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(QFileInfo::exists(sceneFile));
}

TEST_F(SceneSaveLoadTest, Exporter_DirectObjExport_WritesModelAndMaterialFiles)
{
    auto* manager = Manager::getSingleton();
    auto mesh = createInMemoryTriangleMesh("direct_obj_mesh");
    auto* sn = manager->addSceneNode("DirectObjNode");
    auto* entity = manager->createEntity(sn, mesh);
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString objFile = tmpDir.path() + "/direct_export.obj";

    const int result = MeshImporterExporter::exporter(sn, objFile, "OBJ (*.obj)");
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(QFileInfo::exists(objFile));
    EXPECT_TRUE(QFileInfo::exists(tmpDir.path() + "/direct_export.material"));
}

TEST_F(SceneSaveLoadTest, ConfigureCameraMovesCameraParentBasedOnEntitySize)
{
    auto* manager = Manager::getSingleton();
    auto mesh = createInMemoryTriangleMesh("camera_config_mesh");
    auto* sn = manager->addSceneNode("CameraConfigNode");
    auto* entity = manager->createEntity(sn, mesh);
    ASSERT_NE(entity, nullptr);

    Ogre::SceneManager* sceneMgr = manager->getSceneMgr();
    Ogre::Camera* camera = nullptr;
    if (sceneMgr->hasCamera("CoverageCamera")) {
        camera = sceneMgr->getCamera("CoverageCamera");
    } else {
        camera = sceneMgr->createCamera("CoverageCamera");
        Ogre::SceneNode* cameraNode = sceneMgr->getRootSceneNode()->createChildSceneNode("CoverageCameraNode");
        cameraNode->attachObject(camera);
    }
    ASSERT_NE(camera, nullptr);
    camera->setFOVy(Ogre::Degree(60));

    MeshImporterExporter::configureCameraForTesting(entity);

    const Ogre::Vector3 cameraPos = camera->getParentSceneNode()->getPosition();
    EXPECT_FLOAT_EQ(cameraPos.x, 0.0f);
    EXPECT_FLOAT_EQ(cameraPos.y, 0.0f);
    EXPECT_LT(cameraPos.z, 0.0f);
}

TEST_F(SceneSaveLoadTest, ConfigureCameraUpdatesAllCameraNodes)
{
    auto* manager = Manager::getSingleton();
    auto mesh = createInMemoryTriangleMesh("camera_config_multi_mesh");
    auto* sn = manager->addSceneNode("CameraConfigMultiNode");
    auto* entity = manager->createEntity(sn, mesh);
    ASSERT_NE(entity, nullptr);

    Ogre::SceneManager* sceneMgr = manager->getSceneMgr();
    Ogre::Camera* cameraA = sceneMgr->hasCamera("CoverageCameraA")
        ? sceneMgr->getCamera("CoverageCameraA")
        : sceneMgr->createCamera("CoverageCameraA");
    Ogre::Camera* cameraB = sceneMgr->hasCamera("CoverageCameraB")
        ? sceneMgr->getCamera("CoverageCameraB")
        : sceneMgr->createCamera("CoverageCameraB");
    ASSERT_NE(cameraA, nullptr);
    ASSERT_NE(cameraB, nullptr);

    if (!cameraA->getParentSceneNode()) {
        auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("CoverageCameraNodeA");
        node->attachObject(cameraA);
    }
    if (!cameraB->getParentSceneNode()) {
        auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("CoverageCameraNodeB");
        node->attachObject(cameraB);
    }

    cameraA->setFOVy(Ogre::Degree(45));
    cameraB->setFOVy(Ogre::Degree(75));
    cameraA->getParentSceneNode()->setPosition(10.0f, 20.0f, 30.0f);
    cameraB->getParentSceneNode()->setPosition(-10.0f, -20.0f, -30.0f);

    MeshImporterExporter::configureCameraForTesting(entity);

    const Ogre::Vector3 posA = cameraA->getParentSceneNode()->getPosition();
    const Ogre::Vector3 posB = cameraB->getParentSceneNode()->getPosition();
    EXPECT_FLOAT_EQ(posA.x, 0.0f);
    EXPECT_FLOAT_EQ(posA.y, 0.0f);
    EXPECT_FLOAT_EQ(posB.x, 0.0f);
    EXPECT_FLOAT_EQ(posB.y, 0.0f);
    EXPECT_LT(posA.z, 0.0f);
    EXPECT_LT(posB.z, 0.0f);
    EXPECT_NE(posA.z, posB.z);
}

TEST_F(SceneSaveLoadTest, SceneExporter_TwoEntitiesReportBothTextureSteps)
{
    auto* manager = Manager::getSingleton();

    auto meshA = createInMemoryTriangleMesh("progress_multi_mesh_a");
    auto* nodeA = manager->addSceneNode("ProgressMultiNodeA");
    ASSERT_NE(manager->createEntity(nodeA, meshA), nullptr);

    auto meshB = createInMemoryTriangleMesh("progress_multi_mesh_b");
    auto* nodeB = manager->addSceneNode("ProgressMultiNodeB");
    ASSERT_NE(manager->createEntity(nodeB, meshB), nullptr);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString sceneFile = tmpDir.path() + "/progress_multi.scene.gltf";

    QStringList statusMessages;
    int result = MeshImporterExporter::sceneExporter(
        sceneFile,
        [&statusMessages](int, const QString& status) {
            statusMessages.append(status);
        });

    EXPECT_EQ(result, 0);
    EXPECT_TRUE(QFileInfo::exists(sceneFile));
    EXPECT_THAT(statusMessages, ::testing::Contains(QStringLiteral("Exporting textures (1/2)...")));
    EXPECT_THAT(statusMessages, ::testing::Contains(QStringLiteral("Exporting textures (2/2)...")));
    ASSERT_FALSE(statusMessages.isEmpty());
    EXPECT_EQ(statusMessages.back(), QStringLiteral("Done."));
}

TEST_F(SceneSaveLoadTest, Exporter_DirectColladaExport_WritesModelAndMaterialFiles)
{
    auto* manager = Manager::getSingleton();
    auto mesh = createInMemoryTriangleMesh("direct_collada_mesh");
    auto* sn = manager->addSceneNode("DirectColladaNode");
    auto* entity = manager->createEntity(sn, mesh);
    ASSERT_NE(entity, nullptr);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString daeFile = tmpDir.path() + "/direct_export.dae";

    const int result = MeshImporterExporter::exporter(sn, daeFile, "Collada (*.dae)");
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(QFileInfo::exists(daeFile));
    EXPECT_TRUE(QFileInfo::exists(tmpDir.path() + "/direct_export.material"));
}

TEST_F(SceneSaveLoadTest, RoundTrip_MixedSkeletalAndNonSkeletal) {
    auto* manager = Manager::getSingleton();

    // Create a skeletal entity
    auto* skelEntity = createAnimatedTestEntity("MixedSkelEntity");
    ASSERT_NE(skelEntity, nullptr);
    ASSERT_TRUE(skelEntity->hasSkeleton());

    // Create a non-skeletal entity
    auto mesh2 = createInMemoryTriangleMesh("mixed_plain_mesh");
    auto* sn2 = manager->addSceneNode("MixedPlainNode");
    manager->createEntity(sn2, mesh2);
    sn2->setPosition(Ogre::Vector3(5.0f, 0.0f, 0.0f));

    ASSERT_EQ(manager->getSceneNodes().size(), 2);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString sceneFile = tmpDir.path() + "/mixed_scene.scene.gltf";

    int exportResult = MeshImporterExporter::sceneExporter(sceneFile);
    ASSERT_EQ(exportResult, 0);

    ASSERT_TRUE(MeshImporterExporter::sceneImporter(sceneFile));
    EXPECT_EQ(manager->getSceneNodes().size(), 2);

    // Verify both entities reimported
    bool foundSkeletal = false, foundPlain = false;
    for (auto* sn : manager->getSceneNodes())
    {
        if (!manager->getSceneMgr()->hasEntity(sn->getName()))
            continue;
        auto* e = manager->getSceneMgr()->getEntity(sn->getName());
        if (e->hasSkeleton())
            foundSkeletal = true;
        else
            foundPlain = true;
    }
    EXPECT_TRUE(foundSkeletal) << "Skeletal entity not found after round-trip";
    EXPECT_TRUE(foundPlain) << "Non-skeletal entity not found after round-trip";
}

TEST_F(SceneSaveLoadTest, RoundTrip_TwoSkeletalEntities_BonePrefixing) {
    auto* manager = Manager::getSingleton();

    // Create two skeletal entities — exercises bone name prefixing
    auto* entityA = createAnimatedTestEntity("SkelEntityA");
    ASSERT_NE(entityA, nullptr);
    ASSERT_TRUE(entityA->hasSkeleton());

    auto* entityB = createAnimatedTestEntity("SkelEntityB");
    ASSERT_NE(entityB, nullptr);
    ASSERT_TRUE(entityB->hasSkeleton());

    // Place them apart
    auto* snA = manager->getSceneNodes().at(0);
    auto* snB = manager->getSceneNodes().at(1);
    snA->setPosition(Ogre::Vector3(-2.0f, 0.0f, 0.0f));
    snB->setPosition(Ogre::Vector3(2.0f, 0.0f, 0.0f));

    ASSERT_EQ(manager->getSceneNodes().size(), 2);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString sceneFile = tmpDir.path() + "/two_skel.scene.gltf";

    int exportResult = MeshImporterExporter::sceneExporter(sceneFile);
    ASSERT_EQ(exportResult, 0);

    ASSERT_TRUE(MeshImporterExporter::sceneImporter(sceneFile));
    EXPECT_EQ(manager->getSceneNodes().size(), 2);

    // Verify both reimported entities have skeletons
    int skelCount = 0;
    for (auto* sn : manager->getSceneNodes())
    {
        if (!manager->getSceneMgr()->hasEntity(sn->getName()))
            continue;
        auto* e = manager->getSceneMgr()->getEntity(sn->getName());
        if (e->hasSkeleton())
            ++skelCount;
    }
    EXPECT_EQ(skelCount, 2) << "Both skeletal entities should survive round-trip";
}

TEST_F(SceneSaveLoadTest, Exporter_TexturedColladaExportWritesConvertedTextureFiles)
{
    auto* manager = Manager::getSingleton();

    auto mesh = createInMemoryTriangleMesh("textured_collada_mesh");
    auto* sn = manager->addSceneNode("TexturedColladaNode");
    auto* entity = manager->createEntity(sn, mesh);
    ASSERT_NE(entity, nullptr);

    Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().create(
        "TexturedColladaMaterial",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* pass = mat->getTechnique(0)->getPass(0);

    ASSERT_TRUE(static_cast<bool>(createSolidTexture2D("textured_collada_diffuse.jpg", 0xFF00AAFF)));
    ASSERT_TRUE(static_cast<bool>(createSolidTexture2D("textured_collada_normal.png", 0xFF8080FF)));

    pass->createTextureUnitState("textured_collada_diffuse.jpg");
    auto* normalTus = pass->createTextureUnitState("textured_collada_normal.png");
    normalTus->setName("normal_map");

    entity->setMaterialName(mat->getName());

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString daeFile = tmpDir.path() + "/textured_export.dae";

    const int result = MeshImporterExporter::exporter(sn, daeFile, "Collada (*.dae)");
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(QFileInfo::exists(daeFile));
    const QString materialFilePath = tmpDir.path() + "/textured_export.material";
    EXPECT_TRUE(QFileInfo::exists(materialFilePath));

    QFile materialFile(materialFilePath);
    ASSERT_TRUE(materialFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString materialText = QString::fromUtf8(materialFile.readAll());
    EXPECT_TRUE(materialText.contains("textured_collada_diffuse.jpg"));
    EXPECT_TRUE(materialText.contains("textured_collada_normal.png"));
}

TEST_F(SceneSaveLoadTest, Exporter_ObjFromSkeletalEntitySucceeds)
{
    auto* entity = createAnimatedTestEntity("skeletal_obj_export_entity");
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());

    auto* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString objFile = tmpDir.path() + "/skeletal_export.obj";

    const int result = MeshImporterExporter::exporter(node, objFile, "OBJ (*.obj)");
    EXPECT_EQ(result, 0);
    EXPECT_TRUE(QFileInfo::exists(objFile));
    EXPECT_TRUE(QFileInfo::exists(tmpDir.path() + "/skeletal_export.material"));
}
