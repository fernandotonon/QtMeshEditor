#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <OgreMaterialManager.h>
#include "NormalVisualizer.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

// ===========================================================================
// Integration tests (require Ogre)
// ===========================================================================

class NormalVisualizerIntegrationTest : public ::testing::Test {
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

        // Remove leftover material so createMaterial() runs fresh
        auto existing = Ogre::MaterialManager::getSingleton().getByName(
            "NormalVisualizer/Material");
        if (existing)
            Ogre::MaterialManager::getSingleton().remove(existing);
    }

    void TearDown() override {
        if (!Manager::getSingletonPtr())
            return;
        auto existing = Ogre::MaterialManager::getSingleton().getByName(
            "NormalVisualizer/Material");
        if (existing)
            Ogre::MaterialManager::getSingleton().remove(existing);
        SelectionSet::getSingleton()->clear();
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(50);
    }
};

TEST_F(NormalVisualizerIntegrationTest, MaterialCreatedOnConstruction)
{
    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());

    auto mat = Ogre::MaterialManager::getSingleton().getByName(
        "NormalVisualizer/Material");
    ASSERT_TRUE(mat) << "NormalVisualizer should create its material on construction";

    Ogre::Pass* p = mat->getTechnique(0)->getPass(0);
    ASSERT_NE(p, nullptr);

    EXPECT_FALSE(p->getLightingEnabled());
    EXPECT_TRUE(p->getVertexColourTracking() & Ogre::TVC_DIFFUSE);
    EXPECT_FALSE(p->getDepthWriteEnabled());
    EXPECT_FALSE(p->getDepthCheckEnabled());
}

TEST_F(NormalVisualizerIntegrationTest, MaterialUsesDiffuseNotAmbient)
{
    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());

    auto mat = Ogre::MaterialManager::getSingleton().getByName(
        "NormalVisualizer/Material");
    ASSERT_TRUE(mat);

    Ogre::Pass* p = mat->getTechnique(0)->getPass(0);
    EXPECT_TRUE(p->getVertexColourTracking() & Ogre::TVC_DIFFUSE)
        << "Must use TVC_DIFFUSE for vertex colours to work with lighting disabled (RTSS)";
}

TEST_F(NormalVisualizerIntegrationTest, InitiallyNotVisible)
{
    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    EXPECT_FALSE(visualizer.isVisible());
}

TEST_F(NormalVisualizerIntegrationTest, SetVisibleToggle)
{
    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    visualizer.setVisible(true);
    EXPECT_TRUE(visualizer.isVisible());
    visualizer.setVisible(false);
    EXPECT_FALSE(visualizer.isVisible());
}

TEST_F(NormalVisualizerIntegrationTest, SetVisibleOnEmptySceneNoError)
{
    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    // Should not crash on empty scene
    visualizer.setVisible(true);
    EXPECT_TRUE(visualizer.isVisible());
    visualizer.setVisible(false);
    EXPECT_FALSE(visualizer.isVisible());
}

TEST_F(NormalVisualizerIntegrationTest, DoubleSetVisibleTrueNoError)
{
    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    visualizer.setVisible(true);
    visualizer.setVisible(true); // Should not crash or duplicate
    EXPECT_TRUE(visualizer.isVisible());
    visualizer.setVisible(false);
}

TEST_F(NormalVisualizerIntegrationTest, DestructorCleansUpWhileVisible)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    auto meshPtr = createInMemoryTriangleMesh("NormVizDestructorMesh");
    ASSERT_TRUE(meshPtr);

    Ogre::SceneNode* node = Manager::getSingleton()->getSceneMgr()
        ->getRootSceneNode()->createChildSceneNode("NormVizDestructorNode");
    Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
        "NormVizDestructorEntity", meshPtr);
    node->attachObject(entity);

    {
        NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
        visualizer.setVisible(true);
        // Destructor runs here — should clean up ManualObjects and child nodes
    }

    // Verify the entity's node still exists and the entity is valid
    EXPECT_EQ(entity->getParentSceneNode(), node);

    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

TEST_F(NormalVisualizerIntegrationTest, BuildsOverlayForInMemoryMesh)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    auto meshPtr = createInMemoryTriangleMesh("NormalVisualizerTestMesh");
    ASSERT_TRUE(meshPtr);

    Ogre::SceneNode* node = Manager::getSingleton()->getSceneMgr()
        ->getRootSceneNode()->createChildSceneNode("NormVizTestNode");
    Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
        "NormalVisualizerTestEntity", meshPtr);
    node->attachObject(entity);

    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    visualizer.setVisible(true);

    // The overlay ManualObject should be on a child node, not directly on entity's node
    // So entity's node should still only have 1 attached object (the entity itself)
    // but should have a child node with the ManualObject
    EXPECT_GE(node->numChildren(), 1u);

    visualizer.setVisible(false);

    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

TEST_F(NormalVisualizerIntegrationTest, OverlayCreatedForSkeletalEntity)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    Ogre::Entity* entity = createAnimatedTestEntity("NormVizAnimEntity");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());

    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    visualizer.setVisible(true);

    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);
    EXPECT_GE(node->numChildren(), 1u);

    visualizer.setVisible(false);

    // Clean up
    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}
