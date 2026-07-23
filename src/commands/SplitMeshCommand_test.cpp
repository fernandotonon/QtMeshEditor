#include <gtest/gtest.h>

#include <QString>

#include "commands/SplitMeshCommand.h"

// No-Ogre / error-branch coverage for SplitMeshCommand: the ctor/text contract,
// accessor state before redo(), redo() against an unresolvable entity name
// (→ ok()==false with an error), and undo() before any successful redo (strict
// no-op — no original mesh captured). resolveEntity() returns nullptr when
// Manager::getSingletonPtr() is null OR no entity matches, so a bogus name
// reliably drives the error branch without a scene. The full segment→split→swap
// round-trip needs a real mesh and is covered by the Ogre-gated CLI split test
// (CLIPipeline_cmdsplitparts_coverage_test.cpp).

namespace {
const std::string kBogusEntity = "__qtmesh_nonexistent_entity_for_split_test__";
}

TEST(SplitMeshCommandTest, CtorSetsText)
{
    SplitMeshCommand cmd(kBogusEntity, 1, QStringLiteral("auto"), false,
                         QStringLiteral("Body"));
    EXPECT_EQ(cmd.text(), QStringLiteral("Split Mesh into Parts"));
}

TEST(SplitMeshCommandTest, InitialAccessorState)
{
    SplitMeshCommand cmd(kBogusEntity, 1, QStringLiteral("auto"), true,
                         QStringLiteral("Body"));
    EXPECT_FALSE(cmd.ok());
    EXPECT_EQ(cmd.createdSubMeshes(), 0);
    EXPECT_TRUE(cmd.partNames().empty());
}

TEST(SplitMeshCommandTest, RedoOnUnresolvableEntityFailsCleanly)
{
    SplitMeshCommand cmd(kBogusEntity, 1, QStringLiteral("auto"), true,
                         QStringLiteral("Body"));
    cmd.redo();                       // no scene / no entity → error branch
    EXPECT_FALSE(cmd.ok());
    EXPECT_FALSE(cmd.error().isEmpty());
    EXPECT_EQ(cmd.createdSubMeshes(), 0);
}

TEST(SplitMeshCommandTest, UndoBeforeRedoIsNoOp)
{
    SplitMeshCommand cmd(kBogusEntity, 1, QStringLiteral("auto"), false,
                         QStringLiteral("Body"));
    // No original mesh captured yet → undo must not crash or mutate anything.
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_FALSE(cmd.ok());
}
