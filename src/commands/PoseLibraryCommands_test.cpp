/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

// Pure-logic tests for the pose-library undo commands.
//
// Every code path reachable with a nullptr Ogre::Entity* + an empty/non-empty
// QString is exercised here. These are the early-return guards CodeRabbit and
// Codex flagged on PR #595:
//   - SavePoseCommand   ctor null/empty guard -> mNewSnapshot left empty,
//                       PoseLibrary singleton never touched; redo/undo no-op
//                       when mEntity == nullptr.
//   - DeletePoseCommand ctor: mWasPresent stays false on null/empty; redo/undo
//                       no-op when !mWasPresent.
//   - ApplyPoseCommand  ctor null/empty guard; redo() clears mRedoApplied on a
//                       null entity; undo() is a strict no-op when !mRedoApplied
//                       (must not clobber later edits).
//
// All command text() formatting is pure-string and asserts without any
// Ogre/display dependency. Ogre::Entity is only forward-declared in the
// command header, so we can construct every command with nullptr without ever
// pulling in OgreEntity.h or initialising Ogre. No QApplication is created
// here (test_main.cpp owns the single instance).

#include <gtest/gtest.h>

#include <QString>

#include "commands/PoseLibraryCommands.h"

// ──────────────── SavePoseCommand ───────────────────────────────────

TEST(PoseLibraryCommandsTest, SaveConstructorSetsCommandText)
{
    SavePoseCommand cmd(nullptr, QStringLiteral("Idle"));
    EXPECT_FALSE(cmd.text().isEmpty());
    EXPECT_EQ(cmd.text(), QStringLiteral("Save pose \"Idle\""));
}

TEST(PoseLibraryCommandsTest, SaveTextFormattingHandlesEmptyName)
{
    // Even with the early-return guard tripped (empty name), the text is set
    // before the guard, so it formats as the empty-name string.
    SavePoseCommand cmd(nullptr, QString());
    EXPECT_EQ(cmd.text(), QStringLiteral("Save pose \"\""));
}

TEST(PoseLibraryCommandsTest, SaveTextFormattingPreservesSpecialChars)
{
    const QString name = QStringLiteral("Pose #1 (left arm) — spécïal");
    SavePoseCommand cmd(nullptr, name);
    EXPECT_EQ(cmd.text(), QStringLiteral("Save pose \"%1\"").arg(name));
}

TEST(PoseLibraryCommandsTest, SaveRedoUndoNoOpOnNullEntity)
{
    // Null entity: redo/undo must early-return and never touch the
    // PoseLibrary singleton. No crash == success.
    SavePoseCommand cmd(nullptr, QStringLiteral("Idle"));
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    // Repeated invocations stay safe (idempotent no-op).
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_EQ(cmd.text(), QStringLiteral("Save pose \"Idle\""));
}

TEST(PoseLibraryCommandsTest, SaveRedoUndoNoOpOnEmptyName)
{
    // Empty name also trips the ctor guard. With a null entity the redo/undo
    // mEntity check fires first, so still a clean no-op.
    SavePoseCommand cmd(nullptr, QString());
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
}

// ──────────────── DeletePoseCommand ─────────────────────────────────

TEST(PoseLibraryCommandsTest, DeleteConstructorSetsCommandText)
{
    DeletePoseCommand cmd(nullptr, QStringLiteral("Wave"));
    EXPECT_FALSE(cmd.text().isEmpty());
    EXPECT_EQ(cmd.text(), QStringLiteral("Delete pose \"Wave\""));
}

TEST(PoseLibraryCommandsTest, DeleteTextFormattingHandlesEmptyName)
{
    DeletePoseCommand cmd(nullptr, QString());
    EXPECT_EQ(cmd.text(), QStringLiteral("Delete pose \"\""));
}

TEST(PoseLibraryCommandsTest, DeleteRedoUndoNoOpOnNullEntity)
{
    // mWasPresent stays false (ctor bailed before hasPose), and the redo/undo
    // guards (`!mEntity || !mWasPresent`) make both a no-op. Must not hit the
    // PoseLibrary singleton.
    DeletePoseCommand cmd(nullptr, QStringLiteral("Wave"));
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_EQ(cmd.text(), QStringLiteral("Delete pose \"Wave\""));
}

TEST(PoseLibraryCommandsTest, DeleteRedoUndoNoOpOnEmptyName)
{
    DeletePoseCommand cmd(nullptr, QString());
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
}

// ──────────────── ApplyPoseCommand ──────────────────────────────────

TEST(PoseLibraryCommandsTest, ApplyConstructorSetsCommandText)
{
    ApplyPoseCommand cmd(nullptr, QStringLiteral("Run"));
    EXPECT_FALSE(cmd.text().isEmpty());
    EXPECT_EQ(cmd.text(), QStringLiteral("Apply pose \"Run\""));
}

TEST(PoseLibraryCommandsTest, ApplyTextFormattingHandlesEmptyName)
{
    ApplyPoseCommand cmd(nullptr, QString());
    EXPECT_EQ(cmd.text(), QStringLiteral("Apply pose \"\""));
}

TEST(PoseLibraryCommandsTest, ApplyRedoClearsAppliedFlagOnNullEntity)
{
    // redo() always resets mRedoApplied = false first, then early-returns on a
    // null entity before reaching the library. Because the apply never
    // happened, undo() must be a strict no-op (it would otherwise clobber later
    // edits with the stale mPreApply snapshot — Codex P1 on PR #595).
    ApplyPoseCommand cmd(nullptr, QStringLiteral("Run"));
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_EQ(cmd.text(), QStringLiteral("Apply pose \"Run\""));
}

TEST(PoseLibraryCommandsTest, ApplyUndoBeforeRedoIsNoOp)
{
    // undo() without a preceding successful redo() (mRedoApplied == false) must
    // be a strict no-op. With a null entity the `!mEntity` guard also fires,
    // but the important contract is that no apply/restore happens.
    ApplyPoseCommand cmd(nullptr, QStringLiteral("Run"));
    EXPECT_NO_THROW(cmd.undo());
}

TEST(PoseLibraryCommandsTest, ApplyRedoUndoNoOpOnEmptyName)
{
    ApplyPoseCommand cmd(nullptr, QString());
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
}

// ──────────────── Cross-command text-formatting independence ─────────

TEST(PoseLibraryCommandsTest, EachCommandTypeHasDistinctTextPrefix)
{
    const QString name = QStringLiteral("T-Pose");
    SavePoseCommand save(nullptr, name);
    DeletePoseCommand del(nullptr, name);
    ApplyPoseCommand apply(nullptr, name);

    EXPECT_TRUE(save.text().startsWith(QStringLiteral("Save pose")));
    EXPECT_TRUE(del.text().startsWith(QStringLiteral("Delete pose")));
    EXPECT_TRUE(apply.text().startsWith(QStringLiteral("Apply pose")));

    // All embed the pose name verbatim inside the quotes.
    EXPECT_TRUE(save.text().contains(name));
    EXPECT_TRUE(del.text().contains(name));
    EXPECT_TRUE(apply.text().contains(name));
}
