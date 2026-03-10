#include <gtest/gtest.h>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSettings>
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
    EXPECT_EQ(obj["skeleton"].toObject()["bones"].toInt(), 5);
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

TEST(CLIPipelineFormatForExtension, GLTF2)
{
    EXPECT_EQ(CLIPipeline::formatForExtension("model.gltf2"), "glTF 2.0 (*.gltf2)");
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

// --- run() tests (early-return paths that don't create QApplication or call _exit) ---

TEST(CLIPipelineRun, HelpFlag)
{
    char arg0[] = "qtmesh";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    EXPECT_EQ(CLIPipeline::run(2, argv), 0);
}

TEST(CLIPipelineRun, HelpFlagShort)
{
    char arg0[] = "qtmesh";
    char arg1[] = "-h";
    char* argv[] = {arg0, arg1};
    EXPECT_EQ(CLIPipeline::run(2, argv), 0);
}

TEST(CLIPipelineRun, VersionFlag)
{
    char arg0[] = "qtmesh";
    char arg1[] = "--version";
    char* argv[] = {arg0, arg1};
    EXPECT_EQ(CLIPipeline::run(2, argv), 0);
}

TEST(CLIPipelineRun, VersionFlagShort)
{
    char arg0[] = "qtmesh";
    char arg1[] = "-v";
    char* argv[] = {arg0, arg1};
    EXPECT_EQ(CLIPipeline::run(2, argv), 0);
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
    EXPECT_EQ(CLIPipeline::run(3, argv), 0);
}

TEST(CLIPipelineRun, CliWithHelp)
{
    char arg0[] = "qtmesh";
    char arg1[] = "--cli";
    char arg2[] = "--help";
    char* argv[] = {arg0, arg1, arg2};
    EXPECT_EQ(CLIPipeline::run(3, argv), 0);
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

TEST(CLIPipelineCmdAnimError, NonexistentFile)
{
    TestArgv args({"qtmesh", "anim", "/tmp/nonexistent_cli_test_12345.fbx", "--list"});
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
    QString baseFile = testDataDir() + "/Twist Dance.fbx";
    QString animFile1 = testDataDir() + "/Hip Hop Dancing.fbx";
    QString animFile2 = testDataDir() + "/Rumba Dancing.fbx";
    if (!QFile::exists(baseFile) || !QFile::exists(animFile1) || !QFile::exists(animFile2))
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

// --- Process-based CLI tests ---

namespace {

QString findAppBinary()
{
    QString testBinDir = QCoreApplication::applicationDirPath();

#ifdef Q_OS_MACOS
    QString macPath = testBinDir + "/QtMeshEditor.app/Contents/MacOS/QtMeshEditor";
    if (QFile::exists(macPath))
        return macPath;
#endif

    QString directPath = testBinDir + "/QtMeshEditor";
#ifdef Q_OS_WIN
    directPath += ".exe";
#endif
    if (QFile::exists(directPath))
        return directPath;

    return {};
}

QString tempPath(const QString& filename)
{
    return QDir::tempPath() + "/" + filename;
}

} // anonymous namespace

// --- Global options ---

TEST(CLIPipelineCLI, HelpFlag)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"--cli", "--help"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("Usage:") || out.contains("Commands:"))
        << "stdout: " << out.toStdString();
}

TEST(CLIPipelineCLI, HelpFlagShort)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"--cli", "-h"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("Usage:") || out.contains("Commands:"));
}

TEST(CLIPipelineCLI, VersionFlag)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"--cli", "--version"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("qtmesh")) << "stdout: " << out.toStdString();
}

TEST(CLIPipelineCLI, VersionFlagShort)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"--cli", "-v"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("qtmesh"));
}

TEST(CLIPipelineCLI, NoCommand)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"--cli"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 2);
}

TEST(CLIPipelineCLI, UnknownCommand)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"--cli", "bogus"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 2);

    QString errOut = QString::fromUtf8(proc.readAllStandardError());
    EXPECT_TRUE(errOut.contains("Unknown command")) << "stderr: " << errOut.toStdString();
}

// --- info subcommand ---

TEST(CLIPipelineCLI, InfoNoFile)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"info"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 2);
}

TEST(CLIPipelineCLI, InfoNonexistentFile)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"info", tempPath("nonexistent_file_12345.fbx")});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 1);

    QString errOut = QString::fromUtf8(proc.readAllStandardError());
    EXPECT_TRUE(errOut.contains("File not found")) << "stderr: " << errOut.toStdString();
}

TEST(CLIPipelineCLI, InfoValidFile)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file))
        GTEST_SKIP() << "Test data not found";

    QProcess proc;
    proc.start(binary, {"info", file});
    ASSERT_TRUE(proc.waitForFinished(60000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("Vertices:")) << "stdout: " << out.toStdString();
    EXPECT_TRUE(out.contains("Triangles:"));
}

TEST(CLIPipelineCLI, InfoJsonOutput)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file))
        GTEST_SKIP() << "Test data not found";

    QProcess proc;
    proc.start(binary, {"info", file, "--json"});
    ASSERT_TRUE(proc.waitForFinished(60000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
    EXPECT_TRUE(doc.isObject()) << "stdout not valid JSON: " << out.toStdString();
    EXPECT_TRUE(doc.object().contains("vertices"));
}

// --- convert subcommand ---

TEST(CLIPipelineCLI, ConvertMissingArgs)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"convert"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 2);
}

TEST(CLIPipelineCLI, ConvertMissingOutput)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"convert", "somefile.fbx"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 2);
}

TEST(CLIPipelineCLI, ConvertNonexistentFile)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"convert", tempPath("nonexistent_12345.fbx"), "-o", tempPath("out.mesh")});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 1);
}

TEST(CLIPipelineCLI, ConvertValidFile)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file))
        GTEST_SKIP() << "Test data not found";

    QString outFile = tempPath("cli_convert_test.mesh");
    QString outMaterial = tempPath("cli_convert_test.material");
    QFile::remove(outFile);
    QFile::remove(outMaterial);

    QProcess proc;
    proc.start(binary, {"convert", file, "-o", outFile});
    ASSERT_TRUE(proc.waitForFinished(60000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("Converted:")) << "stdout: " << out.toStdString();
    EXPECT_TRUE(QFile::exists(outFile));

    QFile::remove(outFile);
    QFile::remove(outMaterial);
}

// --- fix subcommand ---

TEST(CLIPipelineCLI, FixNoFile)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"fix"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 2);
}

TEST(CLIPipelineCLI, FixNonexistentFile)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"fix", tempPath("nonexistent_12345.fbx")});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 1);
}

TEST(CLIPipelineCLI, FixValidFile)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file))
        GTEST_SKIP() << "Test data not found";

    QString outFile = tempPath("cli_fix_test.mesh");
    QString outMaterial = tempPath("cli_fix_test.material");
    QFile::remove(outFile);
    QFile::remove(outMaterial);

    QProcess proc;
    proc.start(binary, {"fix", file, "-o", outFile});
    ASSERT_TRUE(proc.waitForFinished(60000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("Fixed:")) << "stdout: " << out.toStdString();
    EXPECT_TRUE(out.contains("Vertices:"));

    QFile::remove(outFile);
    QFile::remove(outMaterial);
}

TEST(CLIPipelineCLI, FixWithAllFlag)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file))
        GTEST_SKIP() << "Test data not found";

    QString outFile = tempPath("cli_fix_all_test.mesh");
    QString outMaterial = tempPath("cli_fix_all_test.material");
    QFile::remove(outFile);
    QFile::remove(outMaterial);

    QProcess proc;
    proc.start(binary, {"fix", file, "-o", outFile, "--all"});
    ASSERT_TRUE(proc.waitForFinished(60000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("Extra:")) << "stdout: " << out.toStdString();

    QFile::remove(outFile);
    QFile::remove(outMaterial);
}

TEST(CLIPipelineCLI, FixWithIndividualFlags)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file))
        GTEST_SKIP() << "Test data not found";

    QString outFile = tempPath("cli_fix_flags_test.mesh");
    QString outMaterial = tempPath("cli_fix_flags_test.material");
    QFile::remove(outFile);
    QFile::remove(outMaterial);

    QProcess proc;
    proc.start(binary, {"fix", file, "-o", outFile, "--remove-degenerates", "--merge-materials"});
    ASSERT_TRUE(proc.waitForFinished(60000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("remove-degenerates")) << "stdout: " << out.toStdString();
    EXPECT_TRUE(out.contains("merge-materials"));

    QFile::remove(outFile);
    QFile::remove(outMaterial);
}

// --- anim subcommand ---

TEST(CLIPipelineCLI, AnimNoFile)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"anim"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_NE(proc.exitCode(), 0);
}

TEST(CLIPipelineCLI, AnimNoAction)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"anim", "somefile.fbx"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 2);

    QString errOut = QString::fromUtf8(proc.readAllStandardError());
    EXPECT_TRUE(errOut.contains("--list") && errOut.contains("--rename") && errOut.contains("--merge"))
        << "stderr: " << errOut.toStdString();
}

TEST(CLIPipelineCLI, AnimListNonexistentFile)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"anim", tempPath("nonexistent_12345.fbx"), "--list"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 1);
}

TEST(CLIPipelineCLI, AnimListValid)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file))
        GTEST_SKIP() << "Test data not found";

    QProcess proc;
    proc.start(binary, {"anim", file, "--list"});
    ASSERT_TRUE(proc.waitForFinished(60000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("Animations:") || out.contains("No animations"))
        << "stdout: " << out.toStdString();
}

TEST(CLIPipelineCLI, AnimListJson)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file))
        GTEST_SKIP() << "Test data not found";

    QProcess proc;
    proc.start(binary, {"anim", file, "--list", "--json"});
    ASSERT_TRUE(proc.waitForFinished(60000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
    EXPECT_TRUE(doc.isArray()) << "Expected JSON array, got: " << out.toStdString();
}

TEST(CLIPipelineCLI, AnimRenameNonexistentAnimation)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file))
        GTEST_SKIP() << "Test data not found";

    QProcess proc;
    proc.start(binary, {"anim", file, "--rename", "NonExistentAnim", "NewName"});
    ASSERT_TRUE(proc.waitForFinished(60000));
    EXPECT_EQ(proc.exitCode(), 1);

    QString errOut = QString::fromUtf8(proc.readAllStandardError());
    EXPECT_TRUE(errOut.contains("not found")) << "stderr: " << errOut.toStdString();
}

TEST(CLIPipelineCLI, AnimRenameDuplicateTarget)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file))
        GTEST_SKIP() << "Test data not found";

    // Get existing animation name
    QProcess listProc;
    listProc.start(binary, {"anim", file, "--list", "--json"});
    ASSERT_TRUE(listProc.waitForFinished(60000));
    if (listProc.exitCode() != 0) GTEST_SKIP() << "Could not list animations";

    QString listOut = QString::fromUtf8(listProc.readAllStandardOutput());
    QJsonDocument doc = QJsonDocument::fromJson(listOut.toUtf8());
    if (!doc.isArray() || doc.array().size() < 2) GTEST_SKIP() << "Need at least 2 animations";

    // Attempt to rename first animation to the name of the second
    QString firstName = doc.array()[0].toObject()["name"].toString();
    QString secondName = doc.array()[1].toObject()["name"].toString();

    QProcess proc;
    proc.start(binary, {"anim", file, "--rename", firstName, secondName});
    ASSERT_TRUE(proc.waitForFinished(60000));
    EXPECT_EQ(proc.exitCode(), 1);

    QString errOut = QString::fromUtf8(proc.readAllStandardError());
    EXPECT_TRUE(errOut.contains("already exists")) << "stderr: " << errOut.toStdString();
}

TEST(CLIPipelineCLI, AnimRenameValid)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file))
        GTEST_SKIP() << "Test data not found";

    // First get the animation name
    QProcess listProc;
    listProc.start(binary, {"anim", file, "--list", "--json"});
    ASSERT_TRUE(listProc.waitForFinished(60000));
    if (listProc.exitCode() != 0) GTEST_SKIP() << "Could not list animations";

    QString listOut = QString::fromUtf8(listProc.readAllStandardOutput());
    QJsonDocument doc = QJsonDocument::fromJson(listOut.toUtf8());
    if (!doc.isArray() || doc.array().isEmpty()) GTEST_SKIP() << "No animations in test file";

    QString animName = doc.array()[0].toObject()["name"].toString();
    if (animName.isEmpty()) GTEST_SKIP() << "Could not get animation name";

    QString outFile = tempPath("cli_rename_test.mesh");
    QString outMaterial = tempPath("cli_rename_test.material");
    QFile::remove(outFile);
    QFile::remove(outMaterial);

    QProcess proc;
    proc.start(binary, {"anim", file, "--rename", animName, "RenamedAnim", "-o", outFile});
    ASSERT_TRUE(proc.waitForFinished(60000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("Renamed animation")) << "stdout: " << out.toStdString();
    EXPECT_TRUE(QFile::exists(outFile));

    QFile::remove(outFile);
    QFile::remove(outMaterial);
}

TEST(CLIPipelineCLI, AnimMergeNonexistentAnimFile)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QString file = testDataDir() + "/Twist Dance.fbx";
    if (!QFile::exists(file))
        GTEST_SKIP() << "Test data not found";

    QProcess proc;
    proc.start(binary, {"anim", file, "--merge", tempPath("nonexistent_file_12345.fbx"),
                         "-o", tempPath("merge_fail.mesh")});
    ASSERT_TRUE(proc.waitForFinished(60000));
    EXPECT_NE(proc.exitCode(), 0);

    QString errOut = QString::fromUtf8(proc.readAllStandardError());
    EXPECT_TRUE(errOut.contains("Failed to load animation file") || errOut.contains("Error:"))
        << "stderr: " << errOut.toStdString();
}

TEST(CLIPipelineCLI, AnimMergeValidFiles)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QString baseFile = testDataDir() + "/Twist Dance.fbx";
    QString animFile = testDataDir() + "/Hip Hop Dancing.fbx";
    if (!QFile::exists(baseFile) || !QFile::exists(animFile))
        GTEST_SKIP() << "Test data not found";

    QString outFile = tempPath("cli_anim_merge_test.mesh");
    QString outMaterial = tempPath("cli_anim_merge_test.material");
    QFile::remove(outFile);
    QFile::remove(outMaterial);

    QProcess proc;
    proc.start(binary, {"anim", baseFile, "--merge", animFile, "-o", outFile});
    ASSERT_TRUE(proc.waitForFinished(120000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("Merged")) << "stdout: " << out.toStdString();
    EXPECT_TRUE(QFile::exists(outFile));

    QFile::remove(outFile);
    QFile::remove(outMaterial);
}

// --- Verbose flag ---

TEST(CLIPipelineCLI, VerboseWithHelp)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"--cli", "--verbose", "--help"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 0);
}

// --- Telemetry / --no-telemetry tests ---

TEST(CLIPipelineRun, HelpTextContainsNoTelemetry)
{
    // printUsage should mention --no-telemetry
    // We test this indirectly via the --help process test
    char arg0[] = "qtmesh";
    char arg1[] = "--help";
    char* argv[] = {arg0, arg1};
    // Just verify it doesn't crash; the process-based test checks content
    EXPECT_EQ(CLIPipeline::run(2, argv), 0);
}

TEST(CLIPipelineRun, NoTelemetryWithHelp)
{
    // --no-telemetry should be skipped when looking for the subcommand
    char arg0[] = "qtmesh";
    char arg1[] = "--no-telemetry";
    char arg2[] = "--help";
    char* argv[] = {arg0, arg1, arg2};
    EXPECT_EQ(CLIPipeline::run(3, argv), 0);
}

TEST(CLIPipelineCLI, HelpOutputContainsNoTelemetry)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"--cli", "--help"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("--no-telemetry"))
        << "Help text should mention --no-telemetry. Got: " << out.toStdString();
}

TEST(CLIPipelineCLI, NoTelemetryWithHelp)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"--cli", "--no-telemetry", "--help"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString out = QString::fromUtf8(proc.readAllStandardOutput());
    EXPECT_TRUE(out.contains("Usage:") || out.contains("Commands:"));
}

TEST(CLIPipelineCLI, NoTelemetryPrintsConfirmation)
{
    QString binary = findAppBinary();
    if (binary.isEmpty()) GTEST_SKIP() << "Binary not found";

    QProcess proc;
    proc.start(binary, {"--cli", "--no-telemetry", "--help"});
    ASSERT_TRUE(proc.waitForFinished(30000));
    EXPECT_EQ(proc.exitCode(), 0);

    QString errOut = QString::fromUtf8(proc.readAllStandardError());
    EXPECT_TRUE(errOut.contains("Telemetry disabled"))
        << "stderr should confirm opt-out. Got: " << errOut.toStdString();
}

// --- Telemetry consent logic unit tests ---

class CLIPipelineTelemetryTest : public ::testing::Test {
protected:
    void SetUp() override {
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
    }
private:
    bool m_hadSetting = false;
    bool m_previousValue = true;
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
