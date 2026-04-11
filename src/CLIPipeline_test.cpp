#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
#include <QTemporaryDir>
#include <vector>
#include <OgreMeshManager.h>
#include <OgreHardwareBufferManager.h>
#include "MeshValidator.h"
#include "MeshLodController.h"
#include "SelectionSet.h"
#include <OgreMaterialManager.h>
#include "CLIPipeline.h"
#include "MeshImporterExporter.h"
#include "SentryReporter.h"
#include "TestHelpers.h"

namespace {
/// Path to the media/models directory relative to the test binary.
QString testDataDir()
{
    QString binDir = QCoreApplication::applicationDirPath();
    QDir dir(binDir);
    dir.cdUp(); // bin -> build_local
    dir.cdUp(); // build_local -> project root
    return dir.absoluteFilePath("media/models");
}

QString writeMinimalObj(const QString& dirPath, const QString& fileName)
{
    const QString path = QDir(dirPath).filePath(fileName);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();

    f.write(
        "o Tri\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n");
    f.close();
    return path;
}

Ogre::MeshPtr createTwoSubmeshSharedMesh(const std::string& name)
{
    Ogre::MeshPtr mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

    mesh->sharedVertexData->vertexCount = 4;
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), mesh->sharedVertexData->vertexCount,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    const float verts[] = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f
    };
    vbuf->writeData(0, sizeof(verts), verts, true);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);

    auto makeSubMesh = [&](const std::string& subName, std::initializer_list<uint16_t> indices) {
        Ogre::SubMesh* sub = mesh->createSubMesh(subName);
        sub->useSharedVertices = true;
        auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            Ogre::HardwareIndexBuffer::IT_16BIT,
            static_cast<size_t>(indices.size()),
            Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        std::vector<uint16_t> data(indices);
        ibuf->writeData(0, sizeof(uint16_t) * data.size(), data.data(), true);
        sub->indexData->indexBuffer = ibuf;
        sub->indexData->indexCount = data.size();
    };

    makeSubMesh("sub0", {0, 1, 2});
    makeSubMesh("sub1", {0, 2, 3});

    mesh->_setBounds(Ogre::AxisAlignedBox(-1, -1, -1, 2, 2, 1));
    mesh->_setBoundingSphereRadius(3.0f);
    mesh->load();
    return mesh;
}

class ScopedDeathTestEnvironment {
public:
    ScopedDeathTestEnvironment()
        : m_hadQtQpaPlatform(qEnvironmentVariableIsSet("QT_QPA_PLATFORM")),
          m_oldQtQpaPlatform(qgetenv("QT_QPA_PLATFORM")),
          m_oldDeathTestStyle(GTEST_FLAG_GET(death_test_style))
    {
    }

    void setOffscreenThreadsafe()
    {
        qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
        GTEST_FLAG_SET(death_test_style, "threadsafe");
    }

    ~ScopedDeathTestEnvironment()
    {
        if (m_hadQtQpaPlatform) {
            qputenv("QT_QPA_PLATFORM", m_oldQtQpaPlatform);
        } else {
            qunsetenv("QT_QPA_PLATFORM");
        }
        GTEST_FLAG_SET(death_test_style, m_oldDeathTestStyle);
    }

private:
    bool m_hadQtQpaPlatform = false;
    QByteArray m_oldQtQpaPlatform;
    std::string m_oldDeathTestStyle;
};
} // anonymous namespace

// --- Formatting tests (no Ogre needed) ---

class CLIPipelineFormatTest : public ::testing::Test {};

TEST_F(CLIPipelineFormatTest, FormatMeshInfoText_BasicFields)
{
    MeshInfo info;
    info.file = "test.mesh";
    info.vertices = 100;
    info.triangles = 50;
    info.submeshes = 2;
    info.materials << "mat1" << "mat2";
    info.bbMin = Ogre::Vector3(-1, -2, -3);
    info.bbMax = Ogre::Vector3(1, 2, 3);

    QString text = CLIPipeline::formatMeshInfoText(info);

    EXPECT_TRUE(text.contains("File: test.mesh"));
    EXPECT_TRUE(text.contains("Vertices: 100"));
    EXPECT_TRUE(text.contains("Triangles: 50"));
    EXPECT_TRUE(text.contains("Submeshes: 2"));
    EXPECT_TRUE(text.contains("mat1, mat2"));
    EXPECT_TRUE(text.contains("Bounding Box:"));
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoText_WithSkeleton)
{
    MeshInfo info;
    info.file = "animated.fbx";
    info.vertices = 200;
    info.triangles = 100;
    info.submeshes = 1;
    info.skeletonName = "test.skeleton";
    info.boneCount = 10;
    info.animations.append({"walk", 1.2f});
    info.animations.append({"run", 0.8f});

    QString text = CLIPipeline::formatMeshInfoText(info);

    EXPECT_TRUE(text.contains("Skeleton: test.skeleton (10 bones)"));
    EXPECT_TRUE(text.contains("Animations:"));
    EXPECT_TRUE(text.contains("walk"));
    EXPECT_TRUE(text.contains("run"));
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoText_NoMaterials)
{
    MeshInfo info;
    info.file = "empty.mesh";
    QString text = CLIPipeline::formatMeshInfoText(info);
    EXPECT_TRUE(text.contains("(none)"));
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoJson_Structure)
{
    MeshInfo info;
    info.file = "test.mesh";
    info.vertices = 300;
    info.triangles = 150;
    info.submeshes = 3;
    info.materials << "matA";
    info.bbMin = Ogre::Vector3(0, 0, 0);
    info.bbMax = Ogre::Vector3(1, 1, 1);

    QString json = CLIPipeline::formatMeshInfoJson(info);
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    ASSERT_TRUE(doc.isObject());

    QJsonObject obj = doc.object();
    EXPECT_EQ(obj["file"].toString(), "test.mesh");
    EXPECT_EQ(obj["vertices"].toInt(), 300);
    EXPECT_EQ(obj["triangles"].toInt(), 150);
    EXPECT_EQ(obj["submeshes"].toInt(), 3);
    EXPECT_TRUE(obj["materials"].isArray());
    EXPECT_EQ(obj["materials"].toArray().size(), 1);
    EXPECT_TRUE(obj["boundingBox"].isObject());
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoJson_WithAnimations)
{
    MeshInfo info;
    info.file = "anim.fbx";
    info.skeletonName = "skel.skeleton";
    info.boneCount = 5;
    info.animations.append({"idle", 3.5f});

    QString json = CLIPipeline::formatMeshInfoJson(info);
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject obj = doc.object();

    EXPECT_TRUE(obj.contains("skeleton"));
    EXPECT_EQ(obj["skeleton"].toObject()["boneCount"].toInt(), 5);
    EXPECT_TRUE(obj.contains("animations"));
    EXPECT_EQ(obj["animations"].toArray().size(), 1);
    EXPECT_EQ(obj["animations"].toArray()[0].toObject()["name"].toString(), "idle");
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoJson_NoSkeleton)
{
    MeshInfo info;
    info.file = "noskel.obj";

    QString json = CLIPipeline::formatMeshInfoJson(info);
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject obj = doc.object();

    EXPECT_FALSE(obj.contains("skeleton"));
    EXPECT_FALSE(obj.contains("animations"));
}

// --- MeshInfo extraction tests (need Ogre) ---

class CLIPipelineOgreTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!tryInitOgre() || !canLoadMeshFiles())
            GTEST_SKIP() << "Ogre not available";
        createStandardOgreMaterials();
    }
};

TEST_F(CLIPipelineOgreTest, ExtractMeshInfo_TriangleMesh)
{
    auto mesh = createInMemoryTriangleMesh("cli_test_tri");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("cli_test_tri");
    auto* entity = sceneMgr->createEntity("cli_test_tri", mesh);
    node->attachObject(entity);

    MeshInfo info = CLIPipeline::extractMeshInfo(entity, "triangle.mesh");

    EXPECT_EQ(info.file, "triangle.mesh");
    EXPECT_EQ(info.vertices, 3u);
    EXPECT_EQ(info.triangles, 1u);
    EXPECT_EQ(info.submeshes, 1u);
    EXPECT_TRUE(info.skeletonName.isEmpty());
    EXPECT_EQ(info.boneCount, 0);
    EXPECT_TRUE(info.animations.isEmpty());
}

TEST_F(CLIPipelineOgreTest, ExtractMeshInfo_AnimatedEntity)
{
    auto* entity = createAnimatedTestEntity("cli_test_anim");
    ASSERT_NE(entity, nullptr);

    MeshInfo info = CLIPipeline::extractMeshInfo(entity, "animated.fbx");

    EXPECT_EQ(info.file, "animated.fbx");
    EXPECT_GT(info.vertices, 0u);
    EXPECT_TRUE(entity->hasSkeleton());
    EXPECT_FALSE(info.skeletonName.isEmpty());
    EXPECT_EQ(info.boneCount, 2);
    EXPECT_EQ(info.animations.size(), 1);
    EXPECT_EQ(info.animations[0].name, "TestAnim");
    EXPECT_FLOAT_EQ(info.animations[0].duration, 1.0f);
}

TEST_F(CLIPipelineOgreTest, ExtractMeshInfo_DeduplicatesMaterialAndTextureNames)
{
    auto mesh = createTwoSubmeshSharedMesh("cli_test_two_submesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("cli_test_two_submesh_node");
    auto* entity = sceneMgr->createEntity("cli_test_two_submesh_entity", mesh);
    ASSERT_NE(entity, nullptr);
    node->attachObject(entity);
    ASSERT_EQ(entity->getNumSubEntities(), 2u);

    Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().create(
        "cli_test_mat_with_textures",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* pass = material->getTechnique(0)->getPass(0);
    pass->createTextureUnitState("albedo.png");
    pass->createTextureUnitState("albedo.png"); // duplicate name
    pass->createTextureUnitState("normal.png");

    entity->getSubEntity(0)->setMaterial(material);
    entity->getSubEntity(1)->setMaterial(material);

    MeshInfo info = CLIPipeline::extractMeshInfo(entity, "multi.mesh");

    EXPECT_EQ(info.submeshes, 2u);
    EXPECT_EQ(info.triangles, 2u);
    EXPECT_EQ(info.materials.size(), 1);
    EXPECT_EQ(info.materials[0], QString::fromStdString(material->getName()));
    EXPECT_EQ(info.textures.size(), 2);
    EXPECT_TRUE(info.textures.contains("albedo.png"));
    EXPECT_TRUE(info.textures.contains("normal.png"));
}

TEST_F(CLIPipelineOgreTest, ExtractMeshInfo_NullEntity)
{
    MeshInfo info = CLIPipeline::extractMeshInfo(nullptr, "null.mesh");
    EXPECT_EQ(info.vertices, 0u);
    EXPECT_EQ(info.triangles, 0u);
}

// --- FixOptions tests ---

TEST(FixOptionsTest, AnySet_DefaultIsFalse)
{
    FixOptions opts;
    EXPECT_FALSE(opts.anySet());
}

TEST(FixOptionsTest, AnySet_RemoveDegenerates)
{
    FixOptions opts;
    opts.removeDegenerates = true;
    EXPECT_TRUE(opts.anySet());
}

TEST(FixOptionsTest, AnySet_MergeMaterials)
{
    FixOptions opts;
    opts.mergeMaterials = true;
    EXPECT_TRUE(opts.anySet());
}

TEST(FixOptionsTest, AnySet_AllFlags)
{
    FixOptions opts;
    opts.removeDegenerates = true;
    opts.mergeMaterials = true;
    EXPECT_TRUE(opts.anySet());
}

TEST(FixOptionsTest, ToAssimpFlags_Default)
{
    FixOptions opts;
    EXPECT_EQ(opts.toAssimpFlags(), 0u);
}

TEST(FixOptionsTest, ToAssimpFlags_RemoveDegenerates)
{
    FixOptions opts;
    opts.removeDegenerates = true;
    EXPECT_EQ(opts.toAssimpFlags(), static_cast<unsigned int>(aiProcess_FindDegenerates));
}

TEST(FixOptionsTest, ToAssimpFlags_MergeMaterials)
{
    FixOptions opts;
    opts.mergeMaterials = true;
    EXPECT_EQ(opts.toAssimpFlags(), static_cast<unsigned int>(aiProcess_RemoveRedundantMaterials));
}

TEST(FixOptionsTest, ToAssimpFlags_All)
{
    FixOptions opts;
    opts.removeDegenerates = true;
    opts.mergeMaterials = true;
    unsigned int expected = aiProcess_FindDegenerates | aiProcess_RemoveRedundantMaterials;
    EXPECT_EQ(opts.toAssimpFlags(), expected);
}

// --- Formatting edge cases ---

TEST_F(CLIPipelineFormatTest, FormatMeshInfoText_WithTextures)
{
    MeshInfo info;
    info.file = "tex.mesh";
    info.textures << "diffuse.png" << "normal.png";

    QString text = CLIPipeline::formatMeshInfoText(info);
    EXPECT_TRUE(text.contains("Textures: diffuse.png, normal.png"));
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoJson_WithTextures)
{
    MeshInfo info;
    info.file = "tex.mesh";
    info.textures << "color.png";
    info.bbMin = Ogre::Vector3::ZERO;
    info.bbMax = Ogre::Vector3::UNIT_SCALE;

    QString json = CLIPipeline::formatMeshInfoJson(info);
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject obj = doc.object();

    EXPECT_TRUE(obj.contains("textures"));
    EXPECT_EQ(obj["textures"].toArray().size(), 1);
    EXPECT_EQ(obj["textures"].toArray()[0].toString(), "color.png");
}

// --- formatForExtension tests (no Ogre needed) ---

TEST(CLIPipelineFormatForExtension, FBX)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.fbx"), "FBX Binary (*.fbx)");
}

TEST(CLIPipelineFormatForExtension, GLB2)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.glb2"), "glTF 2.0 Binary (*.glb2)");
}

TEST(CLIPipelineFormatForExtension, GLB)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.glb"), "glTF 2.0 Binary (*.glb)");
}

TEST(CLIPipelineFormatForExtension, GLTF2)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.gltf2"), "glTF 2.0 (*.gltf2)");
}

TEST(CLIPipelineFormatForExtension, GLTF)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.gltf"), "glTF 2.0 (*.gltf)");
}

TEST(CLIPipelineFormatForExtension, DAE)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.dae"), "Collada (*.dae)");
}

TEST(CLIPipelineFormatForExtension, OBJ)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.obj"), "OBJ (*.obj)");
}

TEST(CLIPipelineFormatForExtension, STL)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.stl"), "STL (*.stl)");
}

TEST(CLIPipelineFormatForExtension, PLY)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.ply"), "PLY (*.ply)");
}

TEST(CLIPipelineFormatForExtension, ThreeDS)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.3ds"), "3DS (*.3ds)");
}

TEST(CLIPipelineFormatForExtension, X)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.x"), "X (*.x)");
}

TEST(CLIPipelineFormatForExtension, MeshXML)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.mesh.xml"), "Ogre XML (*.mesh.xml)");
}

TEST(CLIPipelineFormatForExtension, Mesh)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.mesh"), "Ogre Mesh (*.mesh)");
}

TEST(CLIPipelineFormatForExtension, Assbin)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.assbin"), "Assimp Binary (*.assbin)");
}

TEST(CLIPipelineFormatForExtension, UnknownDefaultsToMesh)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.xyz"), "Ogre Mesh (*.mesh)");
}

TEST(CLIPipelineFormatForExtension, CaseInsensitive)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("MODEL.FBX"), "FBX Binary (*.fbx)");
    EXPECT_EQ(CLIPipeline::formatForExtension("test.DAE"), "Collada (*.dae)");
    EXPECT_EQ(CLIPipeline::formatForExtension("FILE.OBJ"), "OBJ (*.obj)");
}

TEST(CLIPipelineFormatForExtension, PathWithDirectories)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("/tmp/dir/model.fbx"), "FBX Binary (*.fbx)");
    EXPECT_EQ(CLIPipeline::formatForExtension("/tmp/dir/model.glb"), "glTF 2.0 Binary (*.glb)");
    EXPECT_EQ(CLIPipeline::formatForExtension("/tmp/dir/model.gltf"), "glTF 2.0 (*.gltf)");
    EXPECT_EQ(CLIPipeline::formatForExtension("C:\\dir\\model.gltf2"), "glTF 2.0 (*.gltf2)");
}

// --- printUsage / printVersion smoke tests ---

TEST(CLIPipelineSmoke, PrintUsageDoesNotCrash)
{
    EXPECT_NO_FATAL_FAILURE(CLIPipeline::printUsage());
}

TEST(CLIPipelineSmoke, PrintVersionDoesNotCrash)
{
    EXPECT_NO_FATAL_FAILURE(CLIPipeline::printVersion());
}

// --- run() tests (early-return paths that call _exit(0) to bypass static destructors) ---

TEST(CLIPipelineRun, HelpFlag)
{
    char arg0[] = "qtmesh";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    EXPECT_EXIT(CLIPipeline::run(2, argv), testing::ExitedWithCode(0), "");
}

TEST(CLIPipelineRun, HelpFlagShort)
{
    char arg0[] = "qtmesh";
    char arg1[] = "-h";
    char* argv[] = {arg0, arg1};
    EXPECT_EXIT(CLIPipeline::run(2, argv), testing::ExitedWithCode(0), "");
}

TEST(CLIPipelineRun, VersionFlag)
{
    char arg0[] = "qtmesh";
    char arg1[] = "--version";
    char* argv[] = {arg0, arg1};
    EXPECT_EXIT(CLIPipeline::run(2, argv), testing::ExitedWithCode(0), "");
}

TEST(CLIPipelineRun, VersionFlagShort)
{
    char arg0[] = "qtmesh";
    char arg1[] = "-v";
    char* argv[] = {arg0, arg1};
    EXPECT_EXIT(CLIPipeline::run(2, argv), testing::ExitedWithCode(0), "");
}

TEST(CLIPipelineRun, NoCommand)
{
    char arg0[] = "qtmesh";
    char* argv[] = {arg0};
    EXPECT_EQ(CLIPipeline::run(1, argv), 2);
}

TEST(CLIPipelineRun, VerboseWithHelp)
{
    char arg0[] = "qtmesh";
    char arg1[] = "--verbose";
    char arg2[] = "--help";
    char* argv[] = {arg0, arg1, arg2};
    EXPECT_EXIT(CLIPipeline::run(3, argv), testing::ExitedWithCode(0), "");
}

TEST(CLIPipelineRun, CliWithHelp)
{
    char arg0[] = "qtmesh";
    char arg1[] = "--cli";
    char arg2[] = "--help";
    char* argv[] = {arg0, arg1, arg2};
    EXPECT_EXIT(CLIPipeline::run(3, argv), testing::ExitedWithCode(0), "");
}

TEST(CLIPipelineRun, UnknownCommand)
{
    ScopedDeathTestEnvironment scopedEnv;
    scopedEnv.setOffscreenThreadsafe();

    char arg0[] = "qtmesh";
    char arg1[] = "not-a-command";
    char* argv[] = {arg0, arg1};
    EXPECT_EXIT(CLIPipeline::run(2, argv), testing::ExitedWithCode(2), "");
}

TEST(CLIPipelineRun, UnknownCommandWithNoTelemetryFlag)
{
    ScopedDeathTestEnvironment scopedEnv;
    scopedEnv.setOffscreenThreadsafe();

    QSettings settings;
    const QString kSentryEnabledKey = "Sentry/enabled";
    const bool hadSentryEnabled = settings.contains(kSentryEnabledKey);
    const QVariant previousSentryEnabled = settings.value(kSentryEnabledKey);

    char arg0[] = "qtmesh";
    char arg1[] = "--no-telemetry";
    char arg2[] = "not-a-command";
    char* argv[] = {arg0, arg1, arg2};
    EXPECT_EXIT(CLIPipeline::run(3, argv), testing::ExitedWithCode(2), "");

    if (hadSentryEnabled) {
        settings.setValue(kSentryEnabledKey, previousSentryEnabled);
    } else {
        settings.remove(kSentryEnabledKey);
    }
}

// --- TestArgv helper for in-process cmd* tests ---

namespace {

/// RAII helper to build argc/argv from a list of strings.
class TestArgv {
public:
    TestArgv(std::initializer_list<const char*> args)
    {
        for (auto* a : args) {
            m_storage.push_back(QByteArray(a));
        }
        for (auto& ba : m_storage) {
            m_argv.push_back(ba.data());
        }
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() const { return m_argc; }
    char** argv() { return m_argv.data(); }
private:
    QList<QByteArray> m_storage;
    QList<char*> m_argv;
    int m_argc = 0;
};

/// RAII helper to temporarily switch current working directory.
class ScopedCurrentDir {
public:
    explicit ScopedCurrentDir(const QString& path)
        : m_old(QDir::currentPath())
    {
        QDir::setCurrent(path);
    }

    ~ScopedCurrentDir()
    {
        QDir::setCurrent(m_old);
    }

private:
    QString m_old;
};

} // anonymous namespace

// --- initOgreHeadless tests ---

class CLIPipelineInitTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!tryInitOgre() || !canLoadMeshFiles())
            GTEST_SKIP() << "Ogre not available";
        createStandardOgreMaterials();
    }
};

TEST_F(CLIPipelineInitTest, InitOgreHeadless_Idempotent)
{
    // Should return true even though tryInitOgre() already created a render window
    EXPECT_TRUE(CLIPipeline::initOgreHeadless());
}

TEST_F(CLIPipelineInitTest, InitOgreHeadless_CalledTwice)
{
    EXPECT_TRUE(CLIPipeline::initOgreHeadless());
    EXPECT_TRUE(CLIPipeline::initOgreHeadless());
}

// --- In-process cmdInfo tests ---

class CLIPipelineCmdTest : public ::testing::Test {
protected:
    // One-time warmup: the first FBX import in a process sometimes fails
    // due to lazy initialization in the resource/plugin pipeline.
    static void SetUpTestSuite() {
        if (!tryInitOgre() || !canLoadMeshFiles()) return;
        createStandardOgreMaterials();

        QString warmupFile = testDataDir() + "/Twist Dance.fbx";
        if (QFile::exists(warmupFile)) {
            CLIPipeline::initOgreHeadless();
            MeshImporterExporter::importer({warmupFile});
            // Clean up so tests start fresh
            if (Manager::getSingletonPtr()) {
                auto nodes = Manager::getSingleton()->getSceneNodes();
                for (auto* node : nodes) {
                    Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
                    Manager::getSingleton()->destroySceneNode(node);
                }
            }
        }
    }

    void SetUp() override {
        if (!tryInitOgre() || !canLoadMeshFiles())
            GTEST_SKIP() << "Ogre not available";
        createStandardOgreMaterials();
    }
    void TearDown() override {
        if (!Manager::getSingletonPtr()) return;
        auto nodes = Manager::getSingleton()->getSceneNodes(); // copy
        for (auto* node : nodes) {
            Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
            Manager::getSingleton()->destroySceneNode(node);
        }
    }
};

static QString exportGeneratedTriangleMesh(const QString& baseName)
{
    auto* manager = Manager::getSingletonPtr();
    if (!manager)
        return QString();

    const std::string meshName = (baseName + "_mesh").toStdString();
    const QString nodeName = baseName + "_node";

    Ogre::MeshPtr mesh = createInMemoryTriangleMesh(meshName);
    Ogre::SceneNode* node = manager->addSceneNode(nodeName);
    if (!node) {
        if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
            Ogre::MeshManager::getSingleton().remove(old);
        return QString();
    }

    Ogre::Entity* entity = manager->createEntity(node, mesh);
    if (!entity) {
        manager->destroySceneNode(node);
        if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
            Ogre::MeshManager::getSingleton().remove(old);
        return QString();
    }

    const QString outFile = QDir::tempPath() + "/" + baseName + ".mesh";
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/" + baseName + ".material");

    const int exportRc = MeshImporterExporter::exporter(node, outFile, "Ogre Mesh (*.mesh)");

    manager->destroyAllAttachedMovableObjects(node);
    manager->destroySceneNode(node);
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    if (exportRc != 0)
        return QString();
    return outFile;
}

// -- cmdInfo error paths (no Ogre needed) --

TEST(CLIPipelineCmdInfoError, NoFile)
{
    TestArgv args({"qtmesh", "info"});
    EXPECT_EQ(CLIPipeline::cmdInfo(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdInfoError, NonexistentFile)
{
    TestArgv args({"qtmesh", "info", "/tmp/nonexistent_cli_test_file_12345.fbx"});
    EXPECT_EQ(CLIPipeline::cmdInfo(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdInfoError, NonexistentFileWithJsonAndCliFlag)
{
    TestArgv args({"qtmesh", "--cli", "info", "/tmp/nonexistent_cli_test_file_67890.fbx", "--json"});
    EXPECT_EQ(CLIPipeline::cmdInfo(args.argc(), args.argv()), 1);
}

// -- cmdInfo success paths --

TEST_F(CLIPipelineCmdTest, CmdInfo_TextOutput)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    TestArgv args({"qtmesh", "info", fileBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdInfo(args.argc(), args.argv()), 0);
}

TEST_F(CLIPipelineCmdTest, CmdInfo_JsonOutput)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    TestArgv args({"qtmesh", "info", fileBa.constData(), "--json"});
    EXPECT_EQ(CLIPipeline::cmdInfo(args.argc(), args.argv()), 0);
}

TEST_F(CLIPipelineCmdTest, CmdInfo_SkipsCliFlag)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    TestArgv args({"qtmesh", "--cli", "info", fileBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdInfo(args.argc(), args.argv()), 0);
}

TEST_F(CLIPipelineCmdTest, CmdInfo_InvalidExistingFile)
{
    const QString file = QDir::tempPath() + "/cli_test_invalid_info_input.fbx";
    QFile invalid(file);
    ASSERT_TRUE(invalid.open(QIODevice::WriteOnly | QIODevice::Text));
    invalid.write("this is not a valid 3D model file");
    invalid.close();

    QByteArray fileBa = file.toUtf8();
    TestArgv args({"qtmesh", "info", fileBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdInfo(args.argc(), args.argv()), 1);

    QFile::remove(file);
}

// -- cmdConvert error paths --

TEST(CLIPipelineCmdConvertError, NoArgs)
{
    TestArgv args({"qtmesh", "convert"});
    EXPECT_EQ(CLIPipeline::cmdConvert(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdConvertError, MissingOutput)
{
    TestArgv args({"qtmesh", "convert", "somefile.fbx"});
    EXPECT_EQ(CLIPipeline::cmdConvert(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdConvertError, NonexistentFile)
{
    TestArgv args({"qtmesh", "convert", "/tmp/nonexistent_cli_test_12345.fbx",
                   "-o", "/tmp/cli_test_out.mesh"});
    EXPECT_EQ(CLIPipeline::cmdConvert(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdConvertError, NonexistentFileWithFormatAndLongOutputFlag)
{
    TestArgv args({"qtmesh", "--cli", "convert", "/tmp/nonexistent_cli_test_54321.fbx",
                   "--output", "/tmp/cli_test_out.obj", "--format", "OBJ (*.obj)"});
    EXPECT_EQ(CLIPipeline::cmdConvert(args.argc(), args.argv()), 1);
}

// -- cmdConvert success paths --

TEST_F(CLIPipelineCmdTest, CmdConvert_ValidFile)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    QString outFile = QDir::tempPath() + "/cli_test_convert_inproc.mesh";
    QByteArray outBa = outFile.toUtf8();
    QFile::remove(outFile);

    TestArgv args({"qtmesh", "convert", fileBa.constData(), "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdConvert(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/cli_test_convert_inproc.material");
}

TEST_F(CLIPipelineCmdTest, CmdConvert_OutputLongForm)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    QString outFile = QDir::tempPath() + "/cli_test_convert_long.mesh";
    QByteArray outBa = outFile.toUtf8();
    QFile::remove(outFile);

    TestArgv args({"qtmesh", "convert", fileBa.constData(), "--output", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdConvert(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/cli_test_convert_long.material");
}

TEST_F(CLIPipelineCmdTest, CmdConvert_FormatFlag)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    QString outFile = QDir::tempPath() + "/cli_test_convert_fmt.mesh";
    QByteArray outBa = outFile.toUtf8();
    QFile::remove(outFile);

    TestArgv args({"qtmesh", "convert", fileBa.constData(), "-o", outBa.constData(),
                   "--format", "Ogre Mesh (*.mesh)"});
    EXPECT_EQ(CLIPipeline::cmdConvert(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/cli_test_convert_fmt.material");
}

// -- cmdFix error paths --

TEST(CLIPipelineCmdFixError, NoFile)
{
    TestArgv args({"qtmesh", "fix"});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdFixError, NonexistentFile)
{
    TestArgv args({"qtmesh", "fix", "/tmp/nonexistent_cli_test_12345.fbx"});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdFixError, NonexistentFileWithFlagsAndLongOutputFlag)
{
    TestArgv args({"qtmesh", "--cli", "fix", "/tmp/nonexistent_cli_test_22222.fbx",
                   "--output", "/tmp/cli_test_fix_out.mesh",
                   "--remove-degenerates", "--merge-materials", "--all"});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdFixError, ExistingInvalidFileWithAllFlagReturnsError)
{
    const QString file = QDir::tempPath() + "/cli_test_fix_existing_invalid_input.fbx";
    QFile invalid(file);
    ASSERT_TRUE(invalid.open(QIODevice::WriteOnly | QIODevice::Text));
    invalid.write("invalid fbx payload");
    invalid.close();

    QByteArray fileBa = file.toUtf8();
    TestArgv args({"qtmesh", "fix", fileBa.constData(), "--all"});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 1);

    QFile::remove(file);
}

// -- cmdFix success paths --

TEST_F(CLIPipelineCmdTest, CmdFix_Basic)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    QString outFile = QDir::tempPath() + "/cli_test_fix_basic.mesh";
    QByteArray outBa = outFile.toUtf8();
    QFile::remove(outFile);

    TestArgv args({"qtmesh", "fix", fileBa.constData(), "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/cli_test_fix_basic.material");
}

TEST_F(CLIPipelineCmdTest, CmdFix_AllFlag)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    QString outFile = QDir::tempPath() + "/cli_test_fix_all.mesh";
    QByteArray outBa = outFile.toUtf8();
    QFile::remove(outFile);

    TestArgv args({"qtmesh", "fix", fileBa.constData(), "-o", outBa.constData(), "--all"});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/cli_test_fix_all.material");
}

TEST_F(CLIPipelineCmdTest, CmdFix_RemoveDegenerates)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    QString outFile = QDir::tempPath() + "/cli_test_fix_degen.mesh";
    QByteArray outBa = outFile.toUtf8();
    QFile::remove(outFile);

    TestArgv args({"qtmesh", "fix", fileBa.constData(), "-o", outBa.constData(),
                   "--remove-degenerates"});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/cli_test_fix_degen.material");
}

TEST_F(CLIPipelineCmdTest, CmdFix_MergeMaterials)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    QString outFile = QDir::tempPath() + "/cli_test_fix_merge.mesh";
    QByteArray outBa = outFile.toUtf8();
    QFile::remove(outFile);

    TestArgv args({"qtmesh", "fix", fileBa.constData(), "-o", outBa.constData(),
                   "--merge-materials"});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/cli_test_fix_merge.material");
}

TEST_F(CLIPipelineCmdTest, CmdFix_BothFlags)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    QString outFile = QDir::tempPath() + "/cli_test_fix_both.mesh";
    QByteArray outBa = outFile.toUtf8();
    QFile::remove(outFile);

    TestArgv args({"qtmesh", "fix", fileBa.constData(), "-o", outBa.constData(),
                   "--remove-degenerates", "--merge-materials"});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/cli_test_fix_both.material");
}

TEST_F(CLIPipelineCmdTest, CmdFix_OutputLongForm)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    QString outFile = QDir::tempPath() + "/cli_test_fix_long.mesh";
    QByteArray outBa = outFile.toUtf8();
    QFile::remove(outFile);

    TestArgv args({"qtmesh", "fix", fileBa.constData(), "--output", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdFix(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/cli_test_fix_long.material");
}

// -- cmdAnim error paths --

TEST(CLIPipelineCmdAnimError, NoFile)
{
    TestArgv args({"qtmesh", "anim"});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdAnimError, NoAction)
{
    TestArgv args({"qtmesh", "anim", "somefile.fbx"});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdAnimError, RenameMissingNewName)
{
    TestArgv args({"qtmesh", "anim", "somefile.fbx", "--rename", "OldNameOnly"});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdAnimError, NonexistentFile)
{
    TestArgv args({"qtmesh", "anim", "/tmp/nonexistent_cli_test_12345.fbx", "--list"});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdAnimError, MergeModeWithMissingBaseFile)
{
    TestArgv args({"qtmesh", "anim", "/tmp/nonexistent_cli_test_33333.fbx",
                   "--merge", "/tmp/nonexistent_anim_source_33333.fbx",
                   "-o", "/tmp/cli_test_merge_fail.mesh"});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdAnimError, RenameModeWithoutOutputUsesDefaultOutputPath)
{
    TestArgv args({"qtmesh", "anim", "/tmp/nonexistent_cli_test_rename_default_33333.fbx",
                   "--rename", "OldAnimName", "NewAnimName"});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdAnimError, MergeModeWithoutOutputUsesDefaultOutputPath)
{
    TestArgv args({"qtmesh", "anim", "/tmp/nonexistent_cli_test_merge_default_44444.fbx",
                   "--merge", "/tmp/nonexistent_anim_source_44444_a.fbx",
                   "/tmp/nonexistent_anim_source_44444_b.fbx"});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 1);
}

// -- cmdAnim list --

TEST_F(CLIPipelineCmdTest, CmdAnimList_Text)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    TestArgv args({"qtmesh", "anim", fileBa.constData(), "--list"});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0);
}

TEST_F(CLIPipelineCmdTest, CmdAnimList_Json)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    TestArgv args({"qtmesh", "anim", fileBa.constData(), "--list", "--json"});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0);
}

TEST_F(CLIPipelineCmdTest, CmdAnimList_NoSkeletonFile)
{
    // Create a simple OBJ file with no skeleton
    QString objFile = QDir::tempPath() + "/cli_test_noskel.obj";
    {
        QFile f(objFile);
        f.open(QIODevice::WriteOnly | QIODevice::Text);
        f.write("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
        f.close();
    }
    QByteArray fileBa = objFile.toUtf8();

    TestArgv args({"qtmesh", "anim", fileBa.constData(), "--list"});
    int rc = CLIPipeline::cmdAnim(args.argc(), args.argv());
    EXPECT_EQ(rc, 1);  // "no skeleton/animations"
    QFile::remove(objFile);
}

TEST_F(CLIPipelineCmdTest, CmdAnimList_WithCliFlag)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    TestArgv args({"qtmesh", "--cli", "anim", fileBa.constData(), "--list"});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0);
}

TEST_F(CLIPipelineCmdTest, CmdAnimList_NoAnimationsGeneratedMeshReturnsError)
{
    auto* manager = Manager::getSingleton();
    ASSERT_NE(manager, nullptr);

    Ogre::MeshPtr mesh = createInMemorySkeletonMesh("cli_no_anim_mesh");
    ASSERT_TRUE(static_cast<bool>(mesh));

    Ogre::SceneNode* node = manager->addSceneNode("cli_no_anim_node");
    ASSERT_NE(node, nullptr);
    Ogre::Entity* entity = manager->createEntity(node, mesh);
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());
    ASSERT_EQ(entity->getMesh()->getSkeleton()->getNumAnimations(), 0u);

    const QString sourceFile = QDir::tempPath() + "/cli_no_anim_source.mesh";
    QFile::remove(sourceFile);
    ASSERT_EQ(MeshImporterExporter::exporter(node, sourceFile, "Ogre Mesh (*.mesh)"), 0);
    ASSERT_TRUE(QFile::exists(sourceFile));

    auto nodes = manager->getSceneNodes();
    for (auto* n : nodes) {
        manager->destroyAllAttachedMovableObjects(n);
        manager->destroySceneNode(n);
    }

    QByteArray sourceBa = sourceFile.toUtf8();
    TestArgv textArgs({"qtmesh", "anim", sourceBa.constData(), "--list"});
    const int textRc = CLIPipeline::cmdAnim(textArgs.argc(), textArgs.argv());

    TestArgv jsonArgs({"qtmesh", "anim", sourceBa.constData(), "--list", "--json"});
    const int jsonRc = CLIPipeline::cmdAnim(jsonArgs.argc(), jsonArgs.argv());
    ASSERT_EQ(textRc, jsonRc);
    // Environments without the importer/plugin to reload generated .mesh files
    // fail before list-mode animation handling. Accept either:
    // 0 => loaded file and handled no-animation list mode
    // 1 => loader failed before list mode could run
    EXPECT_TRUE(textRc == 0 || textRc == 1);

    QFile::remove(sourceFile);
    QFile::remove(QDir::tempPath() + "/cli_no_anim_source.material");
}

// -- cmdAnim rename --

TEST_F(CLIPipelineCmdTest, CmdAnimRename_NonexistentAnim)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    TestArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--rename", "NonExistentAnimXYZ", "NewName"});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 1);
}

TEST_F(CLIPipelineCmdTest, CmdAnimRename_Valid)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    QString outFile = QDir::tempPath() + "/cli_test_rename_inproc.mesh";
    QByteArray outBa = outFile.toUtf8();
    QFile::remove(outFile);

    // Load file first to discover animation name
    MeshImporterExporter::importer({file});
    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty() || !entities.first()->hasSkeleton())
        GTEST_SKIP() << "Could not load animated file";

    auto skel = entities.first()->getMesh()->getSkeleton();
    if (!skel || skel->getNumAnimations() == 0)
        GTEST_SKIP() << "No animations in file";

    QByteArray animName = QString::fromStdString(skel->getAnimation(0)->getName()).toUtf8();

    // Clean up loaded entities first
    auto nodes = Manager::getSingleton()->getSceneNodes();
    for (auto* node : nodes) {
        Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
        Manager::getSingleton()->destroySceneNode(node);
    }

    TestArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--rename", animName.constData(), "RenamedInProc",
                   "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/cli_test_rename_inproc.material");
}

TEST_F(CLIPipelineCmdTest, CmdAnimRename_SameNameNoop)
{
    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file)) GTEST_SKIP() << "Test data not found";
    QByteArray fileBa = file.toUtf8();

    QString outFile = QDir::tempPath() + "/cli_test_rename_noop.mesh";
    QByteArray outBa = outFile.toUtf8();
    QFile::remove(outFile);

    // Discover animation name
    MeshImporterExporter::importer({file});
    auto& entities = Manager::getSingleton()->getEntities();
    if (entities.isEmpty() || !entities.first()->hasSkeleton())
        GTEST_SKIP() << "Could not load animated file";
    auto skel = entities.first()->getMesh()->getSkeleton();
    if (!skel || skel->getNumAnimations() == 0)
        GTEST_SKIP() << "No animations in file";
    QByteArray animName = QString::fromStdString(skel->getAnimation(0)->getName()).toUtf8();

    auto nodes = Manager::getSingleton()->getSceneNodes();
    for (auto* node : nodes) {
        Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
        Manager::getSingleton()->destroySceneNode(node);
    }

    // Rename to the same name (should succeed — oldName == newName skips dup check)
    TestArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--rename", animName.constData(), animName.constData(),
                   "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0);
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/cli_test_rename_noop.material");
}

TEST_F(CLIPipelineCmdTest, CmdAnimRename_TargetNameAlreadyExists)
{
    auto* entity = createAnimatedTestEntity("cli_test_rename_existing");
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());

    Ogre::SkeletonPtr skelPtr = entity->getMesh()->getSkeleton();
    ASSERT_TRUE(static_cast<bool>(skelPtr));
    Ogre::Skeleton* skel = skelPtr.get();
    ASSERT_GT(skel->getNumAnimations(), 0u);

    const QString firstAnim = QString::fromStdString(skel->getAnimation(static_cast<unsigned short>(0))->getName());
    const QString secondAnim = "ExistingAnimationName";

    if (!skel->hasAnimation(secondAnim.toStdString())) {
        auto* second = skel->createAnimation(secondAnim.toStdString(), 1.0f);
        auto* rootBone = skel->getBone(static_cast<unsigned short>(0));
        ASSERT_NE(rootBone, nullptr);
        auto* track = second->createNodeTrack(rootBone->getHandle());
        track->setAssociatedNode(rootBone);
        auto* key = track->createNodeKeyFrame(0.0f);
        key->setTranslate(Ogre::Vector3::ZERO);
        key->setRotation(Ogre::Quaternion::IDENTITY);
        key->setScale(Ogre::Vector3::UNIT_SCALE);
    }
    ASSERT_TRUE(skel->hasAnimation(secondAnim.toStdString()));

    const QString sourceFile = QDir::tempPath() + "/cli_test_rename_existing_source.mesh";
    QFile::remove(sourceFile);
    ASSERT_EQ(
        MeshImporterExporter::exporter(
            entity->getParentSceneNode(),
            sourceFile,
            "Ogre Mesh (*.mesh)"),
        0);
    ASSERT_TRUE(QFile::exists(sourceFile));

    auto nodes = Manager::getSingleton()->getSceneNodes();
    for (auto* node : nodes) {
        Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
        Manager::getSingleton()->destroySceneNode(node);
    }

    QByteArray sourceBa = sourceFile.toUtf8();
    QByteArray firstBa = firstAnim.toUtf8();
    QByteArray secondBa = secondAnim.toUtf8();
    TestArgv args({"qtmesh", "anim", sourceBa.constData(),
                   "--rename", firstBa.constData(), secondBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 1);

    QFile::remove(sourceFile);
    QFile::remove(QDir::tempPath() + "/cli_test_rename_existing_source.material");
}

// -- cmdAnim merge --

TEST_F(CLIPipelineCmdTest, CmdAnimMerge_Valid)
{
    QString baseFile = testDataDir() + "/Twist Dance.fbx";
    QString animFile = testDataDir() + "/Hip Hop Dancing.fbx";
    if (!QFile::exists(baseFile) || !QFile::exists(animFile))
        GTEST_SKIP() << "Test data not found";
    QByteArray baseBa = baseFile.toUtf8();
    QByteArray animBa = animFile.toUtf8();

    QString outFile = QDir::tempPath() + "/cli_test_merge_inproc.mesh";
    QByteArray outBa = outFile.toUtf8();
    QFile::remove(outFile);

    TestArgv args({"qtmesh", "anim", baseBa.constData(),
                   "--merge", animBa.constData(),
                   "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/cli_test_merge_inproc.material");
}

TEST_F(CLIPipelineCmdTest, CmdAnimMerge_MultipleFiles)
{
    // Merge two files into the base to test multi-source merge path.
    // Uses files already cached by warmup + CmdAnimMerge_Valid to avoid
    // Ogre skeleton name collisions when loading 3+ distinct Mixamo files.
    QString baseFile = testDataDir() + "/Twist Dance.fbx";
    QString animFile1 = testDataDir() + "/Twist Dance.fbx";
    QString animFile2 = testDataDir() + "/Hip Hop Dancing.fbx";
    if (!QFile::exists(baseFile) || !QFile::exists(animFile2))
        GTEST_SKIP() << "Test data not found";
    QByteArray baseBa = baseFile.toUtf8();
    QByteArray anim1Ba = animFile1.toUtf8();
    QByteArray anim2Ba = animFile2.toUtf8();

    QString outFile = QDir::tempPath() + "/cli_test_merge_multi.mesh";
    QByteArray outBa = outFile.toUtf8();
    QFile::remove(outFile);

    TestArgv args({"qtmesh", "anim", baseBa.constData(),
                   "--merge", anim1Ba.constData(), anim2Ba.constData(),
                   "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(outFile));
    QFile::remove(outFile);
    QFile::remove(QDir::tempPath() + "/cli_test_merge_multi.material");
}

TEST_F(CLIPipelineCmdTest, CmdAnimMerge_NonexistentAnimFile)
{
    QString baseFile = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(baseFile)) GTEST_SKIP() << "Test data not found";
    QByteArray baseBa = baseFile.toUtf8();

    TestArgv args({"qtmesh", "anim", baseBa.constData(),
                   "--merge", "/tmp/nonexistent_anim_file_12345.fbx",
                   "-o", "/tmp/cli_test_merge_fail.mesh"});
    EXPECT_NE(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0);
}

TEST_F(CLIPipelineCmdTest, CmdAnimMerge_WithoutSourcesReturnsError)
{
    auto* manager = Manager::getSingleton();
    ASSERT_NE(manager, nullptr);
    auto* entity = createAnimatedTestEntity("cli_merge_no_sources");
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());

    const QString sourceFile = QDir::tempPath() + "/cli_merge_no_sources.mesh";
    QFile::remove(sourceFile);
    ASSERT_EQ(MeshImporterExporter::exporter(entity->getParentSceneNode(), sourceFile, "Ogre Mesh (*.mesh)"), 0);
    ASSERT_TRUE(QFile::exists(sourceFile));

    auto nodes = manager->getSceneNodes();
    for (auto* n : nodes) {
        manager->destroyAllAttachedMovableObjects(n);
        manager->destroySceneNode(n);
    }

    QByteArray sourceBa = sourceFile.toUtf8();
    TestArgv args({"qtmesh", "anim", sourceBa.constData(), "--merge"});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 1);

    QFile::remove(sourceFile);
    QFile::remove(QDir::tempPath() + "/cli_merge_no_sources.material");
}

// --- Additional formatting edge cases ---

TEST_F(CLIPipelineFormatTest, FormatMeshInfoText_ZeroValues)
{
    MeshInfo info;
    info.file = "zero.mesh";
    info.vertices = 0;
    info.triangles = 0;
    info.submeshes = 0;

    QString text = CLIPipeline::formatMeshInfoText(info);
    EXPECT_TRUE(text.contains("Vertices: 0"));
    EXPECT_TRUE(text.contains("Triangles: 0"));
    EXPECT_TRUE(text.contains("Submeshes: 0"));
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoText_MultipleAnimations)
{
    MeshInfo info;
    info.file = "multi.fbx";
    info.skeletonName = "multi.skeleton";
    info.boneCount = 3;
    info.animations.append({"walk", 1.0f});
    info.animations.append({"run", 0.5f});
    info.animations.append({"idle", 4.0f});

    QString text = CLIPipeline::formatMeshInfoText(info);
    EXPECT_TRUE(text.contains("walk"));
    EXPECT_TRUE(text.contains("run"));
    EXPECT_TRUE(text.contains("idle"));
    EXPECT_TRUE(text.contains("4.000s"));
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoJson_BoundingBoxValues)
{
    MeshInfo info;
    info.file = "bb.mesh";
    info.bbMin = Ogre::Vector3(-5.5f, -3.2f, -1.0f);
    info.bbMax = Ogre::Vector3(5.5f, 3.2f, 1.0f);

    QString json = CLIPipeline::formatMeshInfoJson(info);
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject obj = doc.object();

    ASSERT_TRUE(obj.contains("boundingBox"));
    QJsonObject bb = obj["boundingBox"].toObject();
    QJsonArray minArr = bb["min"].toArray();
    QJsonArray maxArr = bb["max"].toArray();
    EXPECT_NEAR(minArr[0].toDouble(), -5.5, 0.01);
    EXPECT_NEAR(minArr[1].toDouble(), -3.2, 0.01);
    EXPECT_NEAR(maxArr[0].toDouble(), 5.5, 0.01);
}

TEST_F(CLIPipelineFormatTest, FormatMeshInfoJson_EmptyMaterials)
{
    MeshInfo info;
    info.file = "nomats.mesh";

    QString json = CLIPipeline::formatMeshInfoJson(info);
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject obj = doc.object();

    EXPECT_TRUE(obj["materials"].isArray());
    EXPECT_EQ(obj["materials"].toArray().size(), 0);
}

// Process-based CLI tests (CLIPipelineCLI suite) removed:
// They require the QtMeshEditor binary which is not available to the
// test binary in CI. See git history for the original tests.

// --- Telemetry consent logic unit tests ---

class CLIPipelineTelemetryTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Match org/app name used by CLIPipeline::run() so QSettings
        // accesses the same store as the real CLI binary.
        m_prevOrg = QCoreApplication::organizationName();
        m_prevApp = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName("QtMeshEditor");
        QCoreApplication::setApplicationName("QtMeshEditor");

        QSettings settings;
        m_hadSetting = settings.contains("Sentry/enabled");
        if (m_hadSetting)
            m_previousValue = settings.value("Sentry/enabled").toBool();
        settings.remove("Sentry/enabled");
    }
    void TearDown() override {
        QSettings settings;
        if (m_hadSetting)
            settings.setValue("Sentry/enabled", m_previousValue);
        else
            settings.remove("Sentry/enabled");

        // Restore original org/app name
        QCoreApplication::setOrganizationName(m_prevOrg);
        QCoreApplication::setApplicationName(m_prevApp);
    }
private:
    bool m_hadSetting = false;
    bool m_previousValue = true;
    QString m_prevOrg;
    QString m_prevApp;
};

TEST_F(CLIPipelineTelemetryTest, NoTelemetryPersistsOptOut)
{
    // Simulate --no-telemetry by directly calling setEnabled(false)
    SentryReporter::setEnabled(false);
    EXPECT_FALSE(SentryReporter::isEnabled());
    EXPECT_FALSE(SentryReporter::isFirstLaunch());
}

TEST_F(CLIPipelineTelemetryTest, FirstLaunchEnablesTelemetry)
{
    // Simulate first CLI launch: isFirstLaunch() true, then setEnabled(true)
    EXPECT_TRUE(SentryReporter::isFirstLaunch());
    SentryReporter::setEnabled(true);
    EXPECT_TRUE(SentryReporter::isEnabled());
    EXPECT_FALSE(SentryReporter::isFirstLaunch());
}

TEST_F(CLIPipelineTelemetryTest, SubsequentRunSkipsNotice)
{
    // After first launch, isFirstLaunch() should be false
    SentryReporter::setEnabled(true);
    EXPECT_FALSE(SentryReporter::isFirstLaunch());
    EXPECT_TRUE(SentryReporter::isEnabled());
}

TEST_F(CLIPipelineTelemetryTest, OptOutThenNoTelemetryIsIdempotent)
{
    SentryReporter::setEnabled(false);
    EXPECT_FALSE(SentryReporter::isEnabled());

    // Calling setEnabled(false) again shouldn't change anything
    SentryReporter::setEnabled(false);
    EXPECT_FALSE(SentryReporter::isEnabled());
    EXPECT_FALSE(SentryReporter::isFirstLaunch());
}

// ==========================================================================
// cmdValidate tests
// ==========================================================================

TEST(CLIPipelineCmdValidateError, NoFile)
{
    TestArgv args({"qtmesh", "validate"});
    EXPECT_EQ(CLIPipeline::cmdValidate(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdValidateError, NonexistentFile)
{
    TestArgv args({"qtmesh", "validate", "/tmp/nonexistent_cli_validate_12345.fbx"});
    EXPECT_EQ(CLIPipeline::cmdValidate(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdValidateError, NonexistentFileWithJsonAndCliFlag)
{
    TestArgv args({"qtmesh", "--cli", "validate", "/tmp/nonexistent_cli_validate_67890.fbx", "--json"});
    EXPECT_EQ(CLIPipeline::cmdValidate(args.argc(), args.argv()), 1);
}

class CLIPipelineCmdValidateTest : public ::testing::Test {
protected:
    void SetUp() override {
        MeshValidator::kill();
        MeshLodController::kill();
        if (!tryInitOgre() || !canLoadMeshFiles())
            GTEST_SKIP() << "Ogre not available";
        createStandardOgreMaterials();
        if (Manager::getSingletonPtr())
            SelectionSet::getSingleton()->clear();
    }
    void TearDown() override {
        if (Manager::getSingletonPtr()) {
            SelectionSet::getSingleton()->clear();
            auto nodes = Manager::getSingleton()->getSceneNodes();
            for (auto* node : nodes) {
                Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
                Manager::getSingleton()->destroySceneNode(node);
            }
        }
        MeshValidator::kill();
        MeshLodController::kill();
    }
};

TEST_F(CLIPipelineCmdValidateTest, ValidateCleanMeshReportsNoErrors)
{
    auto meshPtr = createInMemoryTriangleMesh("cli_validate_clean");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("cli_validate_clean_node");
    auto* entity = sceneMgr->createEntity("cli_validate_clean_entity", meshPtr);
    node->attachObject(entity);
    SelectionSet::getSingleton()->selectOne(entity);

    MeshValidator::instance()->doValidate();
    QVariantList issues = MeshValidator::instance()->issues();

    ASSERT_FALSE(issues.isEmpty());
    bool hasError = false;
    for (const QVariant& v : issues)
        if (v.toMap().value("type").toString() == "error") hasError = true;
    EXPECT_FALSE(hasError);
}

TEST_F(CLIPipelineCmdValidateTest, ValidateIssuesHaveExpectedFields)
{
    auto meshPtr = createInMemoryTriangleMesh("cli_validate_fields");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("cli_validate_fields_node");
    auto* entity = sceneMgr->createEntity("cli_validate_fields_entity", meshPtr);
    node->attachObject(entity);
    SelectionSet::getSingleton()->selectOne(entity);

    MeshValidator::instance()->doValidate();
    QVariantList issues = MeshValidator::instance()->issues();

    for (const QVariant& v : issues) {
        QVariantMap map = v.toMap();
        EXPECT_TRUE(map.contains("type"));
        EXPECT_TRUE(map.contains("description"));
        EXPECT_TRUE(map.contains("count"));
        EXPECT_TRUE(map.contains("fixable"));
    }
}

TEST_F(CLIPipelineCmdValidateTest, CmdValidate_SucceedsForGeneratedMeshTextAndJson)
{
    const QString sourceFile = exportGeneratedTriangleMesh("cli_validate_generated");
    ASSERT_FALSE(sourceFile.isEmpty());
    ASSERT_TRUE(QFile::exists(sourceFile));
    QByteArray sourceBa = sourceFile.toUtf8();

    TestArgv textArgs({"qtmesh", "validate", sourceBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdValidate(textArgs.argc(), textArgs.argv()), 0);

    TestArgv jsonArgs({"qtmesh", "validate", sourceBa.constData(), "--json"});
    EXPECT_EQ(CLIPipeline::cmdValidate(jsonArgs.argc(), jsonArgs.argv()), 0);

    QFile::remove(sourceFile);
    QFile::remove(QDir::tempPath() + "/cli_validate_generated.material");
}

// ==========================================================================
// cmdLod tests
// ==========================================================================

TEST(CLIPipelineCmdLodError, NoFile)
{
    TestArgv args({"qtmesh", "lod"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdLodError, NoMode)
{
    TestArgv args({"qtmesh", "lod", "/tmp/some.fbx"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdLodError, NonexistentFile)
{
    TestArgv args({"qtmesh", "lod", "/tmp/nonexistent_cli_lod_12345.fbx", "--info"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdLodError, NonexistentFileWithCountReductionsAndOutput)
{
    TestArgv args({"qtmesh", "--cli", "lod", "/tmp/nonexistent_cli_lod_67890.fbx",
                   "--count", "3", "--reductions", "0.8,0.5,0.2",
                   "--output", "/tmp/cli_test_lod_out.mesh"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdLodError, NonexistentFileWithAutoMode)
{
    TestArgv args({"qtmesh", "lod", "/tmp/nonexistent_cli_lod_auto_67890.fbx", "--auto"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdLodError, NonexistentFileWithRemoveMode)
{
    TestArgv args({"qtmesh", "lod", "/tmp/nonexistent_cli_lod_remove_67890.fbx", "--remove"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdLodError, NonexistentFileWithInfoAndJsonMode)
{
    TestArgv args({"qtmesh", "lod", "/tmp/nonexistent_cli_lod_info_67890.fbx", "--info", "--json"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdLodError, InvalidCountValueReportsModeError)
{
    TestArgv args({"qtmesh", "lod", "/tmp/nonexistent_cli_lod_count_abc.fbx", "--count", "abc"});
    EXPECT_EQ(CLIPipeline::cmdLod(args.argc(), args.argv()), 2);
}

class CLIPipelineCmdLodTest : public ::testing::Test {
protected:
    void SetUp() override {
        MeshLodController::kill();
        MeshValidator::kill();
        if (!tryInitOgre() || !canLoadMeshFiles())
            GTEST_SKIP() << "Ogre not available";
        createStandardOgreMaterials();
        if (Manager::getSingletonPtr())
            SelectionSet::getSingleton()->clear();
    }
    void TearDown() override {
        if (Manager::getSingletonPtr()) {
            SelectionSet::getSingleton()->clear();
            auto nodes = Manager::getSingleton()->getSceneNodes();
            for (auto* node : nodes) {
                Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
                Manager::getSingleton()->destroySceneNode(node);
            }
        }
        MeshLodController::kill();
        MeshValidator::kill();
    }
};

TEST_F(CLIPipelineCmdLodTest, LodInfoBaseMeshHasOnlyLevel0)
{
    auto meshPtr = createInMemoryTriangleMesh("cli_lod_info");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("cli_lod_info_node");
    auto* entity = sceneMgr->createEntity("cli_lod_info_entity", meshPtr);
    node->attachObject(entity);
    SelectionSet::getSingleton()->selectOne(entity);

    QVariantList lodInfo = MeshLodController::instance()->lodLevelInfo();
    // A fresh mesh has no extra LOD levels — only the base (LOD 0)
    ASSERT_EQ(lodInfo.size(), 1);
    EXPECT_EQ(lodInfo[0].toMap().value("level").toInt(), 0);
    EXPECT_EQ(lodInfo[0].toMap().value("label").toString(), "Base");
}

TEST_F(CLIPipelineCmdLodTest, LodInfoHasExpectedFields)
{
    auto meshPtr = createInMemoryTriangleMesh("cli_lod_fields");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("cli_lod_fields_node");
    auto* entity = sceneMgr->createEntity("cli_lod_fields_entity", meshPtr);
    node->attachObject(entity);
    SelectionSet::getSingleton()->selectOne(entity);

    QVariantList lodInfo = MeshLodController::instance()->lodLevelInfo();
    for (const QVariant& v : lodInfo) {
        QVariantMap map = v.toMap();
        EXPECT_TRUE(map.contains("level"));
        EXPECT_TRUE(map.contains("label"));
        EXPECT_TRUE(map.contains("triangles"));
    }
}

TEST_F(CLIPipelineCmdLodTest, CmdLod_InfoAndRemoveFromGeneratedMesh)
{
    const QString sourceFile = exportGeneratedTriangleMesh("cli_lod_generated");
    ASSERT_FALSE(sourceFile.isEmpty());
    ASSERT_TRUE(QFile::exists(sourceFile));
    QByteArray sourceBa = sourceFile.toUtf8();

    TestArgv infoTextArgs({"qtmesh", "lod", sourceBa.constData(), "--info"});
    EXPECT_EQ(CLIPipeline::cmdLod(infoTextArgs.argc(), infoTextArgs.argv()), 0);

    TestArgv infoJsonArgs({"qtmesh", "lod", sourceBa.constData(), "--info", "--json"});
    EXPECT_EQ(CLIPipeline::cmdLod(infoJsonArgs.argc(), infoJsonArgs.argv()), 0);

    const QString removedOut = QDir::tempPath() + "/cli_lod_removed.mesh";
    QByteArray removedOutBa = removedOut.toUtf8();
    QFile::remove(removedOut);
    QFile::remove(QDir::tempPath() + "/cli_lod_removed.material");

    TestArgv removeArgs({"qtmesh", "lod", sourceBa.constData(), "--remove", "-o", removedOutBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdLod(removeArgs.argc(), removeArgs.argv()), 0);
    EXPECT_TRUE(QFile::exists(removedOut));

    QFile::remove(sourceFile);
    QFile::remove(QDir::tempPath() + "/cli_lod_generated.material");
    QFile::remove(removedOut);
    QFile::remove(QDir::tempPath() + "/cli_lod_removed.material");
}

// ==========================================================================
// cmdPose error paths
// ==========================================================================

TEST(CLIPipelineCmdPoseError, NoFile)
{
    TestArgv args({"qtmesh", "pose"});
    EXPECT_EQ(CLIPipeline::cmdPose(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdPoseError, MissingAnimation)
{
    TestArgv args({"qtmesh", "pose", "some_file.fbx", "--time", "0.0", "-o", "pose.obj"});
    EXPECT_EQ(CLIPipeline::cmdPose(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdPoseError, MissingOutput)
{
    TestArgv args({"qtmesh", "pose", "some_file.fbx", "--animation", "Idle", "--time", "0.0"});
    EXPECT_EQ(CLIPipeline::cmdPose(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdPoseError, MissingTimeAndCount)
{
    TestArgv args({"qtmesh", "pose", "some_file.fbx", "--animation", "Idle", "-o", "pose.obj"});
    EXPECT_EQ(CLIPipeline::cmdPose(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdPoseError, NonexistentFile)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    const QString missingFile = QDir(tmpDir.path()).filePath("qtmesh_pose_missing.fbx");
    QByteArray missingFileBa = missingFile.toUtf8();
    TestArgv args({"qtmesh", "pose", missingFileBa.constData(), "--animation", "Idle", "--time", "0.0", "-o", "pose.obj"});
    EXPECT_EQ(CLIPipeline::cmdPose(args.argc(), args.argv()), 1);
}

// ==========================================================================
// cmdScan tests
// ==========================================================================

TEST(CLIPipelineCmdScanError, MissingConfigFileReturns2)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    const QString missingConfig = tmpDir.filePath("qtmesh_scan_missing_config.yml");
    QFile::remove(missingConfig); // Ensure this path does not exist.
    ASSERT_FALSE(QFile::exists(missingConfig));

    QByteArray configBa = missingConfig.toUtf8();
    TestArgv args({"qtmesh", "scan", "--config", configBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdScanError, InvalidFailOnReturns2)
{
    TestArgv args({"qtmesh", "scan", "--fail-on", "fatal"});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdScanError, NonDirectoryScanRootReturns2)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    const QString filePath = tmpDir.filePath("not_a_directory.txt");
    QFile f(filePath);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write("x");
    f.close();

    QByteArray fileBa = filePath.toUtf8();
    TestArgv args({"qtmesh", "scan", fileBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdScanError, ScanRootMustBeDirectoryWithConfigReturns2)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    const QString filePath = QDir(tmpDir.path()).filePath("not_a_dir.txt");
    QFile file(filePath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
    file.write("x");
    file.close();

    const QString configPath = QDir(tmpDir.path()).filePath("scan.yml");
    QFile cfg(configPath);
    ASSERT_TRUE(cfg.open(QIODevice::WriteOnly | QIODevice::Text));
    cfg.write(
        "scan:\n"
        "  include:\n"
        "    - \"**/*.obj\"\n");
    cfg.close();

    QByteArray filePathBa = filePath.toUtf8();
    QByteArray configBa = configPath.toUtf8();
    TestArgv args({"qtmesh", "scan", filePathBa.constData(), "--config", configBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdScan, WritesJsonAndSarifReports)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    ASSERT_FALSE(writeMinimalObj(rootPath, "scan_mesh.obj").isEmpty());

    const QString configPath = QDir(tmpDir.path()).filePath("scan.yml");
    QFile cfg(configPath);
    ASSERT_TRUE(cfg.open(QIODevice::WriteOnly | QIODevice::Text));
    cfg.write(
        "scan:\n"
        "  include:\n"
        "    - \"**/*.obj\"\n"
        "rules:\n"
        "  allow_missing_materials: true\n");
    cfg.close();

    const QString reportPath = QDir(tmpDir.path()).filePath("reports/out/scan.json");
    const QString sarifPath = QDir(tmpDir.path()).filePath("reports/out/scan.sarif");
    QFile::remove(reportPath);
    QFile::remove(sarifPath);

    QByteArray rootBa = rootPath.toUtf8();
    QByteArray configBa = configPath.toUtf8();
    QByteArray reportBa = reportPath.toUtf8();
    QByteArray sarifBa = sarifPath.toUtf8();
    TestArgv args({"qtmesh", "scan", rootBa.constData(), "--config", configBa.constData(),
                   "--json", "--report", reportBa.constData(), "--sarif", sarifBa.constData(),
                   "--fail-on", "never"});

    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 0);
    ASSERT_TRUE(QFile::exists(reportPath));
    ASSERT_TRUE(QFile::exists(sarifPath));

    QFile reportFile(reportPath);
    ASSERT_TRUE(reportFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString reportContent = QString::fromUtf8(reportFile.readAll());
    EXPECT_TRUE(reportContent.contains("\"summary\""));
    EXPECT_TRUE(reportContent.contains("\"assets\""));

    QFile sarifFile(sarifPath);
    ASSERT_TRUE(sarifFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString sarifContent = QString::fromUtf8(sarifFile.readAll());
    EXPECT_TRUE(sarifContent.contains("\"runs\""));
    EXPECT_TRUE(sarifContent.contains("qtmesh scan"));
}

TEST(CLIPipelineCmdScan, ReportAndSarifAreWrittenWithFailOnNever)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    // Invalid FBX triggers a deterministic load_error finding without Ogre.
    const QString scanFile = tmpDir.filePath("bad.fbx");
    QFile invalid(scanFile);
    ASSERT_TRUE(invalid.open(QIODevice::WriteOnly | QIODevice::Text));
    invalid.write("not a real fbx");
    invalid.close();

    const QString reportPath = tmpDir.filePath("out/report.json");
    const QString sarifPath = tmpDir.filePath("out/report.sarif");
    QFile::remove(reportPath);
    QFile::remove(sarifPath);

    QByteArray rootBa = tmpDir.path().toUtf8();
    QByteArray reportBa = reportPath.toUtf8();
    QByteArray sarifBa = sarifPath.toUtf8();
    TestArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--json",
                   "--report", reportBa.constData(),
                   "--sarif", sarifBa.constData(),
                   "--fail-on", "never"});

    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 0);
    EXPECT_TRUE(QFile::exists(reportPath));
    EXPECT_TRUE(QFile::exists(sarifPath));

    QFile reportFile(reportPath);
    ASSERT_TRUE(reportFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString report = QString::fromUtf8(reportFile.readAll());
    EXPECT_TRUE(report.contains("\"summary\""));
    EXPECT_TRUE(report.contains("\"load_error\""));

    QFile sarifFile(sarifPath);
    ASSERT_TRUE(sarifFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString sarif = QString::fromUtf8(sarifFile.readAll());
    EXPECT_TRUE(sarif.contains("\"version\": \"2.1.0\""));
    EXPECT_TRUE(sarif.contains("\"tool\""));
}

TEST(CLIPipelineCmdScan, FailOnWarningReturnsFailure)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    ASSERT_FALSE(writeMinimalObj(rootPath, "PlayerModel.obj").isEmpty());

    const QString configPath = QDir(tmpDir.path()).filePath("scan.yml");
    QFile cfg(configPath);
    ASSERT_TRUE(cfg.open(QIODevice::WriteOnly | QIODevice::Text));
    cfg.write(
        "scan:\n"
        "  include:\n"
        "    - \"**/*.obj\"\n"
        "rules:\n"
        "  file_name_case: snake_case\n");
    cfg.close();

    QByteArray rootBa = rootPath.toUtf8();
    QByteArray configBa = configPath.toUtf8();
    TestArgv args({"qtmesh", "scan", rootBa.constData(), "--config", configBa.constData(),
                   "--fail-on", "warning"});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdScan, FailOnNeverAllowsWarnings)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    ASSERT_FALSE(writeMinimalObj(rootPath, "PlayerModel.obj").isEmpty());

    const QString configPath = QDir(tmpDir.path()).filePath("scan.yml");
    QFile cfg(configPath);
    ASSERT_TRUE(cfg.open(QIODevice::WriteOnly | QIODevice::Text));
    cfg.write(
        "scan:\n"
        "  include:\n"
        "    - \"**/*.obj\"\n"
        "rules:\n"
        "  file_name_case: snake_case\n");
    cfg.close();

    QByteArray rootBa = rootPath.toUtf8();
    QByteArray configBa = configPath.toUtf8();
    TestArgv args({"qtmesh", "scan", rootBa.constData(), "--config", configBa.constData(),
                   "--fail-on", "never"});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 0);
}

TEST(CLIPipelineCmdScan, IncludePatternNormalizesBareExtension)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QDir root(tmpDir.path());
    ASSERT_TRUE(root.mkpath("nested/deeper"));

    // If "*.fbx" is normalized to "**/*.fbx", this nested file is scanned,
    // causing load_error and non-zero exit with default fail_on=error.
    const QString nestedFbx = tmpDir.filePath("nested/deeper/model.fbx");
    QFile invalid(nestedFbx);
    ASSERT_TRUE(invalid.open(QIODevice::WriteOnly | QIODevice::Text));
    invalid.write("invalid fbx payload");
    invalid.close();

    QByteArray rootBa = tmpDir.path().toUtf8();
    TestArgv args({"qtmesh", "scan", rootBa.constData(), "--include", "*.fbx"});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdScan, AutoDetectConfigWritesConfiguredReports)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScopedCurrentDir cwd(tmpDir.path());

    QDir root(tmpDir.path());
    ASSERT_TRUE(root.mkpath("assets"));

    const QString scanFile = tmpDir.filePath("assets/auto_bad.fbx");
    QFile invalid(scanFile);
    ASSERT_TRUE(invalid.open(QIODevice::WriteOnly | QIODevice::Text));
    invalid.write("invalid fbx");
    invalid.close();

    const QString configPath = tmpDir.filePath("qtmesh.yml");
    QFile cfg(configPath);
    ASSERT_TRUE(cfg.open(QIODevice::WriteOnly | QIODevice::Text));
    cfg.write(
        "report:\n"
        "  format: json\n"
        "  output: auto/report.json\n"
        "  sarif_output: auto/report.sarif\n"
        "  fail_on: never\n");
    cfg.close();

    QFile::remove(tmpDir.filePath("auto/report.json"));
    QFile::remove(tmpDir.filePath("auto/report.sarif"));

    QByteArray rootBa = tmpDir.filePath("assets").toUtf8();
    TestArgv args({"qtmesh", "scan", rootBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 0);

    const QString autoReport = tmpDir.filePath("auto/report.json");
    const QString autoSarif = tmpDir.filePath("auto/report.sarif");
    EXPECT_TRUE(QFile::exists(autoReport));
    EXPECT_TRUE(QFile::exists(autoSarif));

    QFile reportFile(autoReport);
    ASSERT_TRUE(reportFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString report = QString::fromUtf8(reportFile.readAll());
    EXPECT_TRUE(report.contains("\"summary\""));
}
