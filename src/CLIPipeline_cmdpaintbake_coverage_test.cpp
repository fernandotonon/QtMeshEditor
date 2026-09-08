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
