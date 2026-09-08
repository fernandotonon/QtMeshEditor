/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — `qtmesh paint-bake` argument-gate coverage (#552).

Covers the branches that must fail BEFORE any Ogre init or file write, so they
are exercisable headlessly: missing arguments, an unknown --target, and a
negative --resolution. The successful bake needs a render system and a textured
asset, so it is verified end-to-end by hand rather than here (see the PR).

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include <gtest/gtest.h>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QDir>
#include <QTemporaryDir>
#include <initializer_list>

#include "CLIPipeline.h"

namespace {

/// argc/argv from a list of C-strings, mirroring
/// CLIPipeline_cmdbakevc_coverage_test's helper.
class PaintBakeArgv {
public:
    PaintBakeArgv(std::initializer_list<const char*> args)
    {
        for (auto* a : args) m_storage.push_back(QByteArray(a));
        for (auto& ba : m_storage) m_argv.push_back(ba.data());
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() const { return m_argc; }
    char** argv() { return m_argv.data(); }

private:
    QList<QByteArray> m_storage;
    QList<char*> m_argv;
    int m_argc = 0;
};

} // namespace

TEST(CLIPipeline_cmdPaintBakeCoverageTest, NoArgsReturns2)
{
    PaintBakeArgv args({"qtmesh", "paint-bake"});
    EXPECT_EQ(CLIPipeline::cmdPaintBake(args.argc(), args.argv()), 2);
}

TEST(CLIPipeline_cmdPaintBakeCoverageTest, MissingOutputDirReturns2)
{
    PaintBakeArgv args({"qtmesh", "paint-bake", "model.fbx"});
    EXPECT_EQ(CLIPipeline::cmdPaintBake(args.argc(), args.argv()), 2);
}

TEST(CLIPipeline_cmdPaintBakeCoverageTest, MissingInputFileReturns2)
{
    PaintBakeArgv args({"qtmesh", "paint-bake", "-o", "/tmp/x"});
    EXPECT_EQ(CLIPipeline::cmdPaintBake(args.argc(), args.argv()), 2);
}

// An unknown target must be rejected up front rather than silently baking a
// Generic set, which would look like the requested pack failed to apply.
TEST(CLIPipeline_cmdPaintBakeCoverageTest, UnknownTargetReturns1)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PaintBakeArgv args({"qtmesh", "paint-bake", "model.fbx", "-o",
                        dir.path().toUtf8().constData(),
                        "--target", "unrealengine5"});
    EXPECT_EQ(CLIPipeline::cmdPaintBake(args.argc(), args.argv()), 1);
}

TEST(CLIPipeline_cmdPaintBakeCoverageTest, NegativeResolutionReturns1)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PaintBakeArgv args({"qtmesh", "paint-bake", "model.fbx", "-o",
                        dir.path().toUtf8().constData(),
                        "--resolution", "-512"});
    EXPECT_EQ(CLIPipeline::cmdPaintBake(args.argc(), args.argv()), 1);
}

// --list-targets is a pure query: it must succeed without a file, an output
// directory, or a render system.
TEST(CLIPipeline_cmdPaintBakeCoverageTest, ListTargetsSucceedsWithNoOtherArgs)
{
    PaintBakeArgv args({"qtmesh", "paint-bake", "--list-targets"});
    EXPECT_EQ(CLIPipeline::cmdPaintBake(args.argc(), args.argv()), 0);
}

// A nonexistent input must be reported without initialising Ogre.
TEST(CLIPipeline_cmdPaintBakeCoverageTest, NonexistentInputReturns1)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PaintBakeArgv args({"qtmesh", "paint-bake",
                        "/nonexistent/definitely_not_here.fbx",
                        "-o", dir.path().toUtf8().constData(),
                        "--target", "generic"});
    EXPECT_EQ(CLIPipeline::cmdPaintBake(args.argc(), args.argv()), 1);
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files).isEmpty())
        << "a failed bake must not leave files behind";
}

// --- qtmesh paint (Slice J, #553) -----------------------------------------

namespace {
/// argc/argv for `paint`, same helper shape as the paint-bake cases above.
class PaintArgv {
public:
    PaintArgv(std::initializer_list<const char*> args)
    {
        for (auto* a : args) m_storage.push_back(QByteArray(a));
        for (auto& ba : m_storage) m_argv.push_back(ba.data());
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() const { return m_argc; }
    char** argv() { return m_argv.data(); }
private:
    QList<QByteArray> m_storage;
    QList<char*> m_argv;
    int m_argc = 0;
};
} // namespace

TEST(CLIPipeline_cmdPaintCoverageTest, NoArgsShowsUsage)
{
    PaintArgv args({"qtmesh", "paint"});
    EXPECT_EQ(CLIPipeline::cmdPaint(args.argc(), args.argv()), 2);
}

// The three list queries are pure data: no mesh, no render system, no session.
TEST(CLIPipeline_cmdPaintCoverageTest, ListQueriesSucceedWithoutAMesh)
{
    for (const char* flag : {"--list-stamps", "--list-presets", "--list-palettes"}) {
        PaintArgv args({"qtmesh", "paint", flag});
        EXPECT_EQ(CLIPipeline::cmdPaint(args.argc(), args.argv()), 0) << flag;
    }
}

TEST(CLIPipeline_cmdPaintCoverageTest, ListQueriesSucceedWithJson)
{
    PaintArgv args({"qtmesh", "paint", "--list-presets", "--json"});
    EXPECT_EQ(CLIPipeline::cmdPaint(args.argc(), args.argv()), 0);
}

// Layer MUTATION is deliberately unsupported headlessly (layers are a live
// session and are never persisted), so it must be REFUSED rather than silently
// doing nothing to the output file.
TEST(CLIPipeline_cmdPaintCoverageTest, LayerMutationVerbsAreRefused)
{
    for (const char* verb : {"add", "merge-down", "flatten"}) {
        PaintArgv args({"qtmesh", "paint", "model.fbx", "--layer", verb});
        EXPECT_EQ(CLIPipeline::cmdPaint(args.argc(), args.argv()), 2) << verb;
    }
}

TEST(CLIPipeline_cmdPaintCoverageTest, LayerListNeedsAnInputFile)
{
    PaintArgv args({"qtmesh", "paint", "--layer", "list"});
    EXPECT_EQ(CLIPipeline::cmdPaint(args.argc(), args.argv()), 2);
}

TEST(CLIPipeline_cmdPaintCoverageTest, BakeNeedsAnOutputDirectory)
{
    PaintArgv args({"qtmesh", "paint", "model.fbx", "--bake"});
    EXPECT_EQ(CLIPipeline::cmdPaint(args.argc(), args.argv()), 2);
}

TEST(CLIPipeline_cmdPaintCoverageTest, ApplyStencilNeedsAnOutput)
{
    PaintArgv args({"qtmesh", "paint", "model.fbx", "--apply-stencil", "s.png"});
    EXPECT_EQ(CLIPipeline::cmdPaint(args.argc(), args.argv()), 2);
}

// --camera is required and must be exactly six numbers: there is no viewport
// camera to fall back on headlessly.
TEST(CLIPipeline_cmdPaintCoverageTest, ApplyStencilRejectsAMalformedCamera)
{
    PaintArgv three({"qtmesh", "paint", "model.fbx", "--apply-stencil", "s.png",
                     "--camera", "0,1,4", "-o", "out.mesh"});
    EXPECT_EQ(CLIPipeline::cmdPaint(three.argc(), three.argv()), 2);

    PaintArgv nonNumeric({"qtmesh", "paint", "model.fbx", "--apply-stencil", "s.png",
                          "--camera", "0,1,4,x,1,0", "-o", "out.mesh"});
    EXPECT_EQ(CLIPipeline::cmdPaint(nonNumeric.argc(), nonNumeric.argv()), 2);

    PaintArgv missing({"qtmesh", "paint", "model.fbx", "--apply-stencil", "s.png",
                       "-o", "out.mesh"});
    EXPECT_EQ(CLIPipeline::cmdPaint(missing.argc(), missing.argv()), 2);
}

// Reported in review on PR #993: PaintChannelNS::fromId returns Channel::Count
// for a typo and setActiveChannel silently IGNORES an out-of-range value, so an
// unvalidated --channel would project into whatever channel was active and
// export a valid-looking but wrong asset. Must be rejected before any work.
TEST(CLIPipeline_cmdPaintCoverageTest, ApplyStencilRejectsAnUnknownChannel)
{
    PaintArgv args({"qtmesh", "paint", "model.fbx", "--apply-stencil", "s.png",
                    "--camera", "0,1,4,0,1,0", "--channel", "speculr",
                    "-o", "out.mesh"});
    EXPECT_EQ(CLIPipeline::cmdPaint(args.argc(), args.argv()), 2);
}

// 'height' is a REAL id that setActiveChannel redirects to Normal, so it would
// silently write a different channel than requested (#547).
TEST(CLIPipeline_cmdPaintCoverageTest, ApplyStencilRejectsHeight)
{
    PaintArgv args({"qtmesh", "paint", "model.fbx", "--apply-stencil", "s.png",
                    "--camera", "0,1,4,0,1,0", "--channel", "height",
                    "-o", "out.mesh"});
    EXPECT_EQ(CLIPipeline::cmdPaint(args.argc(), args.argv()), 2);
}
