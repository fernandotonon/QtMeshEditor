#include <gtest/gtest.h>

#include <QString>

#include "commands/ExplodePartsCommand.h"

// No-Ogre / error-branch coverage for ExplodePartsCommand (mirrors
// SplitMeshCommand_test.cpp): ctor/text contract, accessor state before redo(),
// redo() against an unresolvable entity name (→ ok()==false with an error), and
// undo() before any successful redo (strict no-op — no original mesh captured).
// resolveSourceEntity() returns nullptr when Manager::getSingletonPtr() is null
// OR no entity matches, so a bogus name reliably drives the error branch without
// a scene. The full explode→node-create round-trip needs a real GL scene.

namespace {
const std::string kBogusEntity = "__qtmesh_nonexistent_entity_for_explode_test__";
}

TEST(ExplodePartsCommandTest, CtorSetsText)
{
    ExplodePartsCommand cmd(kBogusEntity, 0.5f);
    EXPECT_EQ(cmd.text(), QStringLiteral("Explode into Parts"));
}

TEST(ExplodePartsCommandTest, InitialAccessorState)
{
    ExplodePartsCommand cmd(kBogusEntity, 0.5f);
    EXPECT_FALSE(cmd.ok());
    EXPECT_EQ(cmd.createdParts(), 0);
    EXPECT_TRUE(cmd.partNodeNames().empty());
}

TEST(ExplodePartsCommandTest, RedoOnUnresolvableEntityFailsCleanly)
{
    ExplodePartsCommand cmd(kBogusEntity, 0.5f);
    cmd.redo();                       // no scene / no entity → error branch
    EXPECT_FALSE(cmd.ok());
    EXPECT_FALSE(cmd.error().isEmpty());
    EXPECT_EQ(cmd.createdParts(), 0);
}

TEST(ExplodePartsCommandTest, UndoBeforeRedoIsNoOp)
{
    ExplodePartsCommand cmd(kBogusEntity, 0.5f);
    EXPECT_NO_THROW(cmd.undo());
    EXPECT_FALSE(cmd.ok());
}
