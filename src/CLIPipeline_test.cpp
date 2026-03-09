#include <gtest/gtest.h>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "CLIPipeline.h"
#include "TestHelpers.h"

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

QString testDataDir()
{
    QString binDir = QCoreApplication::applicationDirPath();
    QDir dir(binDir);
    dir.cdUp(); // bin -> build_local
    dir.cdUp(); // build_local -> project root
    return dir.absoluteFilePath("media/models");
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
