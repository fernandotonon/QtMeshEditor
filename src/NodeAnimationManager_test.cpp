#include <gtest/gtest.h>

#include <QSignalSpy>

#include "Manager.h"
#include "NodeAnimationManager.h"
#include "TestHelpers.h"

#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

namespace {

// Build (or fetch) a named SceneNode under the scene root so the
// add-keyframe path has a real Ogre::Node to bind to. Named so the
// manager can look it up via Manager::getSceneMgr()->getSceneNode.
Ogre::SceneNode* makeNamedNode(const std::string& name)
{
    auto* scene = Manager::getSingleton()->getSceneMgr();
    if (scene->hasSceneNode(name))
        return scene->getSceneNode(name);
    return scene->getRootSceneNode()->createChildSceneNode(name);
}

}  // namespace

// =============================================================================
// Standalone (no Ogre)
// =============================================================================

TEST(NodeAnimationManagerStandalone, InstanceIsSingleton) {
    auto* a = NodeAnimationManager::instance();
    auto* b = NodeAnimationManager::instance();
    EXPECT_EQ(a, b);
    EXPECT_NE(a, nullptr);
}

TEST(NodeAnimationManagerStandalone, EmptyNamesRejected) {
    auto* m = NodeAnimationManager::instance();
    EXPECT_FALSE(m->createClip(QString(), 1.0));
    EXPECT_FALSE(m->createClip(QStringLiteral("OK"), 0.0));   // zero length
    EXPECT_FALSE(m->createClip(QStringLiteral("OK"), -1.0));  // negative length
    EXPECT_FALSE(m->deleteClip(QString()));
    EXPECT_FALSE(m->setClipEnabled(QString(), true));
}

// =============================================================================
// Scene fixture — tests need a real Ogre SceneManager.
// =============================================================================

class NodeAnimationManagerSceneTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        // Reset whatever state a prior test left behind. We don't
        // tear down the SceneManager (Manager::kill() before each
        // test would invalidate Ogre::Root for the rest of the
        // suite), but we do drop our clips so name collisions don't
        // bleed between cases.
        if (auto* scene = Manager::getSingleton()->getSceneMgr()) {
            const std::vector<std::string> drops = {
                "NA_LifeCycle", "NA_ListOrder1", "NA_ListOrder2",
                "NA_KeyAdd", "NA_KeyIdempotent", "NA_DeleteUnknown",
                "NA_OutOfRange", "NA_Enable", "NA_Signals",
                "NA_Collision"
            };
            for (const auto& n : drops) {
                if (scene->hasAnimationState(n))
                    scene->destroyAnimationState(n);
                if (scene->hasAnimation(n))
                    scene->removeAnimation(n);
            }
        }
    }
};

TEST_F(NodeAnimationManagerSceneTest, CreateAndDeleteClip) {
    auto* m = NodeAnimationManager::instance();
    EXPECT_TRUE(m->createClip(QStringLiteral("NA_LifeCycle"), 2.0));
    auto* scene = Manager::getSingleton()->getSceneMgr();
    EXPECT_TRUE(scene->hasAnimation("NA_LifeCycle"));
    EXPECT_TRUE(scene->hasAnimationState("NA_LifeCycle"));

    // Duplicate-name create is rejected.
    EXPECT_FALSE(m->createClip(QStringLiteral("NA_LifeCycle"), 1.0));

    EXPECT_TRUE(m->deleteClip(QStringLiteral("NA_LifeCycle")));
    EXPECT_FALSE(scene->hasAnimation("NA_LifeCycle"));
    EXPECT_FALSE(scene->hasAnimationState("NA_LifeCycle"));
}

TEST_F(NodeAnimationManagerSceneTest, ListClipsReturnsInScenecreationOrder) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_ListOrder1"), 1.0));
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_ListOrder2"), 1.0));
    const QStringList clips = m->listClips();
    EXPECT_TRUE(clips.contains(QStringLiteral("NA_ListOrder1")));
    EXPECT_TRUE(clips.contains(QStringLiteral("NA_ListOrder2")));
}

TEST_F(NodeAnimationManagerSceneTest, AddKeyframeStoresTransformAndAdvancesTrack) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_KeyAdd"), 2.0));
    auto* node = makeNamedNode("NA_KeyAdd_Node");
    ASSERT_NE(node, nullptr);

    EXPECT_TRUE(m->addKeyframe(QStringLiteral("NA_KeyAdd"),
                                QStringLiteral("NA_KeyAdd_Node"),
                                0.5,
                                Ogre::Vector3(1, 2, 3),
                                Ogre::Quaternion(1, 0, 0, 0),
                                Ogre::Vector3(1, 1, 1)));

    QList<double> keys = m->keyTimesForNode(QStringLiteral("NA_KeyAdd"),
                                             QStringLiteral("NA_KeyAdd_Node"));
    ASSERT_EQ(keys.size(), 1);
    EXPECT_NEAR(keys[0], 0.5, 1e-4);

    // Adding a second, distinct keyframe creates a new entry — not
    // a duplicate. Verifies the merge-epsilon doesn't over-collapse.
    EXPECT_TRUE(m->addKeyframe(QStringLiteral("NA_KeyAdd"),
                                QStringLiteral("NA_KeyAdd_Node"),
                                1.5,
                                Ogre::Vector3(4, 5, 6),
                                Ogre::Quaternion(1, 0, 0, 0),
                                Ogre::Vector3(2, 2, 2)));
    keys = m->keyTimesForNode(QStringLiteral("NA_KeyAdd"),
                              QStringLiteral("NA_KeyAdd_Node"));
    EXPECT_EQ(keys.size(), 2);
}

TEST_F(NodeAnimationManagerSceneTest, AddKeyframeIdempotentWithinEpsilon) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_KeyIdempotent"), 2.0));
    auto* node = makeNamedNode("NA_KeyIdempotent_Node");
    ASSERT_NE(node, nullptr);

    // Two keyframes < 1ms apart → second one updates the first in place.
    EXPECT_TRUE(m->addKeyframe(QStringLiteral("NA_KeyIdempotent"),
                                QStringLiteral("NA_KeyIdempotent_Node"),
                                0.500,
                                Ogre::Vector3(1, 0, 0),
                                Ogre::Quaternion(1, 0, 0, 0),
                                Ogre::Vector3(1, 1, 1)));
    EXPECT_TRUE(m->addKeyframe(QStringLiteral("NA_KeyIdempotent"),
                                QStringLiteral("NA_KeyIdempotent_Node"),
                                0.5005,
                                Ogre::Vector3(9, 9, 9),
                                Ogre::Quaternion(1, 0, 0, 0),
                                Ogre::Vector3(2, 2, 2)));
    QList<double> keys = m->keyTimesForNode(QStringLiteral("NA_KeyIdempotent"),
                                             QStringLiteral("NA_KeyIdempotent_Node"));
    EXPECT_EQ(keys.size(), 1);  // collapsed
}

TEST_F(NodeAnimationManagerSceneTest, KeyframeRejectedForUnknownNodeOrClip) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_DeleteUnknown"), 1.0));
    EXPECT_FALSE(m->addKeyframe(QStringLiteral("NA_DeleteUnknown"),
                                 QStringLiteral("DoesNotExist"),
                                 0.5,
                                 Ogre::Vector3::ZERO,
                                 Ogre::Quaternion::IDENTITY,
                                 Ogre::Vector3(1,1,1)));
    EXPECT_FALSE(m->addKeyframe(QStringLiteral("ClipDoesNotExist"),
                                 QStringLiteral("Anything"),
                                 0.5,
                                 Ogre::Vector3::ZERO,
                                 Ogre::Quaternion::IDENTITY,
                                 Ogre::Vector3(1,1,1)));
}

TEST_F(NodeAnimationManagerSceneTest, KeyframeOutOfRangeRejected) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_OutOfRange"), 1.0));
    makeNamedNode("NA_OutOfRange_Node");

    // Negative time
    EXPECT_FALSE(m->addKeyframe(QStringLiteral("NA_OutOfRange"),
                                 QStringLiteral("NA_OutOfRange_Node"),
                                 -0.1,
                                 Ogre::Vector3::ZERO,
                                 Ogre::Quaternion::IDENTITY,
                                 Ogre::Vector3(1,1,1)));
    // Past clip length
    EXPECT_FALSE(m->addKeyframe(QStringLiteral("NA_OutOfRange"),
                                 QStringLiteral("NA_OutOfRange_Node"),
                                 2.0,
                                 Ogre::Vector3::ZERO,
                                 Ogre::Quaternion::IDENTITY,
                                 Ogre::Vector3(1,1,1)));
    // At length boundary (inclusive) is fine
    EXPECT_TRUE(m->addKeyframe(QStringLiteral("NA_OutOfRange"),
                                QStringLiteral("NA_OutOfRange_Node"),
                                1.0,
                                Ogre::Vector3::ZERO,
                                Ogre::Quaternion::IDENTITY,
                                Ogre::Vector3(1,1,1)));
}

TEST_F(NodeAnimationManagerSceneTest, SetClipEnabledTogglesState) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_Enable"), 1.0));

    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* state = scene->getAnimationState("NA_Enable");
    ASSERT_NE(state, nullptr);

    EXPECT_FALSE(state->getEnabled());  // newly created is disabled by default
    EXPECT_TRUE(m->setClipEnabled(QStringLiteral("NA_Enable"), true));
    EXPECT_TRUE(state->getEnabled());
    EXPECT_TRUE(m->setClipEnabled(QStringLiteral("NA_Enable"), false));
    EXPECT_FALSE(state->getEnabled());

    EXPECT_FALSE(m->setClipEnabled(QStringLiteral("UnknownClip"), true));
}

TEST_F(NodeAnimationManagerSceneTest, ClipsChangedSignalFiresOnCreateAndDelete) {
    auto* m = NodeAnimationManager::instance();
    QSignalSpy spy(m, &NodeAnimationManager::clipsChanged);

    ASSERT_TRUE(m->createClip(QStringLiteral("NA_Signals"), 1.0));
    EXPECT_GE(spy.count(), 1);

    const int afterCreate = spy.count();
    ASSERT_TRUE(m->deleteClip(QStringLiteral("NA_Signals")));
    EXPECT_GT(spy.count(), afterCreate);
}

TEST_F(NodeAnimationManagerSceneTest, KeyTimesForMissingClipIsEmpty) {
    auto* m = NodeAnimationManager::instance();
    EXPECT_TRUE(m->keyTimesForNode(QStringLiteral("Nope"),
                                    QStringLiteral("Whatever")).isEmpty());
}

// Two nodes in the same clip must land on independent tracks even
// when their names would have hash-collided under the original
// `qHash & 0xFFFF` strategy (Codex P1 on PR #584). This test
// verifies via the more general property: distinct node names →
// keyTimesForNode returns disjoint key sets, never a shared one.
TEST_F(NodeAnimationManagerSceneTest, DistinctNodesGetDistinctTracks) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_Collision"), 2.0));
    auto* nodeA = makeNamedNode("NA_Collision_NodeA");
    auto* nodeB = makeNamedNode("NA_Collision_NodeB");
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);

    EXPECT_TRUE(m->addKeyframe(QStringLiteral("NA_Collision"),
                                QStringLiteral("NA_Collision_NodeA"),
                                0.25, Ogre::Vector3(1,0,0),
                                Ogre::Quaternion::IDENTITY, Ogre::Vector3(1,1,1)));
    EXPECT_TRUE(m->addKeyframe(QStringLiteral("NA_Collision"),
                                QStringLiteral("NA_Collision_NodeB"),
                                0.75, Ogre::Vector3(0,1,0),
                                Ogre::Quaternion::IDENTITY, Ogre::Vector3(1,1,1)));

    auto keysA = m->keyTimesForNode(QStringLiteral("NA_Collision"),
                                     QStringLiteral("NA_Collision_NodeA"));
    auto keysB = m->keyTimesForNode(QStringLiteral("NA_Collision"),
                                     QStringLiteral("NA_Collision_NodeB"));
    // Each node sees ONLY its own keyframe. The pre-fix hash-truncation
    // strategy would have made keysA == keysB == [0.25, 0.75] on
    // collision, or worse, only [0.75] if the second add silently
    // overwrote the first track's keyframe.
    ASSERT_EQ(keysA.size(), 1);
    ASSERT_EQ(keysB.size(), 1);
    EXPECT_NEAR(keysA[0], 0.25, 1e-4);
    EXPECT_NEAR(keysB[0], 0.75, 1e-4);
}
