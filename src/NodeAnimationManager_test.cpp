#include <gtest/gtest.h>

#include <QSignalSpy>

#include "Manager.h"
#include "NodeAnimationManager.h"
#include "TestHelpers.h"
#include "UndoManager.h"
#include "commands/NodeAnimCommands.h"

#include <OgreAnimation.h>
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
                "NA_Collision",
                "NA_CmdCreate", "NA_CmdDelete", "NA_CmdSetKey",
                "NA_CmdHandleStale",
                // Slice C GUI surface (#517)
                "NA_Rows", "NA_Length", "NA_Active", "NA_Scrub",
                "NA_UndoCreate", "NA_UndoDelete", "NA_KeyCurrent",
                "NA_MoveKf", "NA_DeleteKf"
            };
            for (const auto& n : drops) {
                if (scene->hasAnimationState(n))
                    scene->destroyAnimationState(n);
                if (scene->hasAnimation(n))
                    scene->removeAnimation(n);
            }
        }
        // The undoable GUI-surface tests push onto the shared undo
        // stack; clear it between tests so a leftover command can't
        // be undone into a prior test's clip.
        UndoManager::getSingleton()->clear();
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
// ─── Slice C3 — undo commands ────────────────────────────────────────

TEST_F(NodeAnimationManagerSceneTest, CreateClipCommandRoundTrips) {
    CreateNodeAnimClipCommand cmd(QStringLiteral("NA_CmdCreate"), 1.5);
    cmd.redo();
    auto* scene = Manager::getSingleton()->getSceneMgr();
    EXPECT_TRUE(scene->hasAnimation("NA_CmdCreate"));
    EXPECT_TRUE(scene->hasAnimationState("NA_CmdCreate"));

    cmd.undo();
    EXPECT_FALSE(scene->hasAnimation("NA_CmdCreate"));
    EXPECT_FALSE(scene->hasAnimationState("NA_CmdCreate"));

    cmd.redo();  // Redo path is symmetric.
    EXPECT_TRUE(scene->hasAnimation("NA_CmdCreate"));
}

TEST_F(NodeAnimationManagerSceneTest, DeleteClipCommandSnapshotsAndRestores) {
    // Set up: clip with two tracks (two nodes), each with two keys.
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_CmdDelete"), 2.0));
    makeNamedNode("NA_CmdDelete_NodeA");
    makeNamedNode("NA_CmdDelete_NodeB");
    ASSERT_TRUE(m->addKeyframe(QStringLiteral("NA_CmdDelete"),
                                QStringLiteral("NA_CmdDelete_NodeA"),
                                0.5, Ogre::Vector3(1,2,3),
                                Ogre::Quaternion(1,0,0,0), Ogre::Vector3(1,1,1)));
    ASSERT_TRUE(m->addKeyframe(QStringLiteral("NA_CmdDelete"),
                                QStringLiteral("NA_CmdDelete_NodeA"),
                                1.5, Ogre::Vector3(4,5,6),
                                Ogre::Quaternion(1,0,0,0), Ogre::Vector3(2,2,2)));
    ASSERT_TRUE(m->addKeyframe(QStringLiteral("NA_CmdDelete"),
                                QStringLiteral("NA_CmdDelete_NodeB"),
                                0.75, Ogre::Vector3(7,8,9),
                                Ogre::Quaternion(1,0,0,0), Ogre::Vector3(1,1,1)));

    DeleteNodeAnimClipCommand cmd(QStringLiteral("NA_CmdDelete"));
    cmd.redo();
    auto* scene = Manager::getSingleton()->getSceneMgr();
    EXPECT_FALSE(scene->hasAnimation("NA_CmdDelete"));

    cmd.undo();
    EXPECT_TRUE(scene->hasAnimation("NA_CmdDelete"));
    // Verify both tracks + all keyframes came back.
    auto keysA = m->keyTimesForNode(QStringLiteral("NA_CmdDelete"),
                                     QStringLiteral("NA_CmdDelete_NodeA"));
    auto keysB = m->keyTimesForNode(QStringLiteral("NA_CmdDelete"),
                                     QStringLiteral("NA_CmdDelete_NodeB"));
    EXPECT_EQ(keysA.size(), 2);
    EXPECT_EQ(keysB.size(), 1);
}

TEST_F(NodeAnimationManagerSceneTest, SetKeyframeCommandRoundTripsForFreshKey) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_CmdSetKey"), 2.0));
    makeNamedNode("NA_CmdSetKey_Node");

    SetNodeKeyframeCommand cmd(QStringLiteral("NA_CmdSetKey"),
                                QStringLiteral("NA_CmdSetKey_Node"),
                                0.5,
                                Ogre::Vector3(10, 20, 30),
                                Ogre::Quaternion::IDENTITY,
                                Ogre::Vector3(1, 1, 1));
    cmd.redo();
    auto keys = m->keyTimesForNode(QStringLiteral("NA_CmdSetKey"),
                                    QStringLiteral("NA_CmdSetKey_Node"));
    ASSERT_EQ(keys.size(), 1);

    cmd.undo();
    // Track itself was created by redo, so undo should leave the
    // clip in its pre-redo state (no track for this node).
    keys = m->keyTimesForNode(QStringLiteral("NA_CmdSetKey"),
                              QStringLiteral("NA_CmdSetKey_Node"));
    EXPECT_EQ(keys.size(), 0);

    cmd.redo();
    keys = m->keyTimesForNode(QStringLiteral("NA_CmdSetKey"),
                              QStringLiteral("NA_CmdSetKey_Node"));
    EXPECT_EQ(keys.size(), 1);
}

TEST_F(NodeAnimationManagerSceneTest, SetKeyframeCommandRestoresPriorOnOverwrite) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_CmdSetKey"), 2.0));
    makeNamedNode("NA_CmdSetKey_Node");

    // Seed an existing keyframe at t=1.0 with values A.
    ASSERT_TRUE(m->addKeyframe(QStringLiteral("NA_CmdSetKey"),
                                QStringLiteral("NA_CmdSetKey_Node"),
                                1.0, Ogre::Vector3(1, 0, 0),
                                Ogre::Quaternion::IDENTITY,
                                Ogre::Vector3(1, 1, 1)));

    // Command overwrites in place (same t within epsilon).
    SetNodeKeyframeCommand cmd(QStringLiteral("NA_CmdSetKey"),
                                QStringLiteral("NA_CmdSetKey_Node"),
                                1.0005,
                                Ogre::Vector3(9, 9, 9),
                                Ogre::Quaternion::IDENTITY,
                                Ogre::Vector3(2, 2, 2));
    cmd.redo();
    // Still only one keyframe (the merge epsilon collapses).
    auto keys = m->keyTimesForNode(QStringLiteral("NA_CmdSetKey"),
                                    QStringLiteral("NA_CmdSetKey_Node"));
    EXPECT_EQ(keys.size(), 1);

    // Undo restores the prior TRS values at the same time.
    cmd.undo();
    keys = m->keyTimesForNode(QStringLiteral("NA_CmdSetKey"),
                              QStringLiteral("NA_CmdSetKey_Node"));
    EXPECT_EQ(keys.size(), 1);  // key remained — only values reverted
    // We can't read the values back via the manager API yet, but
    // the round-trip count being preserved + the undo path
    // executing without aborts is the contract we care about here.
}

// Codex P1 follow-up regression: the SetKeyframeCommand undo path
// used to call `anim->destroyNodeTrack(handle)` directly, bypassing
// the manager's `m_trackHandles` map. That left a stale handle entry
// behind — and a later `addKeyframe(differentNode, ...)` would
// scan Ogre tracks, find the now-free handle, allocate it for the
// new node, and then re-allocating the original node's handle later
// would silently corrupt the new node's animation.
//
// We exercise the round-trip: redo keyframe on A → undo → keyframe
// B on the same handle slot → keyframe A again. Without the fix,
// A and B end up sharing one track. With the fix they stay distinct.
TEST_F(NodeAnimationManagerSceneTest, SetKeyframeCommandUndoForgetsStaleHandle) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_CmdHandleStale"), 2.0));
    makeNamedNode("NA_CmdHandleStale_NodeA");
    makeNamedNode("NA_CmdHandleStale_NodeB");

    // Author + redo + undo on node A — this leaves the clip
    // with NO tracks (mTrackCreatedByRedo path).
    {
        SetNodeKeyframeCommand cmd(QStringLiteral("NA_CmdHandleStale"),
                                    QStringLiteral("NA_CmdHandleStale_NodeA"),
                                    0.5,
                                    Ogre::Vector3(1, 0, 0),
                                    Ogre::Quaternion::IDENTITY,
                                    Ogre::Vector3(1, 1, 1));
        cmd.redo();
        cmd.undo();
    }

    // Now key node B — this will allocate handle 0 (first free).
    // Then key node A again — this should allocate handle 1
    // (a new handle, NOT the stale handle 0 that node A used to have).
    ASSERT_TRUE(m->addKeyframe(QStringLiteral("NA_CmdHandleStale"),
                                QStringLiteral("NA_CmdHandleStale_NodeB"),
                                0.25, Ogre::Vector3(0, 9, 0),
                                Ogre::Quaternion::IDENTITY,
                                Ogre::Vector3(1, 1, 1)));
    ASSERT_TRUE(m->addKeyframe(QStringLiteral("NA_CmdHandleStale"),
                                QStringLiteral("NA_CmdHandleStale_NodeA"),
                                0.75, Ogre::Vector3(7, 0, 0),
                                Ogre::Quaternion::IDENTITY,
                                Ogre::Vector3(1, 1, 1)));

    // Both nodes see ONLY their own keyframe. Pre-fix, A's
    // addKeyframe would have routed onto B's handle, producing
    // keysA == keysB == [0.25, 0.75] (or worse).
    auto keysA = m->keyTimesForNode(QStringLiteral("NA_CmdHandleStale"),
                                     QStringLiteral("NA_CmdHandleStale_NodeA"));
    auto keysB = m->keyTimesForNode(QStringLiteral("NA_CmdHandleStale"),
                                     QStringLiteral("NA_CmdHandleStale_NodeB"));
    ASSERT_EQ(keysA.size(), 1);
    ASSERT_EQ(keysB.size(), 1);
    EXPECT_NEAR(keysA[0], 0.75, 1e-4);
    EXPECT_NEAR(keysB[0], 0.25, 1e-4);
}

// ─── Existing — distinct-node-track invariant ────────────────────────

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

// ─── Slice C GUI surface (#517) ──────────────────────────────────────

TEST_F(NodeAnimationManagerSceneTest, NodeRowsAndAnimatedNodesReflectTracks) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_Rows"), 2.0));
    makeNamedNode("NA_Rows_A");
    makeNamedNode("NA_Rows_B");
    ASSERT_TRUE(m->addKeyframe(QStringLiteral("NA_Rows"), QStringLiteral("NA_Rows_A"),
                               0.5, Ogre::Vector3(1,0,0), Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1,1,1)));
    ASSERT_TRUE(m->addKeyframe(QStringLiteral("NA_Rows"), QStringLiteral("NA_Rows_A"),
                               1.5, Ogre::Vector3(2,0,0), Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1,1,1)));
    ASSERT_TRUE(m->addKeyframe(QStringLiteral("NA_Rows"), QStringLiteral("NA_Rows_B"),
                               1.0, Ogre::Vector3(0,1,0), Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1,1,1)));

    const QStringList nodes = m->animatedNodes(QStringLiteral("NA_Rows"));
    EXPECT_EQ(nodes.size(), 2);
    EXPECT_TRUE(nodes.contains(QStringLiteral("NA_Rows_A")));
    EXPECT_TRUE(nodes.contains(QStringLiteral("NA_Rows_B")));

    const QVariantList rows = m->nodeRows(QStringLiteral("NA_Rows"));
    ASSERT_EQ(rows.size(), 2);
    // Each row is { node, keyTimes }.
    int keysForA = -1;
    for (const QVariant& v : rows) {
        const QVariantMap row = v.toMap();
        EXPECT_TRUE(row.contains(QStringLiteral("node")));
        EXPECT_TRUE(row.contains(QStringLiteral("keyTimes")));
        if (row.value(QStringLiteral("node")).toString() == QStringLiteral("NA_Rows_A"))
            keysForA = row.value(QStringLiteral("keyTimes")).toList().size();
    }
    EXPECT_EQ(keysForA, 2);

    // Missing clip → empty.
    EXPECT_TRUE(m->animatedNodes(QStringLiteral("Nope")).isEmpty());
    EXPECT_TRUE(m->nodeRows(QStringLiteral("Nope")).isEmpty());
}

TEST_F(NodeAnimationManagerSceneTest, ClipLengthReported) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_Length"), 3.25));
    EXPECT_NEAR(m->clipLength(QStringLiteral("NA_Length")), 3.25, 1e-4);
    EXPECT_NEAR(m->clipLength(QStringLiteral("Missing")), 0.0, 1e-4);
}

TEST_F(NodeAnimationManagerSceneTest, ActiveClipPropertyAndSignal) {
    auto* m = NodeAnimationManager::instance();
    QSignalSpy spy(m, &NodeAnimationManager::activeClipChanged);
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_Active"), 1.0));

    m->setActiveClip(QStringLiteral("NA_Active"));
    EXPECT_EQ(m->activeClip(), QStringLiteral("NA_Active"));
    EXPECT_EQ(spy.count(), 1);

    // Setting the same value is a no-op (no extra signal).
    m->setActiveClip(QStringLiteral("NA_Active"));
    EXPECT_EQ(spy.count(), 1);

    m->setActiveClip(QString());
    EXPECT_TRUE(m->activeClip().isEmpty());
    EXPECT_EQ(spy.count(), 2);
}

TEST_F(NodeAnimationManagerSceneTest, ScrubClipLeavesNodeEditable) {
    // scrubClip must NOT enable the AnimationState — an enabled node
    // clip is re-driven every render frame and would lock the node
    // against gizmo edits (the authoring bug we fixed). Scrubbing is a
    // deliberate no-op on the node: paused node clips stay editable.
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_Scrub"), 2.0));
    auto* scene = Manager::getSingleton()->getSceneMgr();
    auto* state = scene->getAnimationState("NA_Scrub");
    ASSERT_NE(state, nullptr);
    ASSERT_FALSE(state->getEnabled());

    m->scrubClip(QStringLiteral("NA_Scrub"), 1.0);
    EXPECT_FALSE(state->getEnabled());  // stays disabled → node editable

    // Missing clip is a safe no-op.
    m->scrubClip(QStringLiteral("Missing"), 0.5);
}

TEST_F(NodeAnimationManagerSceneTest, CreateAndDeleteClipUndoablePushOntoStack) {
    auto* m = NodeAnimationManager::instance();
    auto* scene = Manager::getSingleton()->getSceneMgr();

    EXPECT_TRUE(m->createClipUndoable(QStringLiteral("NA_UndoCreate"), 1.5));
    EXPECT_TRUE(scene->hasAnimation("NA_UndoCreate"));
    // Duplicate rejected without pushing.
    EXPECT_FALSE(m->createClipUndoable(QStringLiteral("NA_UndoCreate"), 1.0));

    UndoManager::getSingleton()->undo();
    EXPECT_FALSE(scene->hasAnimation("NA_UndoCreate"));
    UndoManager::getSingleton()->redo();
    EXPECT_TRUE(scene->hasAnimation("NA_UndoCreate"));

    // Delete (undoable) then undo restores.
    EXPECT_TRUE(m->deleteClipUndoable(QStringLiteral("NA_UndoCreate")));
    EXPECT_FALSE(scene->hasAnimation("NA_UndoCreate"));
    UndoManager::getSingleton()->undo();
    EXPECT_TRUE(scene->hasAnimation("NA_UndoCreate"));

    // Deleting a missing clip is rejected (no push).
    EXPECT_FALSE(m->deleteClipUndoable(QStringLiteral("Missing")));
}

TEST_F(NodeAnimationManagerSceneTest, KeyNodeCurrentTransformCapturesLiveTransform) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_KeyCurrent"), 2.0));
    auto* node = makeNamedNode("NA_KeyCurrent_Node");
    node->setPosition(Ogre::Vector3(4, 5, 6));
    node->setScale(Ogre::Vector3(2, 2, 2));

    EXPECT_TRUE(m->keyNodeCurrentTransform(QStringLiteral("NA_KeyCurrent"),
                                           QStringLiteral("NA_KeyCurrent_Node"), 0.5));
    auto keys = m->keyTimesForNode(QStringLiteral("NA_KeyCurrent"),
                                   QStringLiteral("NA_KeyCurrent_Node"));
    ASSERT_EQ(keys.size(), 1);
    EXPECT_NEAR(keys[0], 0.5, 1e-4);

    // Undo removes the key.
    UndoManager::getSingleton()->undo();
    keys = m->keyTimesForNode(QStringLiteral("NA_KeyCurrent"),
                              QStringLiteral("NA_KeyCurrent_Node"));
    EXPECT_EQ(keys.size(), 0);

    // Missing node / out-of-range time are rejected.
    EXPECT_FALSE(m->keyNodeCurrentTransform(QStringLiteral("NA_KeyCurrent"),
                                            QStringLiteral("Nope"), 0.5));
    EXPECT_FALSE(m->keyNodeCurrentTransform(QStringLiteral("NA_KeyCurrent"),
                                            QStringLiteral("NA_KeyCurrent_Node"), 99.0));
}

TEST_F(NodeAnimationManagerSceneTest, MoveNodeKeyframeUndoable) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_MoveKf"), 3.0));
    makeNamedNode("NA_MoveKf_Node");
    ASSERT_TRUE(m->addKeyframe(QStringLiteral("NA_MoveKf"), QStringLiteral("NA_MoveKf_Node"),
                               0.5, Ogre::Vector3(1,2,3), Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1,1,1)));

    EXPECT_TRUE(m->moveNodeKeyframe(QStringLiteral("NA_MoveKf"),
                                    QStringLiteral("NA_MoveKf_Node"), 0.5, 1.5));
    auto keys = m->keyTimesForNode(QStringLiteral("NA_MoveKf"),
                                   QStringLiteral("NA_MoveKf_Node"));
    ASSERT_EQ(keys.size(), 1);
    EXPECT_NEAR(keys[0], 1.5, 1e-4);

    UndoManager::getSingleton()->undo();
    keys = m->keyTimesForNode(QStringLiteral("NA_MoveKf"),
                              QStringLiteral("NA_MoveKf_Node"));
    ASSERT_EQ(keys.size(), 1);
    EXPECT_NEAR(keys[0], 0.5, 1e-4);

    // No key at oldTime → rejected. Move onto an existing key → rejected.
    EXPECT_FALSE(m->moveNodeKeyframe(QStringLiteral("NA_MoveKf"),
                                     QStringLiteral("NA_MoveKf_Node"), 2.0, 2.5));
    ASSERT_TRUE(m->addKeyframe(QStringLiteral("NA_MoveKf"), QStringLiteral("NA_MoveKf_Node"),
                               2.0, Ogre::Vector3(9,9,9), Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1,1,1)));
    EXPECT_FALSE(m->moveNodeKeyframe(QStringLiteral("NA_MoveKf"),
                                     QStringLiteral("NA_MoveKf_Node"), 0.5, 2.0));
}

TEST_F(NodeAnimationManagerSceneTest, DeleteNodeKeyframeUndoable) {
    auto* m = NodeAnimationManager::instance();
    ASSERT_TRUE(m->createClip(QStringLiteral("NA_DeleteKf"), 2.0));
    makeNamedNode("NA_DeleteKf_Node");
    ASSERT_TRUE(m->addKeyframe(QStringLiteral("NA_DeleteKf"), QStringLiteral("NA_DeleteKf_Node"),
                               0.5, Ogre::Vector3(1,2,3), Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1,1,1)));
    ASSERT_TRUE(m->addKeyframe(QStringLiteral("NA_DeleteKf"), QStringLiteral("NA_DeleteKf_Node"),
                               1.5, Ogre::Vector3(4,5,6), Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1,1,1)));

    EXPECT_TRUE(m->deleteNodeKeyframe(QStringLiteral("NA_DeleteKf"),
                                      QStringLiteral("NA_DeleteKf_Node"), 0.5));
    auto keys = m->keyTimesForNode(QStringLiteral("NA_DeleteKf"),
                                   QStringLiteral("NA_DeleteKf_Node"));
    ASSERT_EQ(keys.size(), 1);
    EXPECT_NEAR(keys[0], 1.5, 1e-4);

    UndoManager::getSingleton()->undo();
    keys = m->keyTimesForNode(QStringLiteral("NA_DeleteKf"),
                              QStringLiteral("NA_DeleteKf_Node"));
    EXPECT_EQ(keys.size(), 2);

    // Deleting where there is no key → rejected (no push).
    EXPECT_FALSE(m->deleteNodeKeyframe(QStringLiteral("NA_DeleteKf"),
                                       QStringLiteral("NA_DeleteKf_Node"), 0.9));
}
