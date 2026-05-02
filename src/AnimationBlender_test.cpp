#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>

#include "AnimationBlender.h"
#include "AnimationControlController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreKeyFrame.h>

// Pure-data fixture — exercises property setters, signals, and clamping.
// Doesn't need Ogre to load, so runs on every platform (including macOS
// where Ogre plugins fail to load for the test binary).
class AnimationBlenderPropertyTest : public ::testing::Test {
protected:
    void SetUp() override {
        AnimationBlender::kill();
        AnimationControlController::kill();
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }
    void TearDown() override {
        AnimationBlender::kill();
        AnimationControlController::kill();
    }
    QApplication* app = nullptr;
};

TEST_F(AnimationBlenderPropertyTest, DefaultsAreSane) {
    auto* b = AnimationBlender::instance();
    EXPECT_FALSE(b->active());
    EXPECT_DOUBLE_EQ(b->weight(), 0.5);
    EXPECT_EQ(b->mode(), static_cast<int>(AnimationBlender::ModeMix));
    EXPECT_TRUE(b->animA().isEmpty());
    EXPECT_TRUE(b->animB().isEmpty());
}

TEST_F(AnimationBlenderPropertyTest, WeightClamps) {
    auto* b = AnimationBlender::instance();
    b->setWeight(-1.0);
    EXPECT_DOUBLE_EQ(b->weight(), 0.0);
    b->setWeight(2.5);
    EXPECT_DOUBLE_EQ(b->weight(), 1.0);
    b->setWeight(0.42);
    EXPECT_DOUBLE_EQ(b->weight(), 0.42);
}

TEST_F(AnimationBlenderPropertyTest, ModeClampsToKnownValues) {
    auto* b = AnimationBlender::instance();
    b->setMode(99); // invalid → defaults to Mix
    EXPECT_EQ(b->mode(), static_cast<int>(AnimationBlender::ModeMix));
    b->setMode(static_cast<int>(AnimationBlender::ModeAdditive));
    EXPECT_EQ(b->mode(), static_cast<int>(AnimationBlender::ModeAdditive));
    b->setMode(static_cast<int>(AnimationBlender::ModeOverride));
    EXPECT_EQ(b->mode(), static_cast<int>(AnimationBlender::ModeOverride));
}

TEST_F(AnimationBlenderPropertyTest, ActiveTogglesEmitSignal) {
    auto* b = AnimationBlender::instance();
    // Precondition: A/B must be set + distinct for activation to be accepted.
    b->setAnimA("walk");
    b->setAnimB("run");
    QSignalSpy spy(b, &AnimationBlender::activeChanged);
    b->setActive(true);
    EXPECT_EQ(spy.count(), 1);
    b->setActive(true); // unchanged → no re-emit
    EXPECT_EQ(spy.count(), 1);
    b->setActive(false);
    EXPECT_EQ(spy.count(), 2);
}

TEST_F(AnimationBlenderPropertyTest, WeightChangeEmitsSignal) {
    auto* b = AnimationBlender::instance();
    QSignalSpy spy(b, &AnimationBlender::weightChanged);
    b->setWeight(0.25);
    EXPECT_EQ(spy.count(), 1);
    b->setWeight(0.25); // same → no re-emit
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(AnimationBlenderPropertyTest, AnimSelectionEmitsSignal) {
    auto* b = AnimationBlender::instance();
    QSignalSpy spy(b, &AnimationBlender::selectionChanged);
    b->setAnimA("walk");
    EXPECT_EQ(spy.count(), 1);
    b->setAnimB("run");
    EXPECT_EQ(spy.count(), 2);
    b->setAnimA("walk"); // unchanged → no re-emit
    EXPECT_EQ(spy.count(), 2);
}

TEST_F(AnimationBlenderPropertyTest, ApplyOnNullEntityIsNoOp) {
    auto* b = AnimationBlender::instance();
    b->setActive(true);
    EXPECT_NO_THROW(b->apply(nullptr, 0.016));
    EXPECT_FALSE(b->apply(nullptr, 0.016));
}

TEST_F(AnimationBlenderPropertyTest, BakeWithEmptyClipNameReturnsEmpty) {
    auto* b = AnimationBlender::instance();
    EXPECT_TRUE(b->bake("", 30).isEmpty());
}

TEST_F(AnimationBlenderPropertyTest, BakeWithNoSelectionReturnsEmpty) {
    auto* b = AnimationBlender::instance();
    EXPECT_TRUE(b->bake("MyClip", 30).isEmpty());
}

TEST_F(AnimationBlenderPropertyTest, BakeRefusesEmptyAnimAB) {
    auto* b = AnimationBlender::instance();
    // animA/animB unset → bake bails out before ever touching the entity.
    EXPECT_TRUE(b->bake("X", 30).isEmpty());
    b->setAnimA("walk");
    EXPECT_TRUE(b->bake("X", 30).isEmpty());
    b->setAnimA("");
    b->setAnimB("run");
    EXPECT_TRUE(b->bake("X", 30).isEmpty());
}

TEST_F(AnimationBlenderPropertyTest, ActiveEntityNameDefaultsEmpty) {
    auto* b = AnimationBlender::instance();
    EXPECT_TRUE(b->activeEntityName().isEmpty());
}

TEST_F(AnimationBlenderPropertyTest, BakeOverSourceClipReturnsEmpty) {
    auto* b = AnimationBlender::instance();
    b->setAnimA("walk");
    b->setAnimB("run");
    // Even with no entity resolved, the source-name guard should fire first.
    EXPECT_TRUE(b->bake("walk", 30).isEmpty());
    EXPECT_TRUE(b->bake("run", 30).isEmpty());
}

TEST_F(AnimationBlenderPropertyTest, ActivateRefusedWhenAnimAEmpty) {
    auto* b = AnimationBlender::instance();
    b->setAnimB("run"); // A still empty
    b->setActive(true);
    EXPECT_FALSE(b->active());
}

TEST_F(AnimationBlenderPropertyTest, ActivateRefusedWhenAnimBEmpty) {
    auto* b = AnimationBlender::instance();
    b->setAnimA("walk"); // B still empty
    b->setActive(true);
    EXPECT_FALSE(b->active());
}

TEST_F(AnimationBlenderPropertyTest, ActivateRefusedWhenAEqualsB) {
    auto* b = AnimationBlender::instance();
    b->setAnimA("walk");
    b->setAnimB("walk"); // same clip both sides
    b->setActive(true);
    EXPECT_FALSE(b->active());
}

TEST_F(AnimationBlenderPropertyTest, BakeRefusedWhenAEqualsB) {
    auto* b = AnimationBlender::instance();
    b->setAnimA("walk");
    b->setAnimB("walk");
    EXPECT_TRUE(b->bake("X", 30).isEmpty());
}

TEST_F(AnimationBlenderPropertyTest, ClearingAnimAWhileActiveDeactivates) {
    auto* b = AnimationBlender::instance();
    b->setAnimA("walk");
    b->setAnimB("run");
    b->setActive(true);
    ASSERT_TRUE(b->active());
    // Clearing animA invalidates the blend → blender must self-deactivate.
    b->setAnimA("");
    EXPECT_FALSE(b->active());
}

TEST_F(AnimationBlenderPropertyTest, MakingAEqualBWhileActiveDeactivates) {
    auto* b = AnimationBlender::instance();
    b->setAnimA("walk");
    b->setAnimB("run");
    b->setActive(true);
    ASSERT_TRUE(b->active());
    // Setting B to the same clip as A invalidates the blend.
    b->setAnimB("walk");
    EXPECT_FALSE(b->active());
}

// ── Live blend + bake against a real animated entity ──────────────────────────
//
// Uses the standard Ogre test fixture pattern. createAnimatedTestEntity() only
// builds one animation ("TestAnim"); the helper below adds a second one so we
// can exercise the actual blend math.

class AnimationBlenderTest : public ::testing::Test {
protected:
    void SetUp() override {
        AnimationBlender::kill();
        AnimationControlController::kill();
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }
    void TearDown() override {
        SelectionSet::getSingleton()->clear();
        if (app) app->processEvents();
        AnimationBlender::kill();
        AnimationControlController::kill();
    }

    // Adds a second animation "TestAnimB" with its own keyframes on the
    // child bone of the entity returned by createAnimatedTestEntity().
    static void addSecondAnim(Ogre::Entity* entity) {
        auto* skel = entity->getSkeleton();
        // skeleton-instance creation goes through the master skeleton; we add
        // the animation on the master and then re-init the instance via
        // refreshAvailableAnimationState below.
        Ogre::Skeleton* master = skel; // SkeletonInstance shares animations
        auto* anim = master->createAnimation("TestAnimB", 1.0f);
        auto* track = anim->createNodeTrack(1);
        track->setAssociatedNode(skel->getBone(1));

        // KFs designed to differ from TestAnim so weight=0 vs weight=1
        // produce visibly different poses.
        auto* kf0 = track->createNodeKeyFrame(0.0f);
        kf0->setTranslate(Ogre::Vector3(2.0f, 0, 0));
        kf0->setRotation(Ogre::Quaternion::IDENTITY);
        kf0->setScale(Ogre::Vector3::UNIT_SCALE);
        auto* kf1 = track->createNodeKeyFrame(1.0f);
        kf1->setTranslate(Ogre::Vector3(2.0f, 0, 0));
        kf1->setRotation(Ogre::Quaternion::IDENTITY);
        kf1->setScale(Ogre::Vector3::UNIT_SCALE);

        entity->refreshAvailableAnimationState();
    }

    Ogre::Entity* setupBlendEntity(const std::string& name) {
        if (!canLoadMeshFiles()) return nullptr;
        Ogre::Entity* entity = createAnimatedTestEntity(name);
        if (!entity) return nullptr;
        addSecondAnim(entity);
        SelectionSet::getSingleton()->selectOne(entity->getParentSceneNode());
        if (app) app->processEvents();
        // Drive the controller so the blender's refreshFromSelection() picks
        // up the active entity.
        auto* ctrl = AnimationControlController::instance();
        ctrl->updateAnimationTree();
        ctrl->selectAnimation(QString::fromStdString(entity->getName()), "TestAnim");
        return entity;
    }

    QApplication* app = nullptr;
};

TEST_F(AnimationBlenderTest, RefreshExposesEntityAnimations) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupBlendEntity("ABT_RefreshTest");
    ASSERT_NE(entity, nullptr);

    auto* b = AnimationBlender::instance();
    b->refreshFromSelection();
    auto names = b->animations();
    EXPECT_TRUE(names.contains("TestAnim"));
    EXPECT_TRUE(names.contains("TestAnimB"));
}

TEST_F(AnimationBlenderTest, BakeProducesNewClipWithExpectedLength) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupBlendEntity("ABT_BakeTest");
    ASSERT_NE(entity, nullptr);

    auto* b = AnimationBlender::instance();
    b->refreshFromSelection();
    b->setAnimA("TestAnim");
    b->setAnimB("TestAnimB");
    b->setWeight(0.5);
    b->setMode(AnimationBlender::ModeMix);

    QString clip = b->bake("BlendedClip", 30);
    EXPECT_EQ(clip, "BlendedClip");

    auto* skel = entity->getSkeleton();
    ASSERT_TRUE(skel->hasAnimation("BlendedClip"));
    auto* baked = skel->getAnimation("BlendedClip");
    EXPECT_NEAR(baked->getLength(), 1.0f, 1e-3);
    // 30 fps over 1.0s → 31 samples (including endpoints)
    auto* track = baked->getNodeTrack(1);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->getNumKeyFrames(), 31u);
}

TEST_F(AnimationBlenderTest, BakeAtZeroWeightMatchesAnimA) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupBlendEntity("ABT_W0Test");
    ASSERT_NE(entity, nullptr);

    auto* b = AnimationBlender::instance();
    b->refreshFromSelection();
    b->setAnimA("TestAnim");
    b->setAnimB("TestAnimB");
    b->setWeight(0.0);

    ASSERT_FALSE(b->bake("BlendW0", 30).isEmpty());

    auto* skel = entity->getSkeleton();
    auto* baked = skel->getAnimation("BlendW0");
    auto* track = baked->getNodeTrack(1);
    ASSERT_NE(track, nullptr);

    // TestAnim at t=0.5 sets translate to (0.5, 0, 0). Find the closest
    // baked keyframe to t=0.5 and confirm.
    Ogre::TransformKeyFrame* closest = nullptr;
    float bestDist = 1.0f;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        const float d = std::fabs(kf->getTime() - 0.5f);
        if (d < bestDist) { bestDist = d; closest = kf; }
    }
    ASSERT_NE(closest, nullptr);
    // weight=0 ⇒ purely TestAnim (≈0.5 on x at t=0.5)
    EXPECT_NEAR(closest->getTranslate().x, 0.5f, 0.05f);
}

TEST_F(AnimationBlenderTest, BakeAtFullWeightMatchesAnimB) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupBlendEntity("ABT_W1Test");
    ASSERT_NE(entity, nullptr);

    auto* b = AnimationBlender::instance();
    b->refreshFromSelection();
    b->setAnimA("TestAnim");
    b->setAnimB("TestAnimB");
    b->setWeight(1.0);

    ASSERT_FALSE(b->bake("BlendW1", 30).isEmpty());
    auto* baked = entity->getSkeleton()->getAnimation("BlendW1");
    auto* track = baked->getNodeTrack(1);
    ASSERT_NE(track, nullptr);

    // TestAnimB sets translate to (2, 0, 0) at every t — at weight=1 the
    // bake should land near 2.0 on x for every sample.
    auto* mid = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(track->getNumKeyFrames() / 2));
    EXPECT_NEAR(mid->getTranslate().x, 2.0f, 0.05f);
}

TEST_F(AnimationBlenderTest, BakeOverwritesExistingClipWithSameName) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupBlendEntity("ABT_OverwriteTest");
    ASSERT_NE(entity, nullptr);

    auto* b = AnimationBlender::instance();
    b->refreshFromSelection();
    b->setAnimA("TestAnim");
    b->setAnimB("TestAnimB");
    b->setWeight(0.5);

    ASSERT_FALSE(b->bake("Reusable", 30).isEmpty());
    ASSERT_FALSE(b->bake("Reusable", 60).isEmpty());

    auto* skel = entity->getSkeleton();
    EXPECT_TRUE(skel->hasAnimation("Reusable"));
    // 60 fps version should have ~61 keyframes
    auto* track = skel->getAnimation("Reusable")->getNodeTrack(1);
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->getNumKeyFrames(), 61u);
}

// ── New behavior tests (snapshot/restore + bake side effects) ─────────────────

TEST_F(AnimationBlenderTest, BakeRefusesToOverwriteSourceClip) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupBlendEntity("ABT_OverwriteSourceTest");
    ASSERT_NE(entity, nullptr);

    auto* b = AnimationBlender::instance();
    b->refreshFromSelection();
    b->setAnimA("TestAnim");
    b->setAnimB("TestAnimB");
    // Both source clip names should be rejected.
    EXPECT_TRUE(b->bake("TestAnim", 30).isEmpty());
    EXPECT_TRUE(b->bake("TestAnimB", 30).isEmpty());
    // Source clips remain intact.
    EXPECT_TRUE(entity->getSkeleton()->hasAnimation("TestAnim"));
    EXPECT_TRUE(entity->getSkeleton()->hasAnimation("TestAnimB"));
}

TEST_F(AnimationBlenderTest, ActivateDisablesEveryAnimationState) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupBlendEntity("ABT_ActivateDisablesTest");
    ASSERT_NE(entity, nullptr);

    // Pre-condition: both animations enabled before activation.
    auto* setBefore = entity->getAllAnimationStates();
    setBefore->getAnimationState("TestAnim")->setEnabled(true);
    setBefore->getAnimationState("TestAnimB")->setEnabled(true);

    auto* b = AnimationBlender::instance();
    b->refreshFromSelection();
    b->setAnimA("TestAnim");
    b->setAnimB("TestAnimB");
    b->setActive(true);

    // After activation, every per-anim state should be disabled. apply()
    // re-enables A and B on the next render tick — but in this test there's
    // no tick, so we only check the immediate post-activation state.
    auto* set = entity->getAllAnimationStates();
    for (const auto& [name, state] : set->getAnimationStates()) {
        EXPECT_FALSE(state->getEnabled())
            << "expected " << name << " disabled after blender activate";
    }
}

TEST_F(AnimationBlenderTest, DeactivateRestoresPreActivateEnabledFlags) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupBlendEntity("ABT_DeactivateRestoresTest");
    ASSERT_NE(entity, nullptr);

    // Set a known pre-blend configuration: only TestAnim enabled.
    auto* set = entity->getAllAnimationStates();
    set->getAnimationState("TestAnim")->setEnabled(true);
    set->getAnimationState("TestAnimB")->setEnabled(false);

    auto* b = AnimationBlender::instance();
    b->refreshFromSelection();
    b->setAnimA("TestAnim");
    b->setAnimB("TestAnimB");
    b->setActive(true);
    // Verify activation changed things.
    EXPECT_FALSE(set->getAnimationState("TestAnim")->getEnabled());

    b->setActive(false);
    // After deactivation, the snapshot should restore the original flags.
    EXPECT_TRUE(set->getAnimationState("TestAnim")->getEnabled());
    EXPECT_FALSE(set->getAnimationState("TestAnimB")->getEnabled());
}

TEST_F(AnimationBlenderTest, BakeDeactivatesBlender) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupBlendEntity("ABT_BakeDeactivatesTest");
    ASSERT_NE(entity, nullptr);

    auto* b = AnimationBlender::instance();
    b->refreshFromSelection();
    b->setAnimA("TestAnim");
    b->setAnimB("TestAnimB");
    b->setActive(true);
    EXPECT_TRUE(b->active());

    ASSERT_FALSE(b->bake("BakedClip", 30).isEmpty());

    // After bake, the blender should have deactivated itself so the live
    // preview returns to the pre-blend state.
    EXPECT_FALSE(b->active());
}

TEST_F(AnimationBlenderTest, ClipBakedSignalEmittedOnSuccess) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupBlendEntity("ABT_ClipBakedSignalTest");
    ASSERT_NE(entity, nullptr);

    auto* b = AnimationBlender::instance();
    b->refreshFromSelection();
    b->setAnimA("TestAnim");
    b->setAnimB("TestAnimB");

    QSignalSpy spy(b, &AnimationBlender::clipBaked);
    ASSERT_FALSE(b->bake("SignalClip", 30).isEmpty());
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.takeFirst().at(0).toString(), "SignalClip");
}

TEST_F(AnimationBlenderTest, ActiveEntityNameTracksControllerSelection) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = setupBlendEntity("ABT_ActiveEntityNameTest");
    ASSERT_NE(entity, nullptr);

    auto* b = AnimationBlender::instance();
    b->refreshFromSelection();
    EXPECT_EQ(b->activeEntityName().toStdString(), entity->getName());
}
