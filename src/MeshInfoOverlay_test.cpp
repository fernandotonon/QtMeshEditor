#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "MeshInfoOverlay.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

// ===========================================================================
// Pure formatting tests (no Ogre needed)
// ===========================================================================

TEST(MeshInfoOverlayFormat, EmptyList)
{
    QList<Ogre::Entity*> empty;
    QString result = MeshInfoOverlay::formatStats(empty, false);
    EXPECT_EQ(result, "No meshes");
}

TEST(MeshInfoOverlayFormat, EmptyListSelection)
{
    QList<Ogre::Entity*> empty;
    QString result = MeshInfoOverlay::formatStats(empty, true);
    EXPECT_EQ(result, "No meshes");
}

TEST(MeshInfoOverlayFormat, NullEntityInList)
{
    QList<Ogre::Entity*> withNull;
    withNull << nullptr;

    QString result = MeshInfoOverlay::formatStats(withNull, false);
    // Size is 1, so a header is generated, but null entity is skipped
    EXPECT_FALSE(result.isEmpty());
    EXPECT_TRUE(result.contains("Verts:"));
}

// ===========================================================================
// Integration tests (require Ogre for entity creation)
// ===========================================================================

class MeshInfoOverlayIntegrationTest : public ::testing::Test {
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
        if (!Manager::getSingletonPtr())
            return;
        SelectionSet::getSingleton()->clear();
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(50);
    }
};

TEST_F(MeshInfoOverlayIntegrationTest, FormatStatsSingleEntity)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    auto meshPtr = createInMemoryTriangleMesh("MeshInfoSingleMesh");
    ASSERT_TRUE(meshPtr);

    Ogre::SceneNode* node = Manager::getSingleton()->getSceneMgr()
        ->getRootSceneNode()->createChildSceneNode("MeshInfoSingleNode");
    Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
        "MeshInfoSingleEntity", meshPtr);
    node->attachObject(entity);

    QList<Ogre::Entity*> entities;
    entities << entity;

    QString result = MeshInfoOverlay::formatStats(entities, false);

    // Should contain the mesh name
    EXPECT_TRUE(result.contains("MeshInfoSingleMesh"))
        << "Result: " << result.toStdString();

    // Should contain vertex/triangle info
    EXPECT_TRUE(result.contains("Verts:")) << "Result: " << result.toStdString();
    EXPECT_TRUE(result.contains("Tris:")) << "Result: " << result.toStdString();

    // 3 vertices, 1 triangle
    EXPECT_TRUE(result.contains("3")) << "Result: " << result.toStdString();
    EXPECT_TRUE(result.contains("1")) << "Result: " << result.toStdString();

    // Clean up
    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

TEST_F(MeshInfoOverlayIntegrationTest, FormatStatsMultipleEntitiesScene)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    std::vector<Ogre::Entity*> entities;
    std::vector<Ogre::SceneNode*> nodes;

    for (int i = 0; i < 3; ++i) {
        std::string suffix = std::to_string(i);
        auto meshPtr = createInMemoryTriangleMesh("MeshInfoMultiMesh" + suffix);
        ASSERT_TRUE(meshPtr);

        Ogre::SceneNode* node = Manager::getSingleton()->getSceneMgr()
            ->getRootSceneNode()->createChildSceneNode("MeshInfoMultiNode" + suffix);
        Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
            "MeshInfoMultiEntity" + suffix, meshPtr);
        node->attachObject(entity);

        entities.push_back(entity);
        nodes.push_back(node);
    }

    QList<Ogre::Entity*> entityList;
    for (auto* e : entities) entityList << e;

    // Not a selection -- should say "Scene"
    QString result = MeshInfoOverlay::formatStats(entityList, false);
    EXPECT_TRUE(result.contains("Scene (3 meshes)"))
        << "Result: " << result.toStdString();

    // As selection -- should say "Selected"
    QString selResult = MeshInfoOverlay::formatStats(entityList, true);
    EXPECT_TRUE(selResult.contains("Selected (3 meshes)"))
        << "Result: " << selResult.toStdString();

    // Aggregated: 3 entities x 3 verts = 9 verts, 3 tris
    EXPECT_TRUE(result.contains("9")) << "Expected 9 vertices. Result: " << result.toStdString();

    // Clean up
    for (size_t i = 0; i < entities.size(); ++i) {
        nodes[i]->detachObject(entities[i]);
        Manager::getSingleton()->getSceneMgr()->destroyEntity(entities[i]);
        Manager::getSingleton()->getSceneMgr()->destroySceneNode(nodes[i]);
    }
}

TEST_F(MeshInfoOverlayIntegrationTest, FormatStatsWithSkeleton)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    Ogre::Entity* entity = createAnimatedTestEntity("MeshInfoAnimEntity");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());

    QList<Ogre::Entity*> entities;
    entities << entity;

    QString result = MeshInfoOverlay::formatStats(entities, false);

    // Should show bone and animation info
    EXPECT_TRUE(result.contains("Bones:")) << "Result: " << result.toStdString();
    EXPECT_TRUE(result.contains("Anims:")) << "Result: " << result.toStdString();

    // Clean up
    Ogre::SceneNode* node = entity->getParentSceneNode();
    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}
