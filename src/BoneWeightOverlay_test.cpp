#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <OgreMaterialManager.h>
#include <OgreException.h>
#include "BoneWeightOverlay.h"
#include "MeshImporterExporter.h"
#include "SelectionSet.h"
#include "Manager.h"
#include "TestHelpers.h"

// ===========================================================================
// Integration test: verify BoneWeightOverlay material (requires mesh loading)
// ===========================================================================

class BoneWeightOverlayIntegrationTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    Ogre::Entity* entity = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();

        if (!canLoadMeshFiles())
            GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

        // Remove leftover material so createMaterial() runs fresh
        auto existing = Ogre::MaterialManager::getSingleton().getByName(
            "BoneWeightOverlay/Material");
        if (existing)
            Ogre::MaterialManager::getSingleton().remove(existing);

        QStringList uris{"./media/models/robot.mesh"};
        try { MeshImporterExporter::importer(uris); }
        catch (...) { GTEST_SKIP() << "Skipping: failed to import robot.mesh"; }

        if (Manager::getSingleton()->getEntities().isEmpty())
            GTEST_SKIP() << "Skipping: no entity after import";

        entity = Manager::getSingleton()->getEntities().last();
        ASSERT_NE(entity, nullptr);
    }

    void TearDown() override {
        if (!Manager::getSingletonPtr())
            return;
        auto existing = Ogre::MaterialManager::getSingleton().getByName(
            "BoneWeightOverlay/Material");
        if (existing)
            Ogre::MaterialManager::getSingleton().remove(existing);
        SelectionSet::getSingleton()->clear();
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(50);
    }
};

// Verify that BoneWeightOverlay creates its material with TVC_DIFFUSE.
// TVC_AMBIENT does NOT work with lighting disabled on RTSS (macOS Metal/GL3+),
// causing the overlay to display a solid colour instead of vertex-coloured heat map.
TEST_F(BoneWeightOverlayIntegrationTest, MaterialUsesDiffuseVertexColourTracking)
{
    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());

    auto mat = Ogre::MaterialManager::getSingleton().getByName(
        "BoneWeightOverlay/Material");
    ASSERT_TRUE(mat) << "BoneWeightOverlay should create its material on construction";

    Ogre::Pass* p = mat->getTechnique(0)->getPass(0);
    ASSERT_NE(p, nullptr);

    EXPECT_FALSE(p->getLightingEnabled());
    EXPECT_TRUE(p->getVertexColourTracking() & Ogre::TVC_DIFFUSE)
        << "Vertex colour tracking MUST include TVC_DIFFUSE for colours "
           "to display with lighting disabled (RTSS requirement)";
}

// ===========================================================================
// weightToColor tests (no Ogre required)
// ===========================================================================

TEST(BoneWeightOverlayTest, WeightToColorAtZeroIsBlue)
{
    auto color = BoneWeightOverlay::weightToColor(0.0f);
    EXPECT_NEAR(color.r, 0.0f, 1e-5f);
    EXPECT_NEAR(color.g, 0.0f, 1e-5f);
    EXPECT_NEAR(color.b, 1.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorAtQuarterIsCyan)
{
    auto color = BoneWeightOverlay::weightToColor(0.25f);
    EXPECT_NEAR(color.r, 0.0f, 1e-5f);
    EXPECT_NEAR(color.g, 1.0f, 1e-5f);
    EXPECT_NEAR(color.b, 1.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorAtHalfIsGreen)
{
    auto color = BoneWeightOverlay::weightToColor(0.5f);
    EXPECT_NEAR(color.r, 0.0f, 1e-5f);
    EXPECT_NEAR(color.g, 1.0f, 1e-5f);
    EXPECT_NEAR(color.b, 0.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorAtThreeQuartersIsYellow)
{
    auto color = BoneWeightOverlay::weightToColor(0.75f);
    EXPECT_NEAR(color.r, 1.0f, 1e-5f);
    EXPECT_NEAR(color.g, 1.0f, 1e-5f);
    EXPECT_NEAR(color.b, 0.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorAtOneIsRed)
{
    auto color = BoneWeightOverlay::weightToColor(1.0f);
    EXPECT_NEAR(color.r, 1.0f, 1e-5f);
    EXPECT_NEAR(color.g, 0.0f, 1e-5f);
    EXPECT_NEAR(color.b, 0.0f, 1e-5f);
    EXPECT_NEAR(color.a, 0.7f, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorClampsNegative)
{
    auto color = BoneWeightOverlay::weightToColor(-0.5f);
    auto colorZero = BoneWeightOverlay::weightToColor(0.0f);
    EXPECT_NEAR(color.r, colorZero.r, 1e-5f);
    EXPECT_NEAR(color.g, colorZero.g, 1e-5f);
    EXPECT_NEAR(color.b, colorZero.b, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorClampsAboveOne)
{
    auto color = BoneWeightOverlay::weightToColor(1.5f);
    auto colorOne = BoneWeightOverlay::weightToColor(1.0f);
    EXPECT_NEAR(color.r, colorOne.r, 1e-5f);
    EXPECT_NEAR(color.g, colorOne.g, 1e-5f);
    EXPECT_NEAR(color.b, colorOne.b, 1e-5f);
}

TEST(BoneWeightOverlayTest, WeightToColorMidpointsInterpolate)
{
    // At 0.125 (halfway between blue and cyan), green should be 0.5
    auto color = BoneWeightOverlay::weightToColor(0.125f);
    EXPECT_NEAR(color.r, 0.0f, 1e-5f);
    EXPECT_NEAR(color.g, 0.5f, 1e-5f);
    EXPECT_NEAR(color.b, 1.0f, 1e-5f);

    // At 0.625 (halfway between green and yellow), red should be 0.5
    auto color2 = BoneWeightOverlay::weightToColor(0.625f);
    EXPECT_NEAR(color2.r, 0.5f, 1e-5f);
    EXPECT_NEAR(color2.g, 1.0f, 1e-5f);
    EXPECT_NEAR(color2.b, 0.0f, 1e-5f);
}

// ===========================================================================
// In-memory entity tests (require Ogre + GL context, no robot.mesh needed)
// ===========================================================================

class BoneWeightOverlayInMemoryTest : public ::testing::Test {
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

        if (!canLoadMeshFiles())
            GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

        // Remove leftover material so createMaterial() runs fresh
        auto existing = Ogre::MaterialManager::getSingleton().getByName(
            "BoneWeightOverlay/Material");
        if (existing)
            Ogre::MaterialManager::getSingleton().remove(existing);
    }

    void TearDown() override {
        if (!Manager::getSingletonPtr())
            return;
        auto existing = Ogre::MaterialManager::getSingleton().getByName(
            "BoneWeightOverlay/Material");
        if (existing)
            Ogre::MaterialManager::getSingleton().remove(existing);
        SelectionSet::getSingleton()->clear();
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(50);
    }
};

// setVisible(true) starts the update timer and builds the overlay;
// setVisible(false) stops the timer and destroys it.
TEST_F(BoneWeightOverlayInMemoryTest, SetVisibleTogglesTimerAndOverlay)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_VisToggle");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());

    EXPECT_FALSE(overlay.isVisible());

    overlay.setVisible(true);
    EXPECT_TRUE(overlay.isVisible());

    // The overlay ManualObject should now be attached to the entity's parent node
    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);
    EXPECT_GE(node->numAttachedObjects(), 2u)
        << "Entity + overlay ManualObject should both be attached";

    overlay.setVisible(false);
    EXPECT_FALSE(overlay.isVisible());

    // After hiding, the overlay ManualObject should have been destroyed,
    // leaving only the entity attached
    EXPECT_EQ(node->numAttachedObjects(), 1u);
}

// setSelectedBone with a valid bone index rebuilds the overlay when visible.
TEST_F(BoneWeightOverlayInMemoryTest, SetSelectedBoneWithValidIndex)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_BoneValid");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());
    ASSERT_GE(entity->getSkeleton()->getNumBones(), 2u);

    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());
    overlay.setVisible(true);

    // Select bone index 0 (Root bone)
    overlay.setSelectedBone(0);
    EXPECT_TRUE(overlay.isVisible());

    // Select bone index 1 (Child bone -- all vertices are assigned to this bone)
    overlay.setSelectedBone(1);
    EXPECT_TRUE(overlay.isVisible());

    // Verify the overlay is still attached
    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);
    EXPECT_GE(node->numAttachedObjects(), 2u);

    overlay.setVisible(false);
}

// setSelectedBone with an out-of-range index should not crash.
// The overlay just shows zero weight (blue) for all vertices.
TEST_F(BoneWeightOverlayInMemoryTest, SetSelectedBoneWithInvalidIndex)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_BoneInvalid");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());
    overlay.setVisible(true);

    // Use a bone index far beyond the skeleton's bone count
    overlay.setSelectedBone(999);
    EXPECT_TRUE(overlay.isVisible());

    // The overlay should still be functional (showing zero-weight colours)
    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);
    EXPECT_GE(node->numAttachedObjects(), 2u);

    overlay.setVisible(false);
}

// Build overlay on an in-memory animated entity (using createAnimatedTestEntity).
TEST_F(BoneWeightOverlayInMemoryTest, BuildOverlayOnAnimatedEntity)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_AnimBuild");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());

    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());
    overlay.setVisible(true);

    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);

    // Should have both the entity and the overlay ManualObject
    EXPECT_GE(node->numAttachedObjects(), 2u);

    // Select the bone that has assignments (bone index 1 = "Child")
    overlay.setSelectedBone(1);

    // Overlay should still be attached after bone selection change
    EXPECT_GE(node->numAttachedObjects(), 2u);

    overlay.setVisible(false);
}

// Destroying the overlay (via destructor) cleans up while visible.
TEST_F(BoneWeightOverlayInMemoryTest, DestroyOverlayOnDeselect)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_Destroy");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);

    {
        BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());
        overlay.setVisible(true);
        EXPECT_GE(node->numAttachedObjects(), 2u);
        // Destructor runs here while overlay is visible -- should clean up
    }

    // After destruction, only the entity should remain
    EXPECT_EQ(node->numAttachedObjects(), 1u);
    // Entity should still be valid
    EXPECT_EQ(entity->getParentSceneNode(), node);
}

// Multiple show/hide cycles should not leak ManualObjects or crash.
TEST_F(BoneWeightOverlayInMemoryTest, MultipleShowHideCycles)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_MultiCycle");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());
    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);

    for (int i = 0; i < 5; ++i) {
        overlay.setVisible(true);
        EXPECT_TRUE(overlay.isVisible());
        EXPECT_GE(node->numAttachedObjects(), 2u)
            << "Cycle " << i << ": overlay should be attached when visible";

        overlay.setVisible(false);
        EXPECT_FALSE(overlay.isVisible());
        EXPECT_EQ(node->numAttachedObjects(), 1u)
            << "Cycle " << i << ": only entity should remain after hide";
    }
}

// Verify that BoneWeightOverlay does not crash with a non-skeletal entity.
// A non-skeletal entity has no bones, so the overlay should build with
// zero-weight colours for all vertices.
TEST_F(BoneWeightOverlayInMemoryTest, NonSkeletalEntityDoesNotCrash)
{
    auto meshPtr = createInMemoryTriangleMesh("BWO_NonSkelMesh");
    ASSERT_TRUE(meshPtr);

    Ogre::SceneNode* node = Manager::getSingleton()->getSceneMgr()
        ->getRootSceneNode()->createChildSceneNode("BWO_NonSkelNode");
    Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
        "BWO_NonSkelEntity", meshPtr);
    node->attachObject(entity);

    ASSERT_FALSE(entity->hasSkeleton());

    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());
    overlay.setVisible(true);
    EXPECT_TRUE(overlay.isVisible());

    // Even without a skeleton, building the overlay should succeed
    // (all vertices get zero weight = blue)
    EXPECT_GE(node->numAttachedObjects(), 2u);

    overlay.setSelectedBone(0);
    EXPECT_TRUE(overlay.isVisible());

    overlay.setVisible(false);
    EXPECT_EQ(node->numAttachedObjects(), 1u);

    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

// Timer-based update positions: enable overlay, wait for timer to fire,
// verify overlay positions update for animated entity.
TEST_F(BoneWeightOverlayInMemoryTest, TimerBasedUpdatePositions)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_TimerUpdate");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());

    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());
    overlay.setVisible(true);

    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);
    EXPECT_GE(node->numAttachedObjects(), 2u);

    // Enable animation to give the timer something to update
    auto* animState = entity->getAnimationState("TestAnim");
    ASSERT_NE(animState, nullptr);
    animState->setEnabled(true);
    animState->setLoop(true);
    animState->addTime(0.25f);

    // Wait for the timer to fire (timer interval is 0ms, so processEvents should trigger it)
    for (int i = 0; i < 5; ++i) {
        QThread::msleep(10);
        if (app) app->processEvents();
    }

    // The overlay should still be intact after timer-driven updates
    EXPECT_TRUE(overlay.isVisible());
    EXPECT_GE(node->numAttachedObjects(), 2u);

    overlay.setVisible(false);
}

// pollBoneSelection: when visible, the timer polls for bone selection changes.
TEST_F(BoneWeightOverlayInMemoryTest, PollBoneSelectionChanges)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_PollBone");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());
    ASSERT_GE(entity->getSkeleton()->getNumBones(), 2u);

    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());
    overlay.setVisible(true);

    // Simulate bone selection by setting user object bindings (this is what pollBoneSelection reads)
    auto* skeleton = entity->getSkeleton();
    auto* bone0 = skeleton->getBone(0);
    bone0->getUserObjectBindings().setUserAny("selected", Ogre::Any(true));

    // Let timer fire to pick up the bone selection
    for (int i = 0; i < 5; ++i) {
        QThread::msleep(10);
        if (app) app->processEvents();
    }

    // Overlay should still be valid
    EXPECT_TRUE(overlay.isVisible());

    // Clear bone 0 selection and select bone 1
    bone0->getUserObjectBindings().setUserAny("selected", Ogre::Any(false));
    auto* bone1 = skeleton->getBone(1);
    bone1->getUserObjectBindings().setUserAny("selected", Ogre::Any(true));

    // Let timer fire again
    for (int i = 0; i < 5; ++i) {
        QThread::msleep(10);
        if (app) app->processEvents();
    }

    EXPECT_TRUE(overlay.isVisible());

    // Clean up bone bindings
    bone1->getUserObjectBindings().setUserAny("selected", Ogre::Any(false));

    overlay.setVisible(false);
}

// Multiple rapid bone selection changes while visible.
TEST_F(BoneWeightOverlayInMemoryTest, RapidBoneSelectionChanges)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_RapidBone");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());
    ASSERT_GE(entity->getSkeleton()->getNumBones(), 2u);

    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());
    overlay.setVisible(true);

    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);

    // Rapidly switch between bones
    for (int i = 0; i < 10; ++i) {
        overlay.setSelectedBone(static_cast<unsigned short>(i % 2));
        if (app) app->processEvents();
    }

    // The overlay should still be functional after rapid switches
    EXPECT_TRUE(overlay.isVisible());
    EXPECT_GE(node->numAttachedObjects(), 2u);

    overlay.setVisible(false);
    EXPECT_EQ(node->numAttachedObjects(), 1u);
}

// setVisible(true) followed immediately by setVisible(true) again should
// not create duplicate overlays.
TEST_F(BoneWeightOverlayInMemoryTest, DoubleSetVisibleTrueNoDuplicate)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_DblShow");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());
    Ogre::SceneNode* node = entity->getParentSceneNode();
    ASSERT_NE(node, nullptr);

    overlay.setVisible(true);
    unsigned short countAfterFirst = node->numAttachedObjects();

    overlay.setVisible(true);
    unsigned short countAfterSecond = node->numAttachedObjects();

    // The second setVisible(true) calls destroyOverlay then buildOverlay,
    // so the count should be the same (no leaked ManualObjects)
    EXPECT_EQ(countAfterFirst, countAfterSecond);

    overlay.setVisible(false);
}
