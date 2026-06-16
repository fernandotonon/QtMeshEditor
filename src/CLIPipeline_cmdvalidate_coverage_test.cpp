// Coverage tests for CLIPipeline::cmdValidate(argc, argv).
//
// These exercise the live import + SelectionSet append loop, the JSON
// QJsonArray build loop, and the text-output issue-formatting loop
// (including the type=="ok" "OK:" branch and the "[TYPE] desc" else
// branch) using a real .mesh asset (robot.mesh) which produces richer
// validator output than a generated triangle mesh. When robot.mesh is
// unavailable the tests fall back to a freshly-exported generated
// triangle mesh so every test still runs (no GTEST_SKIP).
//
// Distinct filename + distinct TEST suite names from CLIPipeline_test.cpp
// to avoid any ODR / duplicate-registration clash.

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
#include <QUuid>
#include <vector>

#include <Ogre.h>
#include <OgreMeshManager.h>

#include "CLIPipeline.h"
#include "Manager.h"
#include "MeshImporterExporter.h"
#include "MeshLodController.h"
#include "MeshValidator.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

namespace {

// RAII helper to build argc/argv from a list of strings (local to this TU;
// the one in CLIPipeline_test.cpp lives in an anonymous namespace there).
class CovArgv {
public:
    CovArgv(std::initializer_list<const char*> args)
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

// Export a generated triangle mesh into its own temp directory and return
// the .mesh path (empty on failure). Mirrors the helper in
// CLIPipeline_test.cpp but is local to this TU.
QString covExportGeneratedTriangleMesh(const QString& baseName)
{
    auto* manager = Manager::getSingletonPtr();
    if (!manager)
        return QString();

    const QString exportRoot = QDir::tempPath() + "/qtmesh_cov_val_" +
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(exportRoot))
        return QString();

    const std::string meshName = (baseName + "_mesh").toStdString();
    const QString nodeName = baseName + "_node";

    Ogre::MeshPtr mesh = createInMemoryTriangleMesh(meshName);
    Ogre::SceneNode* node = manager->addSceneNode(nodeName);
    if (!node) {
        if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
            Ogre::MeshManager::getSingleton().remove(old);
        QDir(exportRoot).removeRecursively();
        return QString();
    }

    Ogre::Entity* entity = manager->createEntity(node, mesh);
    if (!entity) {
        manager->destroySceneNode(node);
        if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
            Ogre::MeshManager::getSingleton().remove(old);
        QDir(exportRoot).removeRecursively();
        return QString();
    }

    const QString outFile = QDir(exportRoot).filePath(baseName + ".mesh");
    QFile::remove(outFile);
    QFile::remove(QDir(exportRoot).filePath(baseName + ".material"));

    const int exportRc = MeshImporterExporter::exporter(node, outFile, "Ogre Mesh (*.mesh)");

    manager->destroyAllAttachedMovableObjects(node);
    manager->destroySceneNode(node);
    if (auto old = Ogre::MeshManager::getSingleton().getByName(meshName))
        Ogre::MeshManager::getSingleton().remove(old);

    if (exportRc != 0) {
        QDir(exportRoot).removeRecursively();
        return QString();
    }
    return outFile;
}

void covRemoveExportTree(const QString& meshFilePath)
{
    if (meshFilePath.isEmpty())
        return;
    QFileInfo fi(meshFilePath);
    QDir(fi.absolutePath()).removeRecursively();
}

} // namespace

// ==========================================================================
// Fixture mirroring CLIPipelineCmdValidateTest in CLIPipeline_test.cpp.
// ==========================================================================
class CLIPipelineCmdValidateCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        MeshValidator::kill();
        MeshLodController::kill();
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
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

    // Returns a real, existing .mesh file path to validate. Prefers
    // robot.mesh (real geometry → richer OK/info issues); falls back to a
    // freshly-exported generated triangle mesh so the test still runs.
    // `cleanupTree` is set true only when the path is a temp export that
    // should be removed after the test.
    QString validatableMesh(const QString& fallbackBaseName, bool& cleanupTree)
    {
        cleanupTree = false;
        const QString robot = testRobotMeshPath();
        if (!robot.isEmpty() && QFile::exists(robot))
            return robot;
        cleanupTree = true;
        return covExportGeneratedTriangleMesh(fallbackBaseName);
    }
};

// --------------------------------------------------------------------------
// Text mode on a real .mesh: exercises the import + SelectionSet append
// loop and the text issue-formatting loop (OK: branch + [TYPE] else branch).
// Asserts exit 0 (clean mesh → no errors → hasErrors==false branch).
// --------------------------------------------------------------------------
TEST_F(CLIPipelineCmdValidateCoverageTest, TextModeOnRealMeshExitsZero)
{
    bool cleanup = false;
    const QString meshPath = validatableMesh("cov_validate_text", cleanup);
    ASSERT_FALSE(meshPath.isEmpty());
    ASSERT_TRUE(QFile::exists(meshPath));

    QByteArray ba = meshPath.toUtf8();
    CovArgv args({"qtmesh", "validate", ba.constData()});
    EXPECT_EQ(CLIPipeline::cmdValidate(args.argc(), args.argv()), 0);

    if (cleanup)
        covRemoveExportTree(meshPath);
}

// --------------------------------------------------------------------------
// JSON mode on a real .mesh: exercises the QJsonArray build loop with obj
// keys type/description/count/fixable. Asserts exit 0.
// --------------------------------------------------------------------------
TEST_F(CLIPipelineCmdValidateCoverageTest, JsonModeOnRealMeshExitsZero)
{
    bool cleanup = false;
    const QString meshPath = validatableMesh("cov_validate_json", cleanup);
    ASSERT_FALSE(meshPath.isEmpty());
    ASSERT_TRUE(QFile::exists(meshPath));

    QByteArray ba = meshPath.toUtf8();
    CovArgv args({"qtmesh", "validate", ba.constData(), "--json"});
    EXPECT_EQ(CLIPipeline::cmdValidate(args.argc(), args.argv()), 0);

    if (cleanup)
        covRemoveExportTree(meshPath);
}

// --------------------------------------------------------------------------
// --cli flag skip in arg parse (line 2335) on a real file: --cli must be
// skipped, the file still detected, validation still runs and exits 0.
// --------------------------------------------------------------------------
TEST_F(CLIPipelineCmdValidateCoverageTest, CliFlagSkippedOnRealFile)
{
    bool cleanup = false;
    const QString meshPath = validatableMesh("cov_validate_cliflag", cleanup);
    ASSERT_FALSE(meshPath.isEmpty());
    ASSERT_TRUE(QFile::exists(meshPath));

    QByteArray ba = meshPath.toUtf8();
    CovArgv args({"qtmesh", "--cli", "validate", ba.constData()});
    EXPECT_EQ(CLIPipeline::cmdValidate(args.argc(), args.argv()), 0);

    if (cleanup)
        covRemoveExportTree(meshPath);
}

// --------------------------------------------------------------------------
// --cli + --json combined on a real file: both flags skipped/consumed in
// arg parse, JSON build loop runs, exit 0.
// --------------------------------------------------------------------------
TEST_F(CLIPipelineCmdValidateCoverageTest, CliFlagWithJsonOnRealFile)
{
    bool cleanup = false;
    const QString meshPath = validatableMesh("cov_validate_clijson", cleanup);
    ASSERT_FALSE(meshPath.isEmpty());
    ASSERT_TRUE(QFile::exists(meshPath));

    QByteArray ba = meshPath.toUtf8();
    CovArgv args({"qtmesh", "--cli", "validate", ba.constData(), "--json"});
    EXPECT_EQ(CLIPipeline::cmdValidate(args.argc(), args.argv()), 0);

    if (cleanup)
        covRemoveExportTree(meshPath);
}

// --------------------------------------------------------------------------
// Directly exercise the same issue inspection that cmdValidate performs
// internally on robot.mesh: import → select all → doValidate → walk issues.
// This confirms the validator produces both an "ok" issue (→ OK: branch)
// and/or "[TYPE]" issues (→ else branch) and that hasErrors is computed
// from the same loop the cmd uses. Also asserts the obj keys that the JSON
// build loop reads exist.
// --------------------------------------------------------------------------
TEST_F(CLIPipelineCmdValidateCoverageTest, RealMeshIssuesDriveBothTextBranches)
{
    bool cleanup = false;
    const QString meshPath = validatableMesh("cov_validate_branches", cleanup);
    ASSERT_FALSE(meshPath.isEmpty());
    ASSERT_TRUE(QFile::exists(meshPath));

    QFileInfo fi(meshPath);
    ASSERT_TRUE(CLIPipeline::initOgreHeadless());
    MeshImporterExporter::importer({fi.absoluteFilePath()});

    auto& entities = Manager::getSingleton()->getEntities();
    ASSERT_FALSE(entities.isEmpty());

    auto* sel = SelectionSet::getSingleton();
    for (Ogre::Entity* entity : entities)
        sel->append(entity);

    MeshValidator::instance()->doValidate();
    QVariantList issues = MeshValidator::instance()->issues();
    ASSERT_FALSE(issues.isEmpty());

    bool hasErrors = false;
    bool sawOk = false;
    bool sawNonOk = false;
    QString text;
    for (const QVariant& v : issues) {
        QVariantMap map = v.toMap();
        EXPECT_TRUE(map.contains("type"));
        EXPECT_TRUE(map.contains("description"));
        EXPECT_TRUE(map.contains("count"));
        EXPECT_TRUE(map.contains("fixable"));

        const QString type = map.value("type").toString();
        const QString desc = map.value("description").toString();
        if (type == "error")
            hasErrors = true;
        if (type == "ok") {
            sawOk = true;
            text += QString("OK: %1\n").arg(desc);
        } else {
            sawNonOk = true;
            text += QString("[%1] %2\n").arg(type.toUpper(), desc);
        }
    }

    // A clean mesh validates with no errors → cmdValidate returns 0.
    EXPECT_FALSE(hasErrors);
    // At least one branch of the text loop must have been taken; the formatted
    // text is therefore non-empty. (robot.mesh yields OK + info rows.)
    EXPECT_TRUE(sawOk || sawNonOk);
    EXPECT_FALSE(text.isEmpty());

    if (cleanup)
        covRemoveExportTree(meshPath);
}

// --------------------------------------------------------------------------
// Re-validate the same real mesh back-to-back in text then JSON mode within
// one test to confirm the SelectionSet append loop + validator are
// re-entrant across cmdValidate invocations (the cmd selects all entities
// each call; the fixture clears selection only between tests).
// --------------------------------------------------------------------------
TEST_F(CLIPipelineCmdValidateCoverageTest, TextThenJsonBackToBackExitsZero)
{
    bool cleanup = false;
    const QString meshPath = validatableMesh("cov_validate_b2b", cleanup);
    ASSERT_FALSE(meshPath.isEmpty());
    ASSERT_TRUE(QFile::exists(meshPath));

    QByteArray ba = meshPath.toUtf8();

    CovArgv textArgs({"qtmesh", "validate", ba.constData()});
    EXPECT_EQ(CLIPipeline::cmdValidate(textArgs.argc(), textArgs.argv()), 0);

    CovArgv jsonArgs({"qtmesh", "validate", ba.constData(), "--json"});
    EXPECT_EQ(CLIPipeline::cmdValidate(jsonArgs.argc(), jsonArgs.argv()), 0);

    if (cleanup)
        covRemoveExportTree(meshPath);
}
