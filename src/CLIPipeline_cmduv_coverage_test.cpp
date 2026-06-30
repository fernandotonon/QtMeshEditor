#include <gtest/gtest.h>

#include <QByteArray>
#include <QList>
#include <QTemporaryDir>
#include <initializer_list>

#include "CLIPipeline.h"

// Coverage tests for CLIPipeline::cmdUv (issue #400 UV unwrap CLI).
//
// cmdUv parses its own argv and applies usage gates that all return
// BEFORE any Ogre initialisation, so these are safe to run headless:
//   1. no input file                  -> 2
//   2. no mode (--info|--unwrap|...)  -> 2
//   3. --unwrap|--project|--set-seams without -o -> 2
//   4. more than one mode flag        -> 2
// A fifth, file-not-found gate, also runs before Ogre init:
//   5. valid mode flags + missing file -> 1
//
// Distinct filename + distinct suite names (CLIPipelineCmdUvCoverage*) avoid
// any ODR / duplicate-registration clash with the existing CLIPipeline_test.cpp.

namespace {

// Local RAII argc/argv builder (kept private to this TU to avoid ODR clash
// with the identically named helper inside CLIPipeline_test.cpp).
class UvTestArgv {
public:
    UvTestArgv(std::initializer_list<const char*> args)
    {
        for (auto* a : args) {
            m_storage.push_back(QByteArray(a));
        }
        for (auto& ba : m_storage) {
            m_argv.push_back(ba.data());
        }
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

// --- Gate 1: no input file -> 2 ---

TEST(CLIPipelineCmdUvCoverageError, NoInputFile_BareSubcommand)
{
    UvTestArgv args({"qtmesh", "uv"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdUvCoverageError, NoInputFile_FlagsButNoPositional)
{
    // --info supplied but no positional file argument: the missing-input
    // gate is checked first and wins, so this is still 2.
    UvTestArgv args({"qtmesh", "uv", "--info"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdUvCoverageError, NoInputFile_OnlyOutputFlag)
{
    // -o consumes its value; nothing is left as a positional input.
    UvTestArgv args({"qtmesh", "uv", "--unwrap", "-o", "out.glb"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 2);
}

// --- Gate 2: no mode flag -> 2 ---

TEST(CLIPipelineCmdUvCoverageError, NoModeSpecified)
{
    UvTestArgv args({"qtmesh", "uv", "model.fbx"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdUvCoverageError, NoModeSpecified_WithJsonFlagOnly)
{
    // --json alone does not select a mode; still must require a mode flag.
    UvTestArgv args({"qtmesh", "uv", "model.fbx", "--json"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdUvCoverageError, NoModeSpecified_WithResolutionOnly)
{
    // Numeric options are parsed but do not imply a mode.
    UvTestArgv args({"qtmesh", "uv", "model.fbx", "--resolution", "2048"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdUvCoverageError, MultipleModesSpecified)
{
    UvTestArgv args({"qtmesh", "uv", "model.fbx", "--info", "--project", "box", "-o", "out.glb"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 2);
}

// --- Gate 3: write modes without -o output -> 2 ---

TEST(CLIPipelineCmdUvCoverageError, UnwrapWithoutOutput)
{
    UvTestArgv args({"qtmesh", "uv", "model.fbx", "--unwrap"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdUvCoverageError, UnwrapWithoutOutput_WithExtraOptions)
{
    // Resolution/padding/channel are accepted but do not satisfy the -o gate.
    UvTestArgv args({"qtmesh", "uv", "model.fbx", "--unwrap",
                     "--resolution", "1024", "--padding", "8", "--channel", "1"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdUvCoverageError, ProjectWithoutOutput)
{
    UvTestArgv args({"qtmesh", "uv", "model.fbx", "--project", "box"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 2);
}

TEST(CLIPipelineCmdUvCoverageError, SetSeamsWithoutOutput)
{
    UvTestArgv args({"qtmesh", "uv", "model.fbx", "--set-seams", "0:1-2"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 2);
}

// --- Gate 4: valid mode flags + nonexistent file -> 1 ---
// These run after the usage gates but still before Ogre init (file existence
// is checked via QFileInfo before initOgreHeadless()), so they are headless-safe.

TEST(CLIPipelineCmdUvCoverageError, InfoMode_FileNotFound)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray missing =
        dir.filePath("definitely_missing_uv_info.fbx").toLocal8Bit();

    UvTestArgv args({"qtmesh", "uv", missing.constData(), "--info"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdUvCoverageError, InfoModeJson_FileNotFound)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray missing =
        dir.filePath("definitely_missing_uv_info_json.fbx").toLocal8Bit();

    UvTestArgv args({"qtmesh", "uv", missing.constData(), "--info", "--json"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdUvCoverageError, UnwrapMode_FileNotFound)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray missing =
        dir.filePath("definitely_missing_uv_unwrap.fbx").toLocal8Bit();
    const QByteArray out = dir.filePath("out.glb").toLocal8Bit();

    UvTestArgv args({"qtmesh", "uv", missing.constData(), "--unwrap",
                     "-o", out.constData()});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdUvCoverageError, UnwrapMode_FileNotFound_LongOutputFlag)
{
    // Exercise the --output alias for -o on the file-not-found path.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray missing =
        dir.filePath("missing_uv_unwrap_longopt.fbx").toLocal8Bit();
    const QByteArray out = dir.filePath("out_long.glb").toLocal8Bit();

    UvTestArgv args({"qtmesh", "uv", missing.constData(), "--unwrap",
                     "--output", out.constData(), "--no-backup"});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdUvCoverageError, ProjectMode_FileNotFound)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray missing =
        dir.filePath("missing_uv_project.fbx").toLocal8Bit();
    const QByteArray out = dir.filePath("out.glb").toLocal8Bit();

    UvTestArgv args({"qtmesh", "uv", missing.constData(), "--project", "box",
                     "-o", out.constData()});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 1);
}

TEST(CLIPipelineCmdUvCoverageError, SetSeamsMode_FileNotFound)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QByteArray missing =
        dir.filePath("missing_uv_seams.fbx").toLocal8Bit();
    const QByteArray out = dir.filePath("out.glb").toLocal8Bit();

    UvTestArgv args({"qtmesh", "uv", missing.constData(), "--set-seams", "0:1-2",
                     "-o", out.constData()});
    EXPECT_EQ(CLIPipeline::cmdUv(args.argc(), args.argv()), 1);
}
