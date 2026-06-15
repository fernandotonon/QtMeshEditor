/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

// Unit tests for the node-animation undo commands
// (CreateNodeAnimClipCommand / DeleteNodeAnimClipCommand /
// SetNodeKeyframeCommand) and their supporting pure-data snapshot
// structs.
//
// These commands bottom out on two singletons:
//   * Manager::getSingletonPtr() -> sceneMgr()  (null in a headless test)
//   * NodeAnimationManager::instance()          (null unless constructed)
// When both are null every redo()/undo() is a graceful no-op, and the
// constructors only set the command text + leave snapshots empty. That
// makes the no-op branches, text() formatting, and the default-init'd
// snapshot structs all testable without initializing Ogre or a display.
//
// We deliberately do NOT init Ogre. We rely on Manager::getSingletonPtr()
// being null. As a safety net, NodeAnimCommandsTest::SetUp kills any
// pre-existing Manager singleton so sceneMgr() returns null regardless
// of what an earlier suite left behind.

#include <gtest/gtest.h>

#include <QString>

#include <optional>

#include <OgreQuaternion.h>
#include <OgreVector.h>

#include "commands/NodeAnimCommands.h"
#include "Manager.h"
#include "NodeAnimationManager.h"

namespace {

// Ensure the Manager singleton is gone so sceneMgr() resolves to null
// and the commands take their headless no-op branches deterministically.
class NodeAnimCommandsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (Manager::getSingletonPtr() != nullptr)
            Manager::kill();
    }
};

} // namespace

// ─────────────────── Pure-data snapshot structs ─────────────────────

TEST(NodeKeyframeSnapshotTest, DefaultsAreSaneIdentity)
{
    NodeKeyframeSnapshot ks;
    EXPECT_DOUBLE_EQ(ks.time, 0.0);
    EXPECT_EQ(ks.translate, Ogre::Vector3::ZERO);
    EXPECT_EQ(ks.rotation, Ogre::Quaternion::IDENTITY);
    // scale defaults to (1,1,1), NOT zero.
    EXPECT_EQ(ks.scale, Ogre::Vector3(1, 1, 1));
}

TEST(NodeKeyframeSnapshotTest, FieldsAreAssignable)
{
    NodeKeyframeSnapshot ks;
    ks.time = 2.5;
    ks.translate = Ogre::Vector3(1, 2, 3);
    ks.rotation = Ogre::Quaternion(Ogre::Degree(90), Ogre::Vector3::UNIT_Y);
    ks.scale = Ogre::Vector3(2, 2, 2);

    EXPECT_DOUBLE_EQ(ks.time, 2.5);
    EXPECT_EQ(ks.translate, Ogre::Vector3(1, 2, 3));
    EXPECT_EQ(ks.scale, Ogre::Vector3(2, 2, 2));
    EXPECT_FALSE(ks.rotation == Ogre::Quaternion::IDENTITY);
}

TEST(NodeTrackSnapshotTest, DefaultsEmpty)
{
    NodeTrackSnapshot ts;
    EXPECT_TRUE(ts.nodeName.isEmpty());
    EXPECT_TRUE(ts.keys.empty());
}

TEST(NodeTrackSnapshotTest, HoldsKeysInOrder)
{
    NodeTrackSnapshot ts;
    ts.nodeName = QStringLiteral("Bone.001");

    NodeKeyframeSnapshot a;
    a.time = 0.0;
    NodeKeyframeSnapshot b;
    b.time = 1.0;
    ts.keys.push_back(a);
    ts.keys.push_back(b);

    ASSERT_EQ(ts.keys.size(), 2u);
    EXPECT_EQ(ts.nodeName, QStringLiteral("Bone.001"));
    EXPECT_DOUBLE_EQ(ts.keys[0].time, 0.0);
    EXPECT_DOUBLE_EQ(ts.keys[1].time, 1.0);
}

// ─────────────────── CreateNodeAnimClipCommand ──────────────────────

TEST_F(NodeAnimCommandsTest, CreateClipTextMatchesSpec)
{
    CreateNodeAnimClipCommand cmd(QStringLiteral("Walk"), 2.0);
    EXPECT_EQ(cmd.text(), QStringLiteral("Create node clip \"Walk\""));
}

TEST_F(NodeAnimCommandsTest, CreateClipTextWithEmptyName)
{
    CreateNodeAnimClipCommand cmd(QString(), 0.0);
    EXPECT_EQ(cmd.text(), QStringLiteral("Create node clip \"\""));
}

TEST_F(NodeAnimCommandsTest, CreateClipTextWithSpecialChars)
{
    CreateNodeAnimClipCommand cmd(QStringLiteral("Idle 02 (loop)"), 1.5);
    EXPECT_EQ(cmd.text(),
              QStringLiteral("Create node clip \"Idle 02 (loop)\""));
}

TEST_F(NodeAnimCommandsTest, CreateClipRedoUndoNoOpWhenNoManager)
{
    // Whatever the singleton state (other suites in this process may have
    // created NodeAnimationManager / Manager), redo()/undo() must not crash —
    // they early-return when the scene/manager isn't usable. We assert only
    // the externally observable contract: no throw, text unchanged.
    CreateNodeAnimClipCommand cmd(QStringLiteral("Run"), 3.0);
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    // Repeated cycling stays a no-op.
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    // Text is unchanged by redo/undo.
    EXPECT_EQ(cmd.text(), QStringLiteral("Create node clip \"Run\""));
}

// ─────────────────── DeleteNodeAnimClipCommand ──────────────────────

TEST_F(NodeAnimCommandsTest, DeleteClipTextMatchesSpec)
{
    DeleteNodeAnimClipCommand cmd(QStringLiteral("Jump"));
    EXPECT_EQ(cmd.text(), QStringLiteral("Delete node clip \"Jump\""));
}

TEST_F(NodeAnimCommandsTest, DeleteClipTextWithEmptyName)
{
    DeleteNodeAnimClipCommand cmd{QString()};
    EXPECT_EQ(cmd.text(), QStringLiteral("Delete node clip \"\""));
}

TEST_F(NodeAnimCommandsTest, DeleteClipCtorSnapshotsEmptyWhenNoScene)
{
    // Without a usable scene the constructor's sceneMgr() block is skipped,
    // so nothing is captured; undo() must be a clean no-op regardless of
    // singleton state.
    DeleteNodeAnimClipCommand cmd(QStringLiteral("Crouch"));
    EXPECT_NO_THROW(cmd.undo());
}

TEST_F(NodeAnimCommandsTest, DeleteClipRedoUndoNoOpWhenNoManager)
{
    DeleteNodeAnimClipCommand cmd(QStringLiteral("Attack"));
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_EQ(cmd.text(), QStringLiteral("Delete node clip \"Attack\""));
}

// ─────────────────── SetNodeKeyframeCommand ─────────────────────────

TEST_F(NodeAnimCommandsTest, SetKeyframeTextFormatsTimeTwoDecimals)
{
    SetNodeKeyframeCommand cmd(QStringLiteral("Walk"),
                               QStringLiteral("Hips"),
                               1.5,
                               Ogre::Vector3(1, 0, 0),
                               Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1, 1, 1));
    EXPECT_EQ(cmd.text(),
              QStringLiteral("Keyframe \"Walk\"@1.50s on 'Hips'"));
}

TEST_F(NodeAnimCommandsTest, SetKeyframeTextRoundsTime)
{
    // 0.125 -> "0.12" (banker's/round-half-to-even or round-half-up;
    // Qt's 'f' uses round-half-to-even, giving 0.12). The exact rule
    // doesn't matter for the spec — only that it is 2 decimals. We
    // pick a value with no ambiguity in the 2-decimal output.
    SetNodeKeyframeCommand cmd(QStringLiteral("Run"),
                               QStringLiteral("Spine"),
                               2.345,
                               Ogre::Vector3::ZERO,
                               Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1, 1, 1));
    EXPECT_EQ(cmd.text(),
              QStringLiteral("Keyframe \"Run\"@2.35s on 'Spine'"));
}

TEST_F(NodeAnimCommandsTest, SetKeyframeTextZeroTime)
{
    SetNodeKeyframeCommand cmd(QStringLiteral("Idle"),
                               QStringLiteral("Root"),
                               0.0,
                               Ogre::Vector3::ZERO,
                               Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1, 1, 1));
    EXPECT_EQ(cmd.text(),
              QStringLiteral("Keyframe \"Idle\"@0.00s on 'Root'"));
}

TEST_F(NodeAnimCommandsTest, SetKeyframeTextEmbedsNodeName)
{
    SetNodeKeyframeCommand cmd(QStringLiteral("Clip"),
                               QStringLiteral("LeftHand_End"),
                               10.0,
                               Ogre::Vector3::ZERO,
                               Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1, 1, 1));
    EXPECT_TRUE(cmd.text().contains(QStringLiteral("LeftHand_End")));
    EXPECT_EQ(cmd.text(),
              QStringLiteral("Keyframe \"Clip\"@10.00s on 'LeftHand_End'"));
}

TEST_F(NodeAnimCommandsTest, SetKeyframeCtorNoPriorWhenNoScene)
{
    // The constructor's prior-keyframe scan only runs with a usable scene;
    // without one mPriorKeyframe stays empty. We assert the externally
    // observable consequence regardless of singleton state: redo()/undo()
    // don't throw.
    SetNodeKeyframeCommand cmd(QStringLiteral("Walk"),
                               QStringLiteral("Hips"),
                               0.5,
                               Ogre::Vector3(1, 2, 3),
                               Ogre::Quaternion(Ogre::Degree(45),
                                                Ogre::Vector3::UNIT_X),
                               Ogre::Vector3(2, 2, 2));
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
}

TEST_F(NodeAnimCommandsTest, SetKeyframeRedoUndoNoOpWhenNoManager)
{
    SetNodeKeyframeCommand cmd(QStringLiteral("Dance"),
                               QStringLiteral("Chest"),
                               4.25,
                               Ogre::Vector3::UNIT_Z,
                               Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1, 1, 1));
    // redo() returns early at sceneMgr()==null / manager==null;
    // undo() returns early at scene==null. Cycle multiple times.
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_EQ(cmd.text(),
              QStringLiteral("Keyframe \"Dance\"@4.25s on 'Chest'"));
}

TEST_F(NodeAnimCommandsTest, SetKeyframeUndoBeforeRedoIsSafe)
{
    // undo() without a preceding redo() must still early-return rather than
    // dereference anything, regardless of singleton state.
    SetNodeKeyframeCommand cmd(QStringLiteral("X"),
                               QStringLiteral("Y"),
                               1.0,
                               Ogre::Vector3::ZERO,
                               Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1, 1, 1));
    EXPECT_NO_THROW(cmd.undo());
}

TEST_F(NodeAnimCommandsTest, SetKeyframeNegativeTimeFormats)
{
    // Defensive: negative time still formats to 2 decimals with sign.
    SetNodeKeyframeCommand cmd(QStringLiteral("Clip"),
                               QStringLiteral("Node"),
                               -1.5,
                               Ogre::Vector3::ZERO,
                               Ogre::Quaternion::IDENTITY,
                               Ogre::Vector3(1, 1, 1));
    EXPECT_EQ(cmd.text(),
              QStringLiteral("Keyframe \"Clip\"@-1.50s on 'Node'"));
}
