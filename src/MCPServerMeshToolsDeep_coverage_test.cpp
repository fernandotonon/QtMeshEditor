// Coverage tests for MCPServer mesh tools end-to-end success paths.
//
// Targets two execution paths the existing MCPServerTest suite does NOT cover:
//
//   1. toolLoadMesh success branch (MCPServer.cpp:1126-1132): a MainWindow IS
//      set, the file DOES exist, so mainWindow->importMeshs({path}) runs and the
//      makeSuccessResult("Loaded mesh from: %1") is returned. The existing tests
//      only hit the no-MainWindow and missing-file error branches.
//
//   2. toolGetMeshInfo SELECTION branch (MCPServer.cpp:1146-1199): an entity is
//      selected (sel->getEntitiesCount() > 0) so the per-entity multi-line block
//      is emitted with the "Mesh Information (N entities)" header. The existing
//      tests only cover the no-selection / empty-scene branch.
//
// Distinct suite name (MCPServerMeshToolsDeepCoverageTest) and file-local
// result-text / isError helpers avoid any ODR clash or duplicate registration
// with MCPServer_test.cpp.

#include <gtest/gtest.h>
#include <QApplication>
#include <QThread>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <memory>

#include "MCPServer.h"
#include "Manager.h"
#include "mainwindow.h"
#include "SelectionSet.h"
#include "PrimitiveObject.h"
#include <OgreEntity.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreException.h>
#include "TestHelpers.h"

namespace {

// File-local helpers (distinct names — no ODR clash with MCPServer_test.cpp's
// getResultText / isError which live in that translation unit).
QString deepResultText(const QJsonObject &result)
{
    const QJsonArray content = result["content"].toArray();
    if (content.isEmpty()) return QString();
    return content[0].toObject()["text"].toString();
}

bool deepIsError(const QJsonObject &result)
{
    return result["isError"].toBool(false);
}

class MCPServerMeshToolsDeepCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        server = std::make_unique<MCPServer>();
    }

    void TearDown() override
    {
        if (SelectionSet::getSingletonPtr()) {
            SelectionSet::getSingleton()->clear();
        }
        server.reset();
        Manager::kill();
        if (app) {
            app->processEvents();
        }
    }

    // Copied locally from MCPServerTest — build a MainWindow with retries to
    // tolerate transient GL/context flakiness in CI.
    MainWindow* createMainWindowWithRetries()
    {
        MainWindow* window = nullptr;
        constexpr int kMaxAttempts = 4;
        for (int attempt = 1; attempt <= kMaxAttempts && !window; ++attempt) {
            try {
                window = new MainWindow();
            } catch (...) {
                window = nullptr;
                if (app) app->processEvents();
                QThread::msleep(150 * attempt);
            }
        }
        return window;
    }

    // Create one triangle entity, attach it to a fresh scene node, and select it.
    Ogre::Entity* createAndSelectTriangleEntity(const QString& baseName)
    {
        auto* manager = Manager::getSingletonPtr();
        if (!manager) return nullptr;

        Ogre::MeshPtr mesh = createInMemoryTriangleMesh((baseName + "_mesh").toStdString());
        if (!mesh) return nullptr;

        Ogre::SceneManager* sceneMgr = manager->getSceneMgr();
        if (!sceneMgr) return nullptr;

        Ogre::SceneNode* node = manager->addSceneNode(baseName);
        if (!node) return nullptr;

        Ogre::Entity* entity = sceneMgr->createEntity((baseName + "_entity").toStdString(), mesh);
        if (!entity) return nullptr;

        node->attachObject(entity);
        SelectionSet::getSingleton()->clear();
        SelectionSet::getSingleton()->selectOne(entity);
        if (app) app->processEvents();
        return entity;
    }

    QApplication* app = nullptr;
    std::unique_ptr<MCPServer> server;
};

// ===========================================================================
// toolLoadMesh — success path with a real MainWindow + real on-disk mesh
// ===========================================================================

TEST_F(MCPServerMeshToolsDeepCoverageTest, LoadMeshSuccessWithMainWindow)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "mesh import requires GL (Xvfb in CI)";

    const QString robot = testRobotMeshPath();
    ASSERT_FALSE(robot.isEmpty()) << "robot.mesh not found on disk";

    std::unique_ptr<MainWindow> mainWindow(createMainWindowWithRetries());
    ASSERT_NE(mainWindow.get(), nullptr) << "MainWindow construction failed";
    server->setMainWindow(mainWindow.get());

    QJsonObject args;
    args["path"] = robot;
    QJsonObject result = server->callTool("load_mesh", args);

    EXPECT_FALSE(deepIsError(result));
    const QString text = deepResultText(result);
    EXPECT_TRUE(text.contains("Loaded mesh from:")) << text.toStdString();
    EXPECT_TRUE(text.contains(robot)) << text.toStdString();

    // Drop the MainWindow before TearDown kills the Manager singleton.
    server->setMainWindow(nullptr);
    if (app) app->processEvents();
    mainWindow.reset();
    if (app) app->processEvents();
}

// Sanity: empty path short-circuits before the MainWindow / file checks even
// when a MainWindow is set.
// With a MainWindow set but a non-existent path, the file-not-found branch fires
// (distinct from the no-MainWindow branch covered elsewhere).
// ===========================================================================
// toolGetMeshInfo — SELECTION branch (entity selected → per-entity block)
// ===========================================================================

TEST_F(MCPServerMeshToolsDeepCoverageTest, GetMeshInfoSelectionBranchSingleEntity)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    Ogre::Entity* entity = createAndSelectTriangleEntity("DeepSelInfoA");
    ASSERT_NE(entity, nullptr);

    SelectionSet* sel = SelectionSet::getSingleton();
    ASSERT_NE(sel, nullptr);
    ASSERT_GT(sel->getEntitiesCount(), 0) << "selection branch precondition";

    QJsonObject result = server->callTool("get_mesh_info", QJsonObject());
    EXPECT_FALSE(deepIsError(result));

    const QString text = deepResultText(result);
    // Header: exactly one selected entity.
    EXPECT_TRUE(text.contains("Mesh Information (1 entities)")) << text.toStdString();

    // Per-entity multi-line block fields.
    EXPECT_TRUE(text.contains("Entity:")) << text.toStdString();
    EXPECT_TRUE(text.contains("Mesh:")) << text.toStdString();
    EXPECT_TRUE(text.contains("Vertices:")) << text.toStdString();
    EXPECT_TRUE(text.contains("Triangles:")) << text.toStdString();
    EXPECT_TRUE(text.contains("SubMeshes:")) << text.toStdString();
    EXPECT_TRUE(text.contains("Materials:")) << text.toStdString();
    EXPECT_TRUE(text.contains("Position:")) << text.toStdString();
    EXPECT_TRUE(text.contains("Scale:")) << text.toStdString();

    // The selected entity's name must appear in the block.
    EXPECT_TRUE(text.contains(QString::fromStdString(entity->getName())))
        << text.toStdString();
}

// Selection branch reflects a non-default transform on the parent node, exercising
// the parentNode position/scale read (MCPServer.cpp:1180-1182).
TEST_F(MCPServerMeshToolsDeepCoverageTest, GetMeshInfoSelectionReportsTransform)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    Ogre::Entity* entity = createAndSelectTriangleEntity("DeepSelInfoB");
    ASSERT_NE(entity, nullptr);

    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);
    node->setPosition(3.0f, 4.0f, 5.0f);
    node->setScale(2.0f, 2.0f, 2.0f);
    if (app) app->processEvents();

    QJsonObject result = server->callTool("get_mesh_info", QJsonObject());
    EXPECT_FALSE(deepIsError(result));

    const QString text = deepResultText(result);
    EXPECT_TRUE(text.contains("Mesh Information (1 entities)")) << text.toStdString();
    EXPECT_TRUE(text.contains("Position: 3")) << text.toStdString();
    EXPECT_TRUE(text.contains("Scale: 2")) << text.toStdString();
}

// Multi-entity selection: header count must equal the number of selected entities,
// and the block must list both selected entity names.
TEST_F(MCPServerMeshToolsDeepCoverageTest, GetMeshInfoSelectionBranchTwoEntities)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    Ogre::Entity* first = createAndSelectTriangleEntity("DeepSelInfoC1");
    ASSERT_NE(first, nullptr);

    // Add a second entity and add it to the selection (selectOne clears, so use
    // selectOne then re-add the first; simplest is to build the selection here).
    auto* manager = Manager::getSingletonPtr();
    ASSERT_NE(manager, nullptr);
    Ogre::MeshPtr mesh2 = createInMemoryTriangleMesh("DeepSelInfoC2_mesh");
    ASSERT_TRUE(mesh2);
    Ogre::SceneManager* sceneMgr = manager->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);
    Ogre::SceneNode* node2 = manager->addSceneNode("DeepSelInfoC2");
    ASSERT_NE(node2, nullptr);
    Ogre::Entity* second = sceneMgr->createEntity("DeepSelInfoC2_entity", mesh2);
    ASSERT_NE(second, nullptr);
    node2->attachObject(second);

    SelectionSet* sel = SelectionSet::getSingleton();
    ASSERT_NE(sel, nullptr);
    sel->clear();
    sel->append(first);
    sel->append(second);
    if (app) app->processEvents();
    ASSERT_EQ(sel->getEntitiesCount(), 2);

    QJsonObject result = server->callTool("get_mesh_info", QJsonObject());
    EXPECT_FALSE(deepIsError(result));

    const QString text = deepResultText(result);
    EXPECT_TRUE(text.contains("Mesh Information (2 entities)")) << text.toStdString();
    EXPECT_TRUE(text.contains(QString::fromStdString(first->getName())))
        << text.toStdString();
    EXPECT_TRUE(text.contains(QString::fromStdString(second->getName())))
        << text.toStdString();
}

} // namespace
