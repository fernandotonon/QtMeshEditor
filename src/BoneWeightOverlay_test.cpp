#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <OgreMaterialManager.h>
#include <OgreException.h>
#include "BoneWeightOverlay.h"
#include "MeshImporterExporter.h"
#include "AnimationWidget.h"
#include "SelectionSet.h"
#include "SkinWeightController.h"
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
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        ASSERT_TRUE(canLoadMeshFiles()) << "mesh loading requires GL (Xvfb in CI)";
        // Remove leftover material so createMaterial() runs fresh
        auto existing = Ogre::MaterialManager::getSingleton().getByName(
            "BoneWeightOverlay/Material");
        if (existing)
            Ogre::MaterialManager::getSingleton().remove(existing);

        const QString robotPath = testRobotMeshPath();
        ASSERT_FALSE(robotPath.isEmpty()) << "robot.mesh not found under media/models";
        QStringList uris{robotPath};
        ASSERT_NO_THROW(MeshImporterExporter::importer(uris));
        ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
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
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        ASSERT_TRUE(canLoadMeshFiles()) << "mesh loading requires GL (Xvfb in CI)";
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
    ASSERT_NE(entity, nullptr);

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
    ASSERT_NE(entity, nullptr);

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
    ASSERT_NE(entity, nullptr);

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
// Destroying the overlay (via destructor) cleans up while visible.
TEST_F(BoneWeightOverlayInMemoryTest, DestroyOverlayOnDeselect)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_Destroy");
    ASSERT_NE(entity, nullptr);

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
    ASSERT_NE(entity, nullptr);

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
    ASSERT_NE(entity, nullptr);

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
    ASSERT_NE(entity, nullptr);

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
// setVisible(true) followed immediately by setVisible(true) again should
// not create duplicate overlays.
TEST_F(BoneWeightOverlayInMemoryTest, DoubleSetVisibleTrueNoDuplicate)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_DblShow");
    ASSERT_NE(entity, nullptr);

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

// --- live colour refresh (Skel Slice D, #558) ------------------------------

// refreshColours() exists so a weight-paint stroke is visible AS it is painted.
// The overlay caches colours at build time, so the real risk is a refresh that
// runs without restamping anything. Assert the CACHED COLOUR ACTUALLY CHANGES
// when the underlying assignment changes — a crash-free no-op would pass a
// weaker test while leaving the user staring at a frozen heat map.
TEST_F(BoneWeightOverlayInMemoryTest, RefreshColoursRestampsAfterAWeightEdit)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_RefreshColours");
    ASSERT_NE(entity, nullptr);

    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());
    overlay.setSelectedBone(1);
    overlay.setVisible(true);

    ASSERT_FALSE(overlay.sectionColours().empty());
    ASSERT_FALSE(overlay.sectionColours()[0].empty());
    const Ogre::ColourValue before = overlay.sectionColours()[0][0];

    // The fixture weights every vertex 1.0 to bone 1 (full red). Drop vertex 0
    // to a low weight, which must read back as a distinctly cooler colour.
    Ogre::MeshPtr mesh = entity->getMesh();
    ASSERT_TRUE(mesh);
    const auto saved = mesh->getBoneAssignments();
    mesh->clearBoneAssignments();
    for (const auto& kv : saved) {
        Ogre::VertexBoneAssignment vba = kv.second;
        if (vba.vertexIndex == 0 && vba.boneIndex == 1)
            vba.weight = 0.1f;
        mesh->addBoneAssignment(vba);
    }

    overlay.refreshColours();

    const Ogre::ColourValue after = overlay.sectionColours()[0][0];
    EXPECT_NE(before.r, after.r) << "refreshColours() must restamp the cache";
    EXPECT_LT(after.r, before.r) << "a lower weight must read cooler (less red)";
    EXPECT_NEAR(after.r, BoneWeightOverlay::weightToColor(0.1f).r, 1e-5f);
}

// --- per-vertex dots (Skel Slice D, #558) ---------------------------------

// The dots exist to give a depth cue the depth-off heat map cannot. Assert the
// toggle actually creates/destroys the ManualObject, and that it stays off when
// the overlay itself is hidden (no stray object left in the scene).
TEST_F(BoneWeightOverlayInMemoryTest, ShowVerticesCreatesAndDestroysThePointObject)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_VertDots");
    ASSERT_NE(entity, nullptr);
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();

    BoneWeightOverlay overlay(entity, sceneMgr);
    overlay.setSelectedBone(1);
    overlay.setVisible(true);

    EXPECT_FALSE(overlay.showVertices()) << "dots are opt-in";

    overlay.setShowVertices(true);
    EXPECT_TRUE(overlay.showVertices());
    const std::string objName =
        sceneMgr->getName() + "/BoneWeightVertices/" + entity->getName();
    EXPECT_TRUE(sceneMgr->hasManualObject(objName))
        << "enabling dots must emit a point-list object";

    overlay.setShowVertices(false);
    EXPECT_FALSE(overlay.showVertices());
    EXPECT_FALSE(sceneMgr->hasManualObject(objName))
        << "disabling dots must remove the object, not just hide it";
}

// Turning the whole overlay off must take the dots with it, or they linger on a
// mesh whose heat map is gone.
TEST_F(BoneWeightOverlayInMemoryTest, HidingTheOverlayAlsoDropsTheVertexDots)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_VertDotsHide");
    ASSERT_NE(entity, nullptr);
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();

    BoneWeightOverlay overlay(entity, sceneMgr);
    overlay.setSelectedBone(1);
    overlay.setVisible(true);
    overlay.setShowVertices(true);

    const std::string objName =
        sceneMgr->getName() + "/BoneWeightVertices/" + entity->getName();
    ASSERT_TRUE(sceneMgr->hasManualObject(objName));

    overlay.setVisible(false);
    EXPECT_FALSE(sceneMgr->hasManualObject(objName))
        << "hiding the overlay must tear down the dots too";
}

// The dots are depth-TESTED while the heat map is not — that asymmetry is the
// entire point of the feature, so pin it.
TEST_F(BoneWeightOverlayInMemoryTest, VertexDotMaterialIsDepthTestedUnlikeTheHeatMap)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_VertDotsDepth");
    ASSERT_NE(entity, nullptr);

    BoneWeightOverlay overlay(entity, Manager::getSingleton()->getSceneMgr());
    overlay.setSelectedBone(1);
    overlay.setVisible(true);
    overlay.setShowVertices(true);

    auto& matMgr = Ogre::MaterialManager::getSingleton();
    auto dotMat = matMgr.getByName("BoneWeightOverlay/VertexMaterial");
    ASSERT_TRUE(dotMat);
    auto* dotPass = dotMat->getTechnique(0)->getPass(0);
    EXPECT_TRUE(dotPass->getDepthCheckEnabled())
        << "dots must be occluded by the mesh; that occlusion IS the depth cue";
    EXPECT_FALSE(dotPass->getDepthWriteEnabled())
        << "dots must not write depth, or they occlude the skeleton";

    auto heatMat = matMgr.getByName("BoneWeightOverlay/Material");
    ASSERT_TRUE(heatMat);
    EXPECT_FALSE(heatMat->getTechnique(0)->getPass(0)->getDepthCheckEnabled())
        << "the heat map deliberately shows through the surface";
}

// A brand-new overlay must ADOPT the current weight-paint state.
//
// This used to cover "paint enabled BEFORE any overlay exists", but paint mode
// now creates the overlay itself (SkinWeightController::setWeightPaintEnabled
// -> toggleBoneWeights), so that ordering is no longer reachable. What still
// matters, and what this now checks, is that an overlay REBUILT while paint
// mode is live comes back with its dots on — the case that occurs when the user
// hides and re-shows the heat map mid-session.
TEST_F(BoneWeightOverlayInMemoryTest, NewOverlayAdoptsActiveWeightPaintState)
{
    Ogre::Entity* entity = createAnimatedTestEntity("BWO_AdoptPaint");
    ASSERT_NE(entity, nullptr);

    AnimationWidget widget(nullptr);
    SelectionSet::getSingleton()->selectOne(entity);
    SkinWeightController::instance()->setWeightPaintEnabled(true);

    // Paint mode brought the overlay up by itself.
    auto* first = widget.getBoneWeightOverlay(entity);
    ASSERT_NE(first, nullptr) << "paint mode must create the overlay";

    // Destroy just the overlay, leaving paint mode enabled, then rebuild it the
    // way a re-show does. Calling toggleBoneWeights(false) would also exit paint
    // mode by design, so drive the rebuild directly.
    ASSERT_TRUE(widget.toggleBoneWeights(entity, true))
        << "re-showing an already-shown overlay must be a no-op, not a rebuild";

    auto* overlay = widget.getBoneWeightOverlay(entity);
    ASSERT_NE(overlay, nullptr);
    EXPECT_TRUE(overlay->showVertices())
        << "the overlay must read paint mode at construction";

    SkinWeightController::instance()->setWeightPaintEnabled(false);
}
