/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — unit tests for MorphCommands

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

// These tests cover the *headless-reachable* surface of the three morph
// authoring commands: the QUndoCommand text() formatting, the
// null-entity guards in redo()/undo() (every method early-returns when
// mEntity == nullptr, so they are safe no-ops), and the pure-data
// MorphPoseSlice struct defaults. Everything else (snapshotByName,
// buildPosesFromSlices, removePosesByName) requires a real Ogre::Mesh
// with poses + an Animation and is therefore Ogre-gated and not
// exercised here. We deliberately pass nullptr for Ogre::Entity* — the
// type is only forward-declared in the header, so no Ogre runtime is
// needed to construct or drive the commands through their guards.

#include <gtest/gtest.h>

#include <QString>

#include <map>
#include <vector>

#include <OgreVector.h>

#include "commands/MorphCommands.h"

namespace {

// Build a sample slice list so the ctor's copy into mSlices is
// exercised with real, distinct data.
std::vector<MorphPoseSlice> makeSlices()
{
    std::vector<MorphPoseSlice> slices;

    MorphPoseSlice a;
    a.submeshHandle = 1;
    a.offsets[0] = Ogre::Vector3f(1.0f, 0.0f, 0.0f);
    a.offsets[5] = Ogre::Vector3f(0.0f, 2.0f, 0.0f);
    slices.push_back(a);

    MorphPoseSlice b;
    b.submeshHandle = 2;
    b.offsets[3] = Ogre::Vector3f(0.0f, 0.0f, -1.0f);
    slices.push_back(b);

    return slices;
}

} // namespace

// ──────────────── MorphPoseSlice (pure data) ────────────────────────

TEST(MorphPoseSliceTest, DefaultFieldsMatchOgre1BasedConvention)
{
    MorphPoseSlice slice;
    // Ogre's 1-based submesh convention (0 = shared verts).
    EXPECT_EQ(slice.submeshHandle, static_cast<unsigned short>(1));
    EXPECT_TRUE(slice.offsets.empty());
}

TEST(MorphPoseSliceTest, OffsetsArePopulatableAndQueryable)
{
    MorphPoseSlice slice;
    slice.submeshHandle = 7;
    slice.offsets[2] = Ogre::Vector3f(1.5f, -2.0f, 3.25f);

    EXPECT_EQ(slice.submeshHandle, static_cast<unsigned short>(7));
    ASSERT_EQ(slice.offsets.size(), 1u);
    ASSERT_TRUE(slice.offsets.count(2) == 1);
    const Ogre::Vector3f& v = slice.offsets.at(2);
    EXPECT_FLOAT_EQ(v.x, 1.5f);
    EXPECT_FLOAT_EQ(v.y, -2.0f);
    EXPECT_FLOAT_EQ(v.z, 3.25f);
}

TEST(MorphPoseSliceTest, CopyPreservesHandleAndOffsets)
{
    auto slices = makeSlices();
    ASSERT_EQ(slices.size(), 2u);

    std::vector<MorphPoseSlice> copy = slices;
    ASSERT_EQ(copy.size(), 2u);
    EXPECT_EQ(copy[0].submeshHandle, static_cast<unsigned short>(1));
    EXPECT_EQ(copy[0].offsets.size(), 2u);
    EXPECT_EQ(copy[1].submeshHandle, static_cast<unsigned short>(2));
    EXPECT_EQ(copy[1].offsets.size(), 1u);
}

// ──────────────── AddMorphTargetCommand ─────────────────────────────

TEST(AddMorphTargetCommandTest, TextFormatting)
{
    AddMorphTargetCommand cmd(nullptr, QStringLiteral("Smile"), makeSlices());
    EXPECT_EQ(cmd.text(), QStringLiteral("Add morph target \"Smile\""));
}

TEST(AddMorphTargetCommandTest, TextFormattingEmptyName)
{
    AddMorphTargetCommand cmd(nullptr, QString(), {});
    EXPECT_EQ(cmd.text(), QStringLiteral("Add morph target \"\""));
}

TEST(AddMorphTargetCommandTest, TextFormattingUnicodeName)
{
    const QString name = QStringLiteral("ñ_target_漢字");
    AddMorphTargetCommand cmd(nullptr, name, {});
    EXPECT_EQ(cmd.text(), QStringLiteral("Add morph target \"%1\"").arg(name));
}

// ──────────────── DeleteMorphTargetCommand ──────────────────────────

TEST(DeleteMorphTargetCommandTest, TextFormatting)
{
    DeleteMorphTargetCommand cmd(nullptr, QStringLiteral("Blink"));
    EXPECT_EQ(cmd.text(), QStringLiteral("Delete morph target \"Blink\""));
}

TEST(DeleteMorphTargetCommandTest, TextFormattingEmptyName)
{
    DeleteMorphTargetCommand cmd(nullptr, QString());
    EXPECT_EQ(cmd.text(), QStringLiteral("Delete morph target \"\""));
}

TEST(DeleteMorphTargetCommandTest, RedoUndoAreNoOpsWithNullEntity)
{
    // With a null entity the ctor takes neither snapshot branch, so
    // mSnapshot stays empty and redo/undo early-return.
    DeleteMorphTargetCommand cmd(nullptr, QStringLiteral("Wink"));
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_EQ(cmd.text(), QStringLiteral("Delete morph target \"Wink\""));
}

// ──────────────── RenameMorphTargetCommand ──────────────────────────

TEST(RenameMorphTargetCommandTest, TextFormattingUsesArrowGlyph)
{
    RenameMorphTargetCommand cmd(nullptr, QStringLiteral("Old"),
                                 QStringLiteral("New"));
    EXPECT_EQ(cmd.text(),
              QStringLiteral("Rename morph target \"Old\" → \"New\""));
}

TEST(RenameMorphTargetCommandTest, TextFormattingEmptyNames)
{
    RenameMorphTargetCommand cmd(nullptr, QString(), QString());
    EXPECT_EQ(cmd.text(),
              QStringLiteral("Rename morph target \"\" → \"\""));
}

TEST(RenameMorphTargetCommandTest, TextFormattingSameOldAndNew)
{
    RenameMorphTargetCommand cmd(nullptr, QStringLiteral("Same"),
                                 QStringLiteral("Same"));
    EXPECT_EQ(cmd.text(),
              QStringLiteral("Rename morph target \"Same\" → \"Same\""));
}

TEST(RenameMorphTargetCommandTest, RedoUndoAreNoOpsWithNullEntity)
{
    RenameMorphTargetCommand cmd(nullptr, QStringLiteral("A"),
                                 QStringLiteral("B"));
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_NO_THROW(cmd.redo());
    EXPECT_EQ(cmd.text(),
              QStringLiteral("Rename morph target \"A\" → \"B\""));
}

// ──────────────── Cross-cutting: QUndoCommand parent chaining ────────

TEST(MorphCommandsTest, ParentChainingDoesNotCrash)
{
    // Passing a parent transfers ownership to the parent's child list;
    // verify construction with a parent works and child text is set.
    QUndoCommand root;
    auto* add = new AddMorphTargetCommand(nullptr, QStringLiteral("P1"),
                                          makeSlices(), &root);
    auto* del = new DeleteMorphTargetCommand(nullptr, QStringLiteral("P2"),
                                             &root);
    auto* ren = new RenameMorphTargetCommand(nullptr, QStringLiteral("P3a"),
                                             QStringLiteral("P3b"), &root);

    EXPECT_EQ(root.childCount(), 3);
    EXPECT_EQ(add->text(), QStringLiteral("Add morph target \"P1\""));
    EXPECT_EQ(del->text(), QStringLiteral("Delete morph target \"P2\""));
    EXPECT_EQ(ren->text(),
              QStringLiteral("Rename morph target \"P3a\" → \"P3b\""));
    // root owns the children; no manual delete.
}
