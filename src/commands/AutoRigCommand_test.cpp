#include <gtest/gtest.h>

#include <QString>

#include "commands/AutoRigCommand.h"
#include "AutoRig.h"
#include "Manager.h"

// These tests exercise the no-Ogre / error-report branches of AutoRigCommand
// (the ones that need NO scene and NO display):
//
//   * ctor / setText contract (plain "Auto-Rig" vs "Auto-Rig from Markers"),
//   * report()/applied()/skinned() accessors before redo(),
//   * redo() against an unresolvable entity name → error branch (applied==false),
//   * undo() before any successful redo → strict no-op (guarded on applied).
//
// resolveEntity() returns nullptr when Manager::getSingletonPtr() is null OR no
// entity matches, so a bogus name reliably drives the error branch. The actual
// rig + skin attach/detach round-trip needs a real mesh and is covered by an
// Ogre-gated layer on CI.

namespace {
const std::string kBogusEntity =
    "__qtmesh_nonexistent_entity_for_autorig_test__";

AutoRig::Options humanoidOpts() {
    AutoRig::Options o;
    o.tmpl   = AutoRig::Template::Humanoid;
    o.upAxis = 1;
    return o;
}
} // namespace

// ---- ctor / text() -------------------------------------------------------

TEST(AutoRigCommandTest, CtorSetsPlainTextWithoutMarkers) {
    AutoRigCommand cmd(kBogusEntity, humanoidOpts(), {}, /*alsoSkin=*/false);
    EXPECT_EQ(cmd.text(), QStringLiteral("Auto-Rig"));
}

TEST(AutoRigCommandTest, CtorSetsMarkerTextWithMarkers) {
    AutoRig::Marker m;
    m.id  = AutoRig::MarkerId::Hips;
    m.set = true;
    m.pos = {0, 0, 0};
    AutoRigCommand cmd(kBogusEntity, humanoidOpts(), {m}, /*alsoSkin=*/true);
    EXPECT_EQ(cmd.text(), QStringLiteral("Auto-Rig from Markers"));
}

// ---- initial accessor state ----------------------------------------------

TEST(AutoRigCommandTest, ReportInitiallyNotApplied) {
    AutoRigCommand cmd(kBogusEntity, humanoidOpts(), {}, false);
    EXPECT_FALSE(cmd.applied());
    EXPECT_FALSE(cmd.report().applied);
    EXPECT_FALSE(cmd.skinned());
    EXPECT_TRUE(cmd.report().error.isEmpty());
    EXPECT_EQ(cmd.report().boneCount, 0);
    EXPECT_EQ(cmd.report().markersApplied, 0);
}

// ---- redo() on an unresolvable entity → error branch ---------------------

TEST(AutoRigCommandTest, RedoOnBogusEntitySetsErrorReport) {
    AutoRigCommand cmd(kBogusEntity, humanoidOpts(), {}, false);
    cmd.redo();
    EXPECT_FALSE(cmd.applied());
    EXPECT_EQ(cmd.report().error, QStringLiteral("Entity no longer in scene."));
}

TEST(AutoRigCommandTest, RedoWithNoManagerSingleton) {
    Manager::kill();
    ASSERT_EQ(Manager::getSingletonPtr(), nullptr);
    AutoRigCommand cmd(kBogusEntity, humanoidOpts(), {}, true);
    cmd.redo();
    EXPECT_FALSE(cmd.applied());
    EXPECT_FALSE(cmd.skinned());
    EXPECT_EQ(cmd.report().error, QStringLiteral("Entity no longer in scene."));
}

TEST(AutoRigCommandTest, RedoOnBogusEntityIsIdempotent) {
    AutoRigCommand cmd(kBogusEntity, humanoidOpts(), {}, false);
    cmd.redo();
    cmd.redo();
    EXPECT_FALSE(cmd.applied());
    EXPECT_EQ(cmd.report().error, QStringLiteral("Entity no longer in scene."));
}

// ---- undo() before a successful redo → no-op -----------------------------

TEST(AutoRigCommandTest, UndoBeforeApplyIsNoOp) {
    AutoRigCommand cmd(kBogusEntity, humanoidOpts(), {}, false);
    // applied is false → undo() must early-return without touching anything.
    EXPECT_NO_FATAL_FAILURE(cmd.undo());
    EXPECT_FALSE(cmd.applied());

    // Same after a failed redo (still not applied).
    cmd.redo();
    EXPECT_NO_FATAL_FAILURE(cmd.undo());
    EXPECT_FALSE(cmd.applied());
}
