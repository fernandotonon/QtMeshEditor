#include <gtest/gtest.h>

#include <QString>

#include "commands/JoinPartsCommand.h"

// No-Ogre / error-branch coverage for JoinPartsCommand (mirrors
// SplitMeshCommand_test.cpp): ctor/text contract, accessor state before redo(),
// redo() against unresolvable entity names (→ ok()==false with an error), and
// undo() before any successful redo (strict no-op). buildOnce() fails to find a
// bogus entity so redo() drives the error branch without a scene. The full
// join→fuse round-trip needs a real GL scene.

namespace {
const std::string kBogusA = "__qtmesh_nonexistent_part_A__";
const std::string kBogusB = "__qtmesh_nonexistent_part_B__";
}

TEST(JoinPartsCommandTest, CtorSetsText)
{
    JoinPartsCommand cmd({kBogusA, kBogusB}, QStringLiteral("fused"));
    EXPECT_EQ(cmd.text(), QStringLiteral("Join Parts"));
}

TEST(JoinPartsCommandTest, InitialAccessorState)
{
    JoinPartsCommand cmd({kBogusA, kBogusB}, QStringLiteral("fused"));
    EXPECT_FALSE(cmd.ok());
    EXPECT_EQ(cmd.createdSubMeshes(), 0);
    EXPECT_TRUE(cmd.fusedNodeName().empty());
}

TEST(JoinPartsCommandTest, RedoOnUnresolvableEntitiesFailsCleanly)
{
    JoinPartsCommand cmd({kBogusA, kBogusB}, QStringLiteral("fused"));
    cmd.redo();                       // no scene / no entities → error branch
    EXPECT_FALSE(cmd.ok());
    EXPECT_FALSE(cmd.error().isEmpty());
    EXPECT_EQ(cmd.createdSubMeshes(), 0);
}

TEST(JoinPartsCommandTest, UndoBeforeRedoIsNoOp)
{
    JoinPartsCommand cmd({kBogusA, kBogusB}, QStringLiteral("fused"));
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_FALSE(cmd.ok());
}
