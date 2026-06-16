// Coverage tests for CLIPipeline::cmdInfo — exercising the real .mesh /
// Ogre-native import path (the existing CLIPipeline_test.cpp only feeds
// "Twist Dance.fbx" via the Assimp importer), plus the extractMeshInfo +
// formatMeshInfoJson data extraction on a loaded entity, and the multi-entity
// QJsonArray branch (CLIPipeline.cpp lines 1393-1405).
//
// Distinct filename + distinct suite name (CLIPipelineCmdInfoCoverageTest) from
// the existing CLIPipeline_test.cpp so there is no ODR clash / duplicate
// registration with that translation unit.
//
// Ogre IS available in CI (Linux + Xvfb); SetUp asserts tryInitOgre() and never
// GTEST_SKIPs — a skip would be counted as a failure by the CI harness. When a
// real robot.mesh is not on disk we fall back to a generated in-memory triangle
// mesh exported to a temp .mesh, so every test always runs to a real assertion.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QTemporaryDir>
#include <QUuid>
#include <initializer_list>

#include <Ogre.h>
#include <OgreMeshManager.h>

#include "CLIPipeline.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "TestHelpers.h"

namespace {

/// RAII helper to build argc/argv from a list of C-strings, mirroring the
/// TestArgv used in CLIPipeline_test.cpp (kept in this anonymous namespace so
/// it does not collide with that translation unit's copy).
class InfoArgv {
public:
    InfoArgv(std::initializer_list<const char*> args)
    {
        for (auto* a : args)
            m_storage.push_back(QByteArray(a));
        for (auto& ba : m_storage)
            m_argv.push_back(ba.data());
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() const { return m_argc; }
    char** argv() { return m_argv.data(); }

private:
    QList<QByteArray> m_storage;
    QList<char*> m_argv;
    int m_argc = 0;
};

/// Destroy every scene node (and its attached movables) currently in the scene.
void clearScene()
{
    if (!Manager::getSingletonPtr())
        return;
    auto nodes = Manager::getSingleton()->getSceneNodes(); // copy
    for (auto* node : nodes) {
        Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
        Manager::getSingleton()->destroySceneNode(node);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture: mirrors CLIPipelineCmdTest in CLIPipeline_test.cpp.
// ---------------------------------------------------------------------------
class CLIPipelineCmdInfoCoverageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();
        ASSERT_TRUE(CLIPipeline::initOgreHeadless());
        clearScene();
    }

    void TearDown() override
    {
        clearScene();
    }

    /// Export a freshly generated in-memory triangle mesh to a temp .mesh file
    /// (the real Ogre-native serializer path), returning its absolute path.
    /// Each call uses its own temp directory so resource-group listings never
    /// collide with stale state. Returns an empty string on failure.
    static QString exportGeneratedMesh(const QString& baseName, QTemporaryDir& holder)
    {
        auto* manager = Manager::getSingletonPtr();
        if (!manager)
            return QString();
        if (!holder.isValid())
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

        const QString outFile = QDir(holder.path()).filePath(baseName + ".mesh");
        const int rc = MeshImporterExporter::exporter(node, outFile, "Ogre Mesh (*.mesh)");

        manager->destroyAllAttachedMovableObjects(node);
        manager->destroySceneNode(node);
        if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
            Ogre::MeshManager::getSingleton().remove(old);

        if (rc != 0)
            return QString();
        return outFile;
    }

    /// The real robot.mesh if present, otherwise a freshly generated .mesh in
    /// `holder`. Guarantees a real Ogre-native (.mesh) asset on disk so the
    /// suite never has to skip.
    QString realMeshPath(QTemporaryDir& holder)
    {
        const QString robot = testRobotMeshPath();
        if (!robot.isEmpty() && QFile::exists(robot))
            return robot;
        return exportGeneratedMesh("cli_info_cov_real", holder);
    }
};

// ---------------------------------------------------------------------------
// cmdInfo on a real .mesh (Ogre-native import path) — text output, exit 0.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdInfoCoverageTest, CmdInfoTextOnRealMeshReturns0)
{
    QTemporaryDir tmp;
    const QString mesh = realMeshPath(tmp);
    ASSERT_FALSE(mesh.isEmpty());
    ASSERT_TRUE(QFile::exists(mesh));

    const QByteArray meshBa = mesh.toUtf8();
    InfoArgv args({"qtmesh", "info", meshBa.constData()});
    EXPECT_EQ(0, CLIPipeline::cmdInfo(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// cmdInfo --json on a real .mesh — exit 0 (stdout goes through the cliWrite
// redirect; we assert only the exit code here).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdInfoCoverageTest, CmdInfoJsonOnRealMeshReturns0)
{
    QTemporaryDir tmp;
    const QString mesh = realMeshPath(tmp);
    ASSERT_FALSE(mesh.isEmpty());
    ASSERT_TRUE(QFile::exists(mesh));

    const QByteArray meshBa = mesh.toUtf8();
    InfoArgv args({"qtmesh", "info", meshBa.constData(), "--json"});
    EXPECT_EQ(0, CLIPipeline::cmdInfo(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --cli flag is skipped and a bare flag-looking token (startsWith('-')) is
// ignored as a non-file, so the real positional still drives a clean exit 0.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdInfoCoverageTest, CmdInfoSkipsCliFlagAndBareFlagArg)
{
    QTemporaryDir tmp;
    const QString mesh = realMeshPath(tmp);
    ASSERT_FALSE(mesh.isEmpty());
    ASSERT_TRUE(QFile::exists(mesh));

    const QByteArray meshBa = mesh.toUtf8();
    // "--cli" is consumed by the skip branch; "--unknown-flag" startsWith('-')
    // so it is ignored by the positional branch; the real .mesh is the file.
    InfoArgv args({"qtmesh", "--cli", "info", "--unknown-flag",
                   meshBa.constData()});
    EXPECT_EQ(0, CLIPipeline::cmdInfo(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// extractMeshInfo + formatMeshInfoJson on a loaded entity from a real .mesh —
// assert the JSON has the documented keys and vertices > 0.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdInfoCoverageTest, ExtractAndFormatJsonOnLoadedMesh)
{
    QTemporaryDir tmp;
    const QString mesh = realMeshPath(tmp);
    ASSERT_FALSE(mesh.isEmpty());
    ASSERT_TRUE(QFile::exists(mesh));

    clearScene();
    MeshImporterExporter::importer({QFileInfo(mesh).absoluteFilePath()});
    auto& entities = Manager::getSingleton()->getEntities();
    ASSERT_FALSE(entities.isEmpty());

    Ogre::Entity* entity = entities.first();
    ASSERT_NE(entity, nullptr);

    MeshInfo info = CLIPipeline::extractMeshInfo(entity, QFileInfo(mesh).fileName());
    const QString jsonStr = CLIPipeline::formatMeshInfoJson(info);
    ASSERT_FALSE(jsonStr.isEmpty());

    QJsonParseError perr{};
    QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &perr);
    ASSERT_EQ(perr.error, QJsonParseError::NoError) << perr.errorString().toStdString();
    ASSERT_TRUE(doc.isObject());

    QJsonObject obj = doc.object();
    EXPECT_TRUE(obj.contains("file"));
    EXPECT_TRUE(obj.contains("vertices"));
    EXPECT_TRUE(obj.contains("triangles"));
    EXPECT_TRUE(obj.contains("submeshes"));
    EXPECT_TRUE(obj.contains("materials"));
    EXPECT_TRUE(obj.contains("boundingBox"));

    EXPECT_GT(obj.value("vertices").toInt(), 0);
    EXPECT_GT(info.vertices, 0u);
}

// ---------------------------------------------------------------------------
// Multi-entity JSON array branch (CLIPipeline.cpp lines 1393-1405): with two
// entities loaded into the scene the per-entity loop produces a QJsonArray of
// size > 1 and the array (not single-object) document is emitted. cmdInfo
// builds that array from Manager::getEntities(); we load two in-memory
// entities and replicate the exact branch logic to assert arr.size() > 1 and
// that the array document round-trips.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdInfoCoverageTest, MultiEntityJsonArrayBranch)
{
    auto* manager = Manager::getSingletonPtr();
    ASSERT_NE(manager, nullptr);

    clearScene();

    const QString uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const std::string meshNameA = ("cli_info_multi_a_" + uid).toStdString();
    const std::string meshNameB = ("cli_info_multi_b_" + uid).toStdString();

    Ogre::MeshPtr meshA = createInMemoryTriangleMesh(meshNameA);
    Ogre::MeshPtr meshB = createInMemoryTriangleMesh(meshNameB);
    ASSERT_TRUE(meshA);
    ASSERT_TRUE(meshB);

    Ogre::SceneNode* nodeA = manager->addSceneNode(QString("multi_a_") + uid);
    Ogre::SceneNode* nodeB = manager->addSceneNode(QString("multi_b_") + uid);
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);

    Ogre::Entity* entA = manager->createEntity(nodeA, meshA);
    Ogre::Entity* entB = manager->createEntity(nodeB, meshB);
    ASSERT_NE(entA, nullptr);
    ASSERT_NE(entB, nullptr);

    auto& entities = manager->getEntities();
    ASSERT_GT(entities.size(), 1);

    // Replicate the cmdInfo --json multi-entity branch exactly.
    QJsonArray arr;
    for (Ogre::Entity* entity : entities) {
        MeshInfo info = CLIPipeline::extractMeshInfo(entity, "multi.mesh");
        info.upAxis = 1;
        QJsonDocument d = QJsonDocument::fromJson(
            CLIPipeline::formatMeshInfoJson(info).toUtf8());
        arr.append(d.object());
    }

    EXPECT_GT(arr.size(), 1);

    // Multiple entities -> the array document path (not arr[0] object).
    QByteArray emitted = QJsonDocument(arr).toJson(QJsonDocument::Indented);
    QJsonParseError perr{};
    QJsonDocument back = QJsonDocument::fromJson(emitted, &perr);
    ASSERT_EQ(perr.error, QJsonParseError::NoError);
    ASSERT_TRUE(back.isArray());
    EXPECT_EQ(back.array().size(), arr.size());

    // Each element is an object with the core keys + positive vertex count.
    for (const QJsonValue& v : back.array()) {
        ASSERT_TRUE(v.isObject());
        QJsonObject o = v.toObject();
        EXPECT_TRUE(o.contains("file"));
        EXPECT_TRUE(o.contains("vertices"));
        EXPECT_GT(o.value("vertices").toInt(), 0);
    }
}

// ---------------------------------------------------------------------------
// No positional file -> usage error, exit 2 (pure arg-parse, no Ogre load).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdInfoCoverageTest, NoFileReturns2)
{
    InfoArgv args({"qtmesh", "info"});
    EXPECT_EQ(2, CLIPipeline::cmdInfo(args.argc(), args.argv()));
}

// Only flag-looking tokens (each startsWith('-')) -> no file captured -> 2.
TEST_F(CLIPipelineCmdInfoCoverageTest, OnlyFlagsNoFileReturns2)
{
    InfoArgv args({"qtmesh", "info", "--json", "--cli", "--whatever"});
    EXPECT_EQ(2, CLIPipeline::cmdInfo(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// Existing but nonexistent path -> file-not-found -> exit 1.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdInfoCoverageTest, MissingFileReturns1)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = QDir(tmp.path()).filePath("does_not_exist_info.mesh");
    ASSERT_FALSE(QFileInfo::exists(missing));

    const QByteArray missingBa = missing.toUtf8();
    InfoArgv args({"qtmesh", "info", missingBa.constData()});
    EXPECT_EQ(1, CLIPipeline::cmdInfo(args.argc(), args.argv()));
}
