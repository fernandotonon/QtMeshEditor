#include <gtest/gtest.h>

#include <QString>

#include "commands/AddPrintPegsCommand.h"
#include "SubMeshOps.h"

// No-Ogre / error-branch coverage for AddPrintPegsCommand (mirrors
// SplitMeshCommand_test.cpp): ctor/text contract, accessor state before redo(),
// redo() against an unresolvable entity (→ ok()==false with an error), and
// undo() before any successful redo (strict no-op). The full split→peg→export
// round-trip is covered by the CLI print-pegs path (verified on Hip Hop
// Dancing.obj: 5 boundaries pegged) and the pure-data SubMeshOps peg tests.

namespace {
const std::string kBogusEntity = "__qtmesh_nonexistent_entity_for_pegs_test__";
}

TEST(AddPrintPegsCommandTest, CtorSetsText)
{
    AddPrintPegsCommand cmd(kBogusEntity, SubMeshOps::PegOptions{});
    EXPECT_EQ(cmd.text(), QStringLiteral("Add Print Alignment Pegs"));
}

TEST(AddPrintPegsCommandTest, InitialAccessorState)
{
    AddPrintPegsCommand cmd(kBogusEntity, SubMeshOps::PegOptions{});
    EXPECT_FALSE(cmd.ok());
    EXPECT_EQ(cmd.peggedBoundaries(), 0);
    EXPECT_EQ(cmd.totalPegs(), 0);
    EXPECT_TRUE(cmd.warnings().empty());
}

TEST(AddPrintPegsCommandTest, RedoOnUnresolvableEntityFailsCleanly)
{
    AddPrintPegsCommand cmd(kBogusEntity, SubMeshOps::PegOptions{});
    cmd.redo();                       // no scene / no entity → error branch
    EXPECT_FALSE(cmd.ok());
    EXPECT_FALSE(cmd.error().isEmpty());
    EXPECT_EQ(cmd.totalPegs(), 0);
}

TEST(AddPrintPegsCommandTest, UndoBeforeRedoIsNoOp)
{
    AddPrintPegsCommand cmd(kBogusEntity, SubMeshOps::PegOptions{});
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_FALSE(cmd.ok());
}
