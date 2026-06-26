#include <gtest/gtest.h>

#include <QString>

#include "commands/ComputeSkinWeightsCommand.h"
#include "SkinWeights.h"
#include "Manager.h"

// These tests exercise the pure-logic / error-report branches of
// ComputeSkinWeightsCommand that require NO Ogre scene and NO display:
//
//   * the ctor / setText("Compute Skin Weights") contract,
//   * report()/applied() accessors before and after redo(),
//   * redo() against an unresolvable entity name → the
//       "entity not found / no mesh" error branch (applied == false),
//   * undo() before any successful redo → strict no-op via the
//       !mCaptured guard (no crash, no side effects).
//
// resolveEntity() returns nullptr when Manager::getSingletonPtr()
// is null OR when no entity matches the requested name, so a bogus
// entity name reliably drives the error branch regardless of whether
// some earlier suite left a Manager singleton alive. Snapshot
// capture/restore needs a real skinned mesh and is intentionally
// left to an Ogre-gated layer.

namespace {

// A name no real entity in any scene would ever carry.
const std::string kBogusEntity =
    "__qtmesh_nonexistent_entity_for_skinweights_test__";

SkinWeightsOptions defaultOpts() {
    return SkinWeightsOptions{}; // documented defaults
}

} // namespace

// ---------------------------------------------------------------------------
// Constructor / text() contract
// ---------------------------------------------------------------------------

TEST(ComputeSkinWeightsCommandTest, CtorSetsCommandText) {
    ComputeSkinWeightsCommand cmd(kBogusEntity, defaultOpts());
    EXPECT_EQ(cmd.text(), QStringLiteral("Compute Skin Weights"));
}

TEST(ComputeSkinWeightsCommandTest, CtorWithCustomOptionsStillSetsText) {
    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = 3;
    opts.falloff = 2.5;
    opts.maxInfluenceDistance = 0.25;
    opts.skipUnweightedBones = true;
    opts.replaceExisting = false;

    ComputeSkinWeightsCommand cmd(kBogusEntity, opts);
    EXPECT_EQ(cmd.text(), QStringLiteral("Compute Skin Weights"));
}

TEST(ComputeSkinWeightsCommandTest, CtorAcceptsEmptyEntityName) {
    // An empty name is still a valid (just unresolvable) target.
    ComputeSkinWeightsCommand cmd(std::string(), defaultOpts());
    EXPECT_EQ(cmd.text(), QStringLiteral("Compute Skin Weights"));
}

// ---------------------------------------------------------------------------
// Initial report() / applied() state (before any redo)
// ---------------------------------------------------------------------------

TEST(ComputeSkinWeightsCommandTest, ReportInitiallyNotApplied) {
    ComputeSkinWeightsCommand cmd(kBogusEntity, defaultOpts());
    EXPECT_FALSE(cmd.report().applied);
    EXPECT_FALSE(cmd.applied());
}

TEST(ComputeSkinWeightsCommandTest, ReportInitiallyHasEmptyError) {
    ComputeSkinWeightsCommand cmd(kBogusEntity, defaultOpts());
    // Default-constructed SkinWeightsReport: error is an empty QString
    // and the numeric counters are zero.
    EXPECT_TRUE(cmd.report().error.isEmpty());
    EXPECT_EQ(cmd.report().totalBones, 0);
    EXPECT_EQ(cmd.report().totalVerticesProcessed, 0);
    EXPECT_EQ(cmd.report().totalAssignmentsBefore, 0);
    EXPECT_EQ(cmd.report().totalAssignmentsAfter, 0);
    EXPECT_TRUE(cmd.report().submeshes.isEmpty());
}

TEST(ComputeSkinWeightsCommandTest, AppliedMirrorsReportAppliedFlag) {
    ComputeSkinWeightsCommand cmd(kBogusEntity, defaultOpts());
    // applied() is defined as `return mReport.applied;`
    EXPECT_EQ(cmd.applied(), cmd.report().applied);
}

// ---------------------------------------------------------------------------
// redo() against an unresolvable entity → error branch
// ---------------------------------------------------------------------------

TEST(ComputeSkinWeightsCommandTest, RedoOnBogusEntityWithNoManagerSingleton) {
    // Force the Manager::getSingletonPtr() == nullptr leg of
    // resolveEntity(). kill() is a no-op if no singleton exists.
    Manager::kill();
    ASSERT_EQ(Manager::getSingletonPtr(), nullptr);

    ComputeSkinWeightsCommand cmd(kBogusEntity, defaultOpts());
    cmd.redo();

    EXPECT_FALSE(cmd.applied());
    EXPECT_EQ(cmd.report().error,
              QStringLiteral("entity not found / no mesh"));
}

// ---------------------------------------------------------------------------
// undo() before any successful redo → strict no-op (!mCaptured guard)
// ---------------------------------------------------------------------------

TEST(ComputeSkinWeightsCommandTest, UndoAfterFailedRedoIsNoOp) {
    // A failed redo() (unresolvable entity) returns before
    // mCaptured is set, so a following undo() must still be a strict
    // no-op and the error report must survive unchanged.
    ComputeSkinWeightsCommand cmd(kBogusEntity, defaultOpts());
    cmd.redo();
    ASSERT_FALSE(cmd.applied());

    EXPECT_NO_THROW(cmd.undo());

    EXPECT_FALSE(cmd.applied());
    EXPECT_EQ(cmd.report().error,
              QStringLiteral("entity not found / no mesh"));
}

// ---------------------------------------------------------------------------
// report() reference identity / accessor stability across calls
// ---------------------------------------------------------------------------

TEST(ComputeSkinWeightsCommandTest, ReportAccessorReturnsPostRedoReport) {
    ComputeSkinWeightsCommand cmd(kBogusEntity, defaultOpts());

    // Before redo.
    EXPECT_TRUE(cmd.report().error.isEmpty());

    cmd.redo();

    // After redo, report()/applied() expose the SkinWeightsReport the
    // redo() wrote (the error branch in this case).
    const SkinWeightsReport& r = cmd.report();
    EXPECT_FALSE(r.applied);
    EXPECT_EQ(r.error, QStringLiteral("entity not found / no mesh"));
    EXPECT_EQ(cmd.applied(), r.applied);
}

TEST(ComputeSkinWeightsCommandTest, ReportReturnsSameUnderlyingObject) {
    ComputeSkinWeightsCommand cmd(kBogusEntity, defaultOpts());
    // report() returns a const reference to the member, so its
    // address is stable across calls (and across redo()).
    const SkinWeightsReport* before = &cmd.report();
    cmd.redo();
    const SkinWeightsReport* after = &cmd.report();
    EXPECT_EQ(before, after);
}
