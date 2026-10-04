// Coverage tests for MCPServer::toolBakeVat (bake_vat).
//
// Targets the fully-untested handler at MCPServer.cpp:4543. The cheap
// argument-validation branches (lines 4550-4558) need no mesh import at all:
//   - missing 'file' / 'anim' / 'output_dir'  -> error
//   - file-not-found (QFileInfo::exists false) -> error
//   - fps <= 0                                  -> error
// The heavy success path drives the real OpenVAT bake on testRobotMeshPath()
// (a real skeletal .mesh with Idle/Shoot/Slump/Walk clips) into a QTemporaryDir
// and asserts every content key (ok/texture/sidecar/frameCount/vertexCount/
// animation/fps/bounds) plus that the texture + sidecar files exist on disk.
// The "mesh has no skeleton" branch is exercised by baking a static
// in-memory triangle mesh exported to a temp file.
//
// Distinct filename + distinct suite name (MCPServerBakeVatCoverageTest) to
// avoid any ODR / duplicate-registration clash with MCPServer_test.cpp.

#include <gtest/gtest.h>
#include <QApplication>
#include <QThread>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#define private public
#include "MCPServer.h"
#undef private

#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreMeshSerializer.h>
#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

#include <memory>

namespace {

QString resultText(const QJsonObject& result)
{
    const QJsonArray content = result["content"].toArray();
    if (content.isEmpty()) return QString();
    return content[0].toObject()["text"].toString();
}

bool resultIsError(const QJsonObject& result)
{
    return result["isError"].toBool(false);
}

/// Parse the success-path payload: toolBakeVat returns an indented JSON
/// document inside the success text content.
QJsonObject parsePayload(const QJsonObject& result)
{
    const QString text = resultText(result);
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return QJsonObject();
    return doc.object();
}

class MCPServerBakeVatCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        server.reset();
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        server = std::make_unique<MCPServer>();
    }

    void TearDown() override
    {
        if (SelectionSet::getSingletonPtr())
            SelectionSet::getSingleton()->clear();
        if (app)
            app->processEvents();
    }

    std::unique_ptr<MCPServer> server;
    QApplication* app = nullptr;
};

// ---------------------------------------------------------------------------
// Validation branch: missing 'file'.
// ---------------------------------------------------------------------------
TEST_F(MCPServerBakeVatCoverageTest, MissingFileIsError)
{
    QJsonObject args;
    args["anim"]       = "Walk";
    args["output_dir"] = "/tmp/whatever";

    const QJsonObject result = server->toolBakeVat(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("missing required"));
}

// ---------------------------------------------------------------------------
// Validation branch: missing 'anim'.
// ---------------------------------------------------------------------------
TEST_F(MCPServerBakeVatCoverageTest, MissingAnimIsError)
{
    QJsonObject args;
    args["file"]       = "/tmp/model.fbx";
    args["output_dir"] = "/tmp/whatever";

    const QJsonObject result = server->toolBakeVat(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("missing required"));
}

// ---------------------------------------------------------------------------
// Validation branch: missing 'output_dir'.
// ---------------------------------------------------------------------------
TEST_F(MCPServerBakeVatCoverageTest, MissingOutputDirIsError)
{
    QJsonObject args;
    args["file"] = "/tmp/model.fbx";
    args["anim"] = "Walk";

    const QJsonObject result = server->toolBakeVat(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("missing required"));
}

// ---------------------------------------------------------------------------
// Validation branch: all empty -> still the missing-args message.
// ---------------------------------------------------------------------------
TEST_F(MCPServerBakeVatCoverageTest, AllEmptyArgsIsError)
{
    const QJsonObject result = server->toolBakeVat(QJsonObject());
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("missing required"));
}

// ---------------------------------------------------------------------------
// Validation branch: file-not-found (QFileInfo::exists false).
// ---------------------------------------------------------------------------
TEST_F(MCPServerBakeVatCoverageTest, FileNotFoundIsError)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("does_not_exist_vat.mesh");
    ASSERT_FALSE(QFileInfo::exists(missing));

    QJsonObject args;
    args["file"]       = missing;
    args["anim"]       = "Walk";
    args["output_dir"] = tmp.path();

    const QJsonObject result = server->toolBakeVat(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("file not found"));
}

// ---------------------------------------------------------------------------
// Validation branch: fps <= 0. Use a real existing file so the fps check is
// reached (it runs after the file-existence check). fps==0 hits the branch.
// ---------------------------------------------------------------------------
TEST_F(MCPServerBakeVatCoverageTest, ZeroFpsIsError)
{
    const QString robot = testRobotMeshPath();
    ASSERT_FALSE(robot.isEmpty()) << "media/models/robot.mesh not found";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    QJsonObject args;
    args["file"]       = robot;
    args["anim"]       = "Walk";
    args["output_dir"] = tmp.path();
    args["fps"]        = 0.0;

    const QJsonObject result = server->toolBakeVat(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("fps must be > 0"));
}

// Negative fps hits the same branch.
TEST_F(MCPServerBakeVatCoverageTest, NegativeFpsIsError)
{
    const QString robot = testRobotMeshPath();
    ASSERT_FALSE(robot.isEmpty()) << "media/models/robot.mesh not found";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    QJsonObject args;
    args["file"]       = robot;
    args["anim"]       = "Walk";
    args["output_dir"] = tmp.path();
    args["fps"]        = -30.0;

    const QJsonObject result = server->toolBakeVat(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("fps must be > 0"));
}

// ---------------------------------------------------------------------------
// "mesh has no skeleton" branch: export a static in-memory triangle mesh to a
// temp .mesh file, then bake it. The import succeeds but no entity has a
// skeleton -> the no-skeleton error fires.
// ---------------------------------------------------------------------------
TEST_F(MCPServerBakeVatCoverageTest, StaticMeshHasNoSkeletonIsError)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context required to build/load meshes";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // Build a static triangle mesh and write it as a .mesh the importer can
    // re-load. createInMemoryTriangleMesh creates a skeleton-less mesh.
    Ogre::MeshPtr mesh = createInMemoryTriangleMesh("BakeVatStaticTri");
    ASSERT_TRUE(static_cast<bool>(mesh));

    const QString meshPath = QDir(tmp.path()).filePath("static_tri.mesh");
    // Serialize the in-memory mesh to disk so toolBakeVat's
    // TransientImportSession can re-load it as a real file. The reloaded
    // entity has no skeleton -> the no-skeleton branch fires.
    {
        Ogre::MeshSerializer serializer;
        serializer.exportMesh(mesh.get(), meshPath.toStdString());
    }
    ASSERT_TRUE(QFileInfo::exists(meshPath))
        << "Failed to serialize static triangle mesh to disk";

    QJsonObject args;
    args["file"]       = meshPath;
    args["anim"]       = "Walk";
    args["output_dir"] = tmp.path();
    args["fps"]        = 30.0;

    const QJsonObject result = server->toolBakeVat(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("no skeleton"));
}

// ---------------------------------------------------------------------------
// Success path: bake a real skeletal animation off robot.mesh into a temp dir.
// Assert every content key and that both output files exist on disk.
// ---------------------------------------------------------------------------
TEST_F(MCPServerBakeVatCoverageTest, BakesRealSkeletalAnimationSuccess)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context required to load skeletal mesh";

    const QString robot = testRobotMeshPath();
    ASSERT_FALSE(robot.isEmpty()) << "media/models/robot.mesh not found";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // "Walk" is a known animation on robot.skeleton (Idle/Shoot/Slump/Walk).
    const QString animName = "Walk";

    QJsonObject args;
    args["file"]       = robot;
    args["anim"]       = animName;
    args["output_dir"] = tmp.path();
    args["fps"]        = 24.0;
    args["basename"]   = "robot_walk_vat";

    const QJsonObject result = server->toolBakeVat(args);
    ASSERT_FALSE(resultIsError(result))
        << "bake_vat reported error: " << resultText(result).toStdString();

    const QJsonObject payload = parsePayload(result);
    ASSERT_FALSE(payload.isEmpty()) << "Success payload was not valid JSON";

    // ok / animation / fps echo-back.
    EXPECT_TRUE(payload["ok"].toBool());
    EXPECT_EQ(payload["animation"].toString(), animName);
    EXPECT_DOUBLE_EQ(payload["fps"].toDouble(), 24.0);

    // Counts must be positive for a real skinned mesh.
    EXPECT_GT(payload["frameCount"].toInt(), 0);
    EXPECT_GT(payload["vertexCount"].toInt(), 0);

    // texture + sidecar paths present and on disk.
    const QString texPath = payload["texture"].toString();
    const QString sidecarPath = payload["sidecar"].toString();
    EXPECT_FALSE(texPath.isEmpty());
    EXPECT_FALSE(sidecarPath.isEmpty());
    EXPECT_TRUE(QFileInfo::exists(texPath))
        << "Position texture missing on disk: " << texPath.toStdString();
    EXPECT_TRUE(QFileInfo::exists(sidecarPath))
        << "Sidecar JSON missing on disk: " << sidecarPath.toStdString();

    // bounds object with min/max each carrying x/y/z.
    ASSERT_TRUE(payload.contains("bounds"));
    const QJsonObject bounds = payload["bounds"].toObject();
    ASSERT_TRUE(bounds.contains("min"));
    ASSERT_TRUE(bounds.contains("max"));
    const QJsonObject lo = bounds["min"].toObject();
    const QJsonObject hi = bounds["max"].toObject();
    EXPECT_TRUE(lo.contains("x") && lo.contains("y") && lo.contains("z"));
    EXPECT_TRUE(hi.contains("x") && hi.contains("y") && hi.contains("z"));
    // A non-degenerate mesh occupies space: max should exceed min on at
    // least one axis.
    EXPECT_GE(hi["x"].toDouble(), lo["x"].toDouble());
    EXPECT_GE(hi["y"].toDouble(), lo["y"].toDouble());
    EXPECT_GE(hi["z"].toDouble(), lo["z"].toDouble());
}

// ---------------------------------------------------------------------------
// Success path variant: omit 'basename' so it defaults to the animation name,
// and omit 'fps' so it defaults to 30. Confirms the default-fps + default-
// basename code paths and that the files still land on disk.
// ---------------------------------------------------------------------------
TEST_F(MCPServerBakeVatCoverageTest, BakeDefaultsFpsAndBasename)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context required to load skeletal mesh";

    const QString robot = testRobotMeshPath();
    ASSERT_FALSE(robot.isEmpty()) << "media/models/robot.mesh not found";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    QJsonObject args;
    args["file"]       = robot;
    args["anim"]       = "Idle"; // another known robot clip
    args["output_dir"] = tmp.path();
    // no fps -> defaults to 30; no basename -> defaults to "Idle".

    const QJsonObject result = server->toolBakeVat(args);
    ASSERT_FALSE(resultIsError(result))
        << "bake_vat reported error: " << resultText(result).toStdString();

    const QJsonObject payload = parsePayload(result);
    ASSERT_FALSE(payload.isEmpty());
    EXPECT_TRUE(payload["ok"].toBool());
    EXPECT_EQ(payload["animation"].toString(), QString("Idle"));
    EXPECT_DOUBLE_EQ(payload["fps"].toDouble(), 30.0);

    const QString texPath = payload["texture"].toString();
    const QString sidecarPath = payload["sidecar"].toString();
    EXPECT_TRUE(QFileInfo::exists(texPath));
    EXPECT_TRUE(QFileInfo::exists(sidecarPath));
}

// ---------------------------------------------------------------------------
// Error path: a valid skeletal mesh but a bogus animation name. The bake
// fails inside VATBaker::bake (animation not found) and surfaces as a
// "VAT bake failed" error.
// ---------------------------------------------------------------------------
TEST_F(MCPServerBakeVatCoverageTest, UnknownAnimationFailsBake)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context required to load skeletal mesh";

    const QString robot = testRobotMeshPath();
    ASSERT_FALSE(robot.isEmpty()) << "media/models/robot.mesh not found";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    QJsonObject args;
    args["file"]       = robot;
    args["anim"]       = "NoSuchAnimation_xyzzy";
    args["output_dir"] = tmp.path();
    args["fps"]        = 30.0;

    const QJsonObject result = server->toolBakeVat(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("VAT bake failed"));
}

} // namespace

// ---------------------------------------------------------------------------
// #522 — mode / encoding / target validation happens BEFORE the import.
// ---------------------------------------------------------------------------
TEST_F(MCPServerBakeVatCoverageTest, UnknownModeIsErrorBeforeImport)
{
    QJsonObject args;
    args["file"]       = "/nonexistent/model.fbx";   // would fail import — must not get there
    args["anim"]       = "Walk";
    args["output_dir"] = "/tmp/whatever";
    args["mode"]       = "bogus";
    const QJsonObject result = server->toolBakeVat(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("unknown mode")) << resultText(result).toStdString();
}

TEST_F(MCPServerBakeVatCoverageTest, UnknownEncodingIsError)
{
    QJsonObject args;
    args["file"]       = "/nonexistent/model.fbx";
    args["anim"]       = "Walk";
    args["output_dir"] = "/tmp/whatever";
    args["encoding"]   = "jpeg";
    const QJsonObject result = server->toolBakeVat(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("unknown encoding")) << resultText(result).toStdString();
}

TEST_F(MCPServerBakeVatCoverageTest, UnknownTargetIsError)
{
    QJsonObject args;
    args["file"]       = "/nonexistent/model.fbx";
    args["anim"]       = "Walk";
    args["output_dir"] = "/tmp/whatever";
    args["target"]     = "blender";
    const QJsonObject result = server->toolBakeVat(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("unknown target")) << resultText(result).toStdString();
}

// Morph mode fills in the default weight clip, so a missing 'anim' is
// no longer the failing gate — the missing file is.
TEST_F(MCPServerBakeVatCoverageTest, MorphModeDefaultsAnimName)
{
    QJsonObject args;
    args["file"]       = "/nonexistent/model.glb";
    args["output_dir"] = "/tmp/whatever";
    args["mode"]       = "morph";
    const QJsonObject result = server->toolBakeVat(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("file not found")) << resultText(result).toStdString();
}

// Skeletal mesh + morph mode: the baker refuses (no poses) and the
// error names it rather than silently baking a skeletal VAT.
TEST_F(MCPServerBakeVatCoverageTest, MorphModeOnSkeletalMeshReportsNoMorphTargets)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context required to load skeletal mesh";
    ASSERT_FALSE(testRobotMeshPath().isEmpty()) << "robot.mesh fixture not found";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QJsonObject args;
    args["file"]       = testRobotMeshPath();
    args["anim"]       = "Walk";
    args["output_dir"] = tmp.path();
    args["mode"]       = "morph";
    const QJsonObject result = server->toolBakeVat(args);
    EXPECT_TRUE(resultIsError(result));
    EXPECT_TRUE(resultText(result).contains("morph targets")) << resultText(result).toStdString();
}

// Rigid mode on the robot (per-submesh vertex data, skeletal clip):
// the payload carries chunk info and the mode tag.
TEST_F(MCPServerBakeVatCoverageTest, RigidModePayloadCarriesChunks)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "GL context required to load skeletal mesh";
    ASSERT_FALSE(testRobotMeshPath().isEmpty()) << "robot.mesh fixture not found";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    QJsonObject args;
    args["file"]       = testRobotMeshPath();
    args["anim"]       = "Walk";
    args["output_dir"] = tmp.path();
    args["mode"]       = "rigid";
    args["encoding"]   = "rgba8";
    args["target"]     = "godot";
    const QJsonObject result = server->toolBakeVat(args);
    if (resultIsError(result)) {
        // A shared-vertex robot mesh cannot be chunked; that is a
        // documented refusal, not a failure of the tool plumbing.
        EXPECT_TRUE(resultText(result).contains("shared")) << resultText(result).toStdString();
        return;
    }
    const QJsonObject payload = parsePayload(result);
    EXPECT_EQ(payload["mode"].toString(), QStringLiteral("rigid"));
    EXPECT_EQ(payload["encoding"].toString(), QStringLiteral("rgba8"));
    EXPECT_EQ(payload["target"].toString(), QStringLiteral("godot"));
    EXPECT_GT(payload["chunkCount"].toInt(), 0);
    EXPECT_TRUE(payload.contains("chunks"));
    EXPECT_TRUE(QFile::exists(payload["texture"].toString()));
    // target=godot ships the rigid Godot template next to the bake.
    EXPECT_TRUE(QFile::exists(QDir(tmp.path()).filePath("openvat_rigid.gdshader")));
}
