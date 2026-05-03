#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <QUndoStack>
#include <QVector>

#include "BulkKeyframeCommands.h"
#include "../Manager.h"
#include "../TestHelpers.h"

#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreKeyFrame.h>

#include <cmath>

class BulkKeyframeCommandsTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }
    void TearDown() override {
        if (app) app->processEvents();
    }
    QApplication* app = nullptr;

    static bool hasKeyframeAt(Ogre::NodeAnimationTrack* track, float time) {
        // Match production's <= tolerance (BulkKeyframeCommands.cpp's
        // findKeyframeIndex uses kEpsilon = 0.001f with `<=`). Using `<`
        // here would let an exactly-at-epsilon offset slip through the
        // collision check in production but report false in this test.
        for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
            if (std::fabs(track->getKeyFrame(i)->getTime() - time) <= 0.001f) return true;
        }
        return false;
    }
};

TEST_F(BulkKeyframeCommandsTest, MoveKeyframesRedoUndoRoundTrip) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("BKC_RedoUndoTest");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = skel->getAnimation("TestAnim")->_getNodeTrackList().begin()->second;

    // TestAnim has keyframes at 0.0, 0.5, 1.0 on the Child bone.
    // Extend length so 0.0+0.2 / 0.5+0.2 / 1.0+0.2 fit within bounds.
    skel->getAnimation("TestAnim")->setLength(2.0f);

    QVector<MoveKeyframesCommand::Item> items;
    items.append({ "Child", 0.0f });
    items.append({ "Child", 0.5f });
    items.append({ "Child", 1.0f });

    QUndoStack stack;
    stack.push(new MoveKeyframesCommand(skel, "TestAnim", items, 0.2f));

    // After redo: keyframes at 0.2, 0.7, 1.2.
    EXPECT_EQ(track->getNumKeyFrames(), 3u);
    EXPECT_TRUE(hasKeyframeAt(track, 0.2f));
    EXPECT_TRUE(hasKeyframeAt(track, 0.7f));
    EXPECT_TRUE(hasKeyframeAt(track, 1.2f));
    EXPECT_FALSE(hasKeyframeAt(track, 0.0f));
    EXPECT_FALSE(hasKeyframeAt(track, 0.5f));
    EXPECT_FALSE(hasKeyframeAt(track, 1.0f));

    stack.undo();
    EXPECT_TRUE(hasKeyframeAt(track, 0.0f));
    EXPECT_TRUE(hasKeyframeAt(track, 0.5f));
    EXPECT_TRUE(hasKeyframeAt(track, 1.0f));
    EXPECT_FALSE(hasKeyframeAt(track, 0.2f));

    stack.redo();
    EXPECT_TRUE(hasKeyframeAt(track, 0.7f));
}

TEST_F(BulkKeyframeCommandsTest, MoveKeyframesPreservesIntraTrackOrdering) {
    // Forward shift past an adjacent keyframe used to collide if the rewrite
    // wasn't two-phase. This test guards the park-then-move trick in shiftAll.
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("BKC_OrderingTest");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = skel->getAnimation("TestAnim")->_getNodeTrackList().begin()->second;
    skel->getAnimation("TestAnim")->setLength(2.0f);

    // Move 0.0 → 0.5 and 0.5 → 1.0 in the same command. The 0.5 keyframe
    // would already be there when we try to write 0.0 → 0.5 if we naively
    // rewrote in order.
    QVector<MoveKeyframesCommand::Item> items;
    items.append({ "Child", 0.0f });
    items.append({ "Child", 0.5f });

    QUndoStack stack;
    stack.push(new MoveKeyframesCommand(skel, "TestAnim", items, 0.5f));

    EXPECT_EQ(track->getNumKeyFrames(), 3u); // 0.5, 1.0 (moved), and 1.0 (original)
    EXPECT_TRUE(hasKeyframeAt(track, 0.5f));
    EXPECT_TRUE(hasKeyframeAt(track, 1.0f));
    EXPECT_FALSE(hasKeyframeAt(track, 0.0f));
}

TEST_F(BulkKeyframeCommandsTest, PasteRedoUndoRoundTrip) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("BKC_PasteRedoUndoTest");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = skel->getAnimation("TestAnim")->_getNodeTrackList().begin()->second;
    skel->getAnimation("TestAnim")->setLength(3.0f);
    const int before = track->getNumKeyFrames();

    QVector<PasteKeyframesCommand::Entry> entries;
    PasteKeyframesCommand::Entry e1{};
    e1.boneName = "Child"; e1.time = 1.5f;
    e1.tx = 0.1f; e1.rw = 1.0f; e1.sx = e1.sy = e1.sz = 1.0f;
    PasteKeyframesCommand::Entry e2{};
    e2.boneName = "Child"; e2.time = 2.5f;
    e2.tx = 0.2f; e2.rw = 1.0f; e2.sx = e2.sy = e2.sz = 1.0f;
    entries.append(e1); entries.append(e2);

    auto* cmd = new PasteKeyframesCommand(skel, "TestAnim", entries);
    QUndoStack stack;
    stack.push(cmd);

    EXPECT_EQ(cmd->pastedCount(), 2);
    EXPECT_EQ(track->getNumKeyFrames(), before + 2);
    EXPECT_TRUE(hasKeyframeAt(track, 1.5f));
    EXPECT_TRUE(hasKeyframeAt(track, 2.5f));

    stack.undo();
    EXPECT_EQ(track->getNumKeyFrames(), before);
    EXPECT_FALSE(hasKeyframeAt(track, 1.5f));
    EXPECT_FALSE(hasKeyframeAt(track, 2.5f));

    stack.redo();
    EXPECT_TRUE(hasKeyframeAt(track, 1.5f));
}

TEST_F(BulkKeyframeCommandsTest, PasteSkipsCollisionsAndUndoOnlyRemovesPasted) {
    ASSERT_TRUE(canLoadMeshFiles());
    Ogre::Entity* entity = createAnimatedTestEntity("BKC_PasteSkipTest");
    ASSERT_NE(entity, nullptr);
    auto* skel = entity->getSkeleton();
    auto* track = skel->getAnimation("TestAnim")->_getNodeTrackList().begin()->second;
    const int before = track->getNumKeyFrames();

    // One entry collides with the existing keyframe at 0.5; the other (0.7)
    // should land. Undo must only remove 0.7 — leaving the existing 0.5 alone.
    QVector<PasteKeyframesCommand::Entry> entries;
    PasteKeyframesCommand::Entry collide{};
    collide.boneName = "Child"; collide.time = 0.5f;
    collide.rw = 1.0f; collide.sx = collide.sy = collide.sz = 1.0f;
    PasteKeyframesCommand::Entry novel{};
    novel.boneName = "Child"; novel.time = 0.7f;
    novel.rw = 1.0f; novel.sx = novel.sy = novel.sz = 1.0f;
    entries.append(collide); entries.append(novel);

    auto* cmd = new PasteKeyframesCommand(skel, "TestAnim", entries);
    QUndoStack stack;
    stack.push(cmd);

    EXPECT_EQ(cmd->pastedCount(), 1);
    EXPECT_TRUE(hasKeyframeAt(track, 0.5f));  // existing, untouched
    EXPECT_TRUE(hasKeyframeAt(track, 0.7f));  // newly pasted
    EXPECT_EQ(track->getNumKeyFrames(), before + 1);

    stack.undo();
    EXPECT_TRUE(hasKeyframeAt(track, 0.5f));  // existing, still there
    EXPECT_FALSE(hasKeyframeAt(track, 0.7f)); // pasted one removed
    EXPECT_EQ(track->getNumKeyFrames(), before);
}
