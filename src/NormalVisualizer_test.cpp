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

// onEntityCreated signal: when visibility is on, a newly created entity
// should automatically get a normal overlay via the signal connection.
TEST_F(NormalVisualizerIntegrationTest, OnEntityCreatedWhileVisible)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    visualizer.setVisible(true);

    // Create an entity AFTER enabling normals -- the entityCreated signal
    // should cause NormalVisualizer to build an overlay automatically.
    auto meshPtr = createInMemoryTriangleMesh("NormVizSignalMesh");
    ASSERT_TRUE(meshPtr);

    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("NormVizSignalNode");
    Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
        "NormVizSignalEntity", meshPtr);
    node->attachObject(entity);

    // Manually emit the signal (Manager::addSceneNode creates a node but
    // entity attachment + signal is normally done inside Manager::createEntity;
    // here we emit directly to test the slot).
    emit Manager::getSingleton()->entityCreated(entity);

    // The entity's parent node should now have a child node for the overlay
    EXPECT_GE(node->numChildren(), 1u);

    visualizer.setVisible(false);

    // Clean up
    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

// onEntityCreated signal when visibility is off: overlay should NOT be built.
TEST_F(NormalVisualizerIntegrationTest, OnEntityCreatedWhileNotVisible)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    // Visibility is off by default
    ASSERT_FALSE(visualizer.isVisible());

    auto meshPtr = createInMemoryTriangleMesh("NormVizNoVisMesh");
    ASSERT_TRUE(meshPtr);

    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("NormVizNoVisNode");
    Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
        "NormVizNoVisEntity", meshPtr);
    node->attachObject(entity);

    emit Manager::getSingleton()->entityCreated(entity);

    // No overlay should be built when not visible
    EXPECT_EQ(node->numChildren(), 0u);

    // Clean up
    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

// onSceneNodeDestroyed signal with visibility on: overlay should be cleaned up.
TEST_F(NormalVisualizerIntegrationTest, OnSceneNodeDestroyedCleansUpOverlay)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    auto meshPtr = createInMemoryTriangleMesh("NormVizDestroySignalMesh");
    ASSERT_TRUE(meshPtr);

    Ogre::SceneNode* node = Manager::getSingleton()->getSceneMgr()
        ->getRootSceneNode()->createChildSceneNode("NormVizDestroySignalNode");
    Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
        "NormVizDestroySignalEntity", meshPtr);
    node->attachObject(entity);

    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    visualizer.setVisible(true);

    // Verify overlay was built
    ASSERT_GE(node->numChildren(), 1u);

    // Emit the signal to simulate node destruction -- the overlay should be removed
    emit Manager::getSingleton()->sceneNodeDestroyed(node);

    // After the signal, the overlay child node should have been destroyed.
    // The entity itself is still attached (Manager hasn't destroyed it yet),
    // but the overlay ManualObject child node should be gone.
    EXPECT_EQ(node->numChildren(), 0u);

    visualizer.setVisible(false);

    // Clean up
    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

// Verify overlay works with shared vertex data (createInMemoryTriangleMesh uses shared).
TEST_F(NormalVisualizerIntegrationTest, OverlayWithSharedVertexData)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    // createInMemoryTriangleMesh creates a mesh with sharedVertexData and
    // sub->useSharedVertices = true, which exercises the shared vertex path.
    auto meshPtr = createInMemoryTriangleMesh("NormVizSharedMesh");
    ASSERT_TRUE(meshPtr);
    ASSERT_NE(meshPtr->sharedVertexData, nullptr);

    Ogre::SceneNode* node = Manager::getSingleton()->getSceneMgr()
        ->getRootSceneNode()->createChildSceneNode("NormVizSharedNode");
    Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
        "NormVizSharedEntity", meshPtr);
    node->attachObject(entity);

    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    visualizer.setVisible(true);

    // Shared vertex data path should produce an overlay child node
    EXPECT_GE(node->numChildren(), 1u);

    visualizer.setVisible(false);

    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

// Multiple entities: show normals for 3+ entities simultaneously.
TEST_F(NormalVisualizerIntegrationTest, MultipleEntitiesSimultaneously)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    const int NUM_ENTITIES = 4;
    std::vector<Ogre::Entity*> entities;
    std::vector<Ogre::SceneNode*> nodes;

    for (int i = 0; i < NUM_ENTITIES; ++i) {
        std::string suffix = std::to_string(i);
        auto meshPtr = createInMemoryTriangleMesh("NormVizMultiMesh" + suffix);
        ASSERT_TRUE(meshPtr);

        Ogre::SceneNode* node = Manager::getSingleton()->getSceneMgr()
            ->getRootSceneNode()->createChildSceneNode("NormVizMultiNode" + suffix);
        Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
            "NormVizMultiEntity" + suffix, meshPtr);
        node->attachObject(entity);

        entities.push_back(entity);
        nodes.push_back(node);
    }

    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    visualizer.setVisible(true);

    // All entities should have overlay child nodes
    for (int i = 0; i < NUM_ENTITIES; ++i) {
        EXPECT_GE(nodes[i]->numChildren(), 1u)
            << "Entity " << i << " should have an overlay child node";
    }

    visualizer.setVisible(false);

    // After hiding, all overlay child nodes should be gone
    for (int i = 0; i < NUM_ENTITIES; ++i) {
        EXPECT_EQ(nodes[i]->numChildren(), 0u)
            << "Entity " << i << " overlay should be removed after hide";
    }

    // Clean up
    for (int i = 0; i < NUM_ENTITIES; ++i) {
        nodes[i]->detachObject(entities[i]);
        Manager::getSingleton()->getSceneMgr()->destroyEntity(entities[i]);
        Manager::getSingleton()->getSceneMgr()->destroySceneNode(nodes[i]);
    }
}

// Exercise updateAnimatedOverlays via the timer path for skeletal entities.
TEST_F(NormalVisualizerIntegrationTest, UpdateAnimatedOverlaysViaTimer)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    Ogre::Entity* entity = createAnimatedTestEntity("NormVizAnimTimer");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());

    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    visualizer.setVisible(true);

    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);
    EXPECT_GE(node->numChildren(), 1u);

    // Enable an animation state to exercise the animated overlay update path
    auto* animState = entity->getAnimationState("TestAnim");
    ASSERT_NE(animState, nullptr);
    animState->setEnabled(true);
    animState->setLoop(true);
    animState->addTime(0.25f);

    // Let the timer fire a few times to exercise updateAnimatedOverlays
    for (int i = 0; i < 3; ++i) {
        QThread::msleep(30);
        if (app) app->processEvents();
    }

    // The overlay should still be intact after animation updates
    EXPECT_GE(node->numChildren(), 1u);

    // Advance animation more
    animState->addTime(0.5f);
    QThread::msleep(30);
    if (app) app->processEvents();

    EXPECT_GE(node->numChildren(), 1u);

    visualizer.setVisible(false);

    // Clean up
    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

// Multiple show/hide/show cycles to exercise repeated toggling with entities.
TEST_F(NormalVisualizerIntegrationTest, MultipleShowHideShowCycles)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    auto meshPtr = createInMemoryTriangleMesh("NormVizCycleMesh");
    ASSERT_TRUE(meshPtr);

    Ogre::SceneNode* node = Manager::getSingleton()->getSceneMgr()
        ->getRootSceneNode()->createChildSceneNode("NormVizCycleNode");
    Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
        "NormVizCycleEntity", meshPtr);
    node->attachObject(entity);

    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());

    for (int i = 0; i < 5; ++i) {
        visualizer.setVisible(true);
        EXPECT_TRUE(visualizer.isVisible());
        EXPECT_GE(node->numChildren(), 1u)
            << "Cycle " << i << ": overlay should be built when visible";

        visualizer.setVisible(false);
        EXPECT_FALSE(visualizer.isVisible());
        EXPECT_EQ(node->numChildren(), 0u)
            << "Cycle " << i << ": overlay should be destroyed when hidden";
    }

    // Clean up
    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

// Add entity while visible, then remove it -- exercises full lifecycle.
TEST_F(NormalVisualizerIntegrationTest, AddEntityWhileVisibleThenRemove)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    visualizer.setVisible(true);

    // Create entity while normals are visible
    auto meshPtr = createInMemoryTriangleMesh("NormVizAddRemoveMesh");
    ASSERT_TRUE(meshPtr);

    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("NormVizAddRemoveNode");
    Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
        "NormVizAddRemoveEntity", meshPtr);
    node->attachObject(entity);

    // Emit entityCreated signal to trigger overlay build
    emit Manager::getSingleton()->entityCreated(entity);
    EXPECT_GE(node->numChildren(), 1u);

    // Now remove: emit sceneNodeDestroyed to clean up overlay
    emit Manager::getSingleton()->sceneNodeDestroyed(node);
    EXPECT_EQ(node->numChildren(), 0u);

    // Re-add: emit entityCreated again to rebuild overlay
    emit Manager::getSingleton()->entityCreated(entity);
    EXPECT_GE(node->numChildren(), 1u);

    visualizer.setVisible(false);

    // Clean up
    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

// Animated entity overlay update with animation state changes
TEST_F(NormalVisualizerIntegrationTest, AnimatedOverlayWithStateChanges)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    Ogre::Entity* entity = createAnimatedTestEntity("NormVizAnimState");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());

    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    visualizer.setVisible(true);

    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);
    EXPECT_GE(node->numChildren(), 1u);

    // Enable animation
    auto* animState = entity->getAnimationState("TestAnim");
    ASSERT_NE(animState, nullptr);
    animState->setEnabled(true);

    // Process several timer updates at different animation times
    for (float t = 0.0f; t <= 1.0f; t += 0.1f) {
        animState->setTimePosition(t);
        QThread::msleep(20);
        if (app) app->processEvents();
    }

    // Disable animation
    animState->setEnabled(false);
    QThread::msleep(20);
    if (app) app->processEvents();

    // Re-enable and advance
    animState->setEnabled(true);
    animState->setTimePosition(0.5f);
    QThread::msleep(20);
    if (app) app->processEvents();

    // Overlay should still be intact
    EXPECT_GE(node->numChildren(), 1u);

    visualizer.setVisible(false);

    // Clean up
    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

// Toggle normals while adding/removing entities via signals.
TEST_F(NormalVisualizerIntegrationTest, ToggleWhileAddingRemovingEntities)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    NormalVisualizer visualizer(Manager::getSingleton()->getSceneMgr());

    // Start with normals ON
    visualizer.setVisible(true);

    // Add first entity
    auto mesh1 = createInMemoryTriangleMesh("NormVizToggleMesh1");
    ASSERT_TRUE(mesh1);
    Ogre::SceneNode* node1 = Manager::getSingleton()->getSceneMgr()
        ->getRootSceneNode()->createChildSceneNode("NormVizToggleNode1");
    Ogre::Entity* entity1 = Manager::getSingleton()->getSceneMgr()->createEntity(
        "NormVizToggleEntity1", mesh1);
    node1->attachObject(entity1);
    emit Manager::getSingleton()->entityCreated(entity1);
    EXPECT_GE(node1->numChildren(), 1u);

    // Turn off normals
    visualizer.setVisible(false);
    EXPECT_EQ(node1->numChildren(), 0u);

    // Add second entity while normals are OFF
    auto mesh2 = createInMemoryTriangleMesh("NormVizToggleMesh2");
    ASSERT_TRUE(mesh2);
    Ogre::SceneNode* node2 = Manager::getSingleton()->getSceneMgr()
        ->getRootSceneNode()->createChildSceneNode("NormVizToggleNode2");
    Ogre::Entity* entity2 = Manager::getSingleton()->getSceneMgr()->createEntity(
        "NormVizToggleEntity2", mesh2);
    node2->attachObject(entity2);
    emit Manager::getSingleton()->entityCreated(entity2);
    // No overlay because normals are off
    EXPECT_EQ(node2->numChildren(), 0u);

    // Turn normals back ON -- both entities should get overlays
    visualizer.setVisible(true);
    EXPECT_GE(node1->numChildren(), 1u);
    EXPECT_GE(node2->numChildren(), 1u);

    // Simulate removing node1 via signal
    emit Manager::getSingleton()->sceneNodeDestroyed(node1);
    EXPECT_EQ(node1->numChildren(), 0u);
    // node2 should still have its overlay
    EXPECT_GE(node2->numChildren(), 1u);

    visualizer.setVisible(false);

    // Clean up
    node1->detachObject(entity1);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity1);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node1);

    node2->detachObject(entity2);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity2);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node2);
}
