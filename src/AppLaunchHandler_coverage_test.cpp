#include "AppLaunchHandler.h"
#include "Manager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>
#include <gtest/gtest.h>

// Coverage-focused suite for AppLaunchHandler's pure static helpers.
// Distinct filename + distinct suite name (AppLaunchHandlerCoverageTest) from
// the existing AppLaunchHandler_test.cpp so there is no duplicate-registration
// or ODR clash. No QApplication is created here (test_main.cpp owns it) and the
// targeted helpers are static, so no instance is needed.

namespace {

// ---------------------------------------------------------------------------
// isCliInvocation: qtmesh binary-name fast-path
// ---------------------------------------------------------------------------

TEST(AppLaunchHandlerCoverageTest, IsCli_QtmeshBinaryName_FastPathTrue)
{
    char a0[] = "qtmesh";
    char a1[] = "hero.fbx"; // a plain path; fast-path should win before this matters
    char* argv[] = {a0, a1, nullptr};
    EXPECT_TRUE(AppLaunchHandler::isCliInvocation(2, argv));
}

TEST(AppLaunchHandlerCoverageTest, IsCli_QtmeshCliBinaryName_FastPathTrue)
{
    char a0[] = "qtmesh-cli";
    char* argv[] = {a0, nullptr};
    EXPECT_TRUE(AppLaunchHandler::isCliInvocation(1, argv));
}

TEST(AppLaunchHandlerCoverageTest, IsCli_QtmeshBinaryName_WithFullPathPrefix)
{
    // fileName() should strip the directory, leaving "qtmesh".
    char a0[] = "/usr/local/bin/qtmesh";
    char* argv[] = {a0, nullptr};
    EXPECT_TRUE(AppLaunchHandler::isCliInvocation(1, argv));
}

TEST(AppLaunchHandlerCoverageTest, IsCli_QtmeshUppercase_CaseInsensitiveFastPath)
{
    // execName is lowercased before the startsWith check.
    char a0[] = "QTMESH";
    char* argv[] = {a0, nullptr};
    EXPECT_TRUE(AppLaunchHandler::isCliInvocation(1, argv));
}

// ---------------------------------------------------------------------------
// isCliInvocation: qtmesheditor binary-name exclusion (contains "editor")
// ---------------------------------------------------------------------------

TEST(AppLaunchHandlerCoverageTest, IsCli_QtmeshEditorBinaryName_NotCli)
{
    // starts with "qtmesh" but contains "editor" => fast-path excluded.
    // No flags / subcommands either, so overall false.
    char a0[] = "QtMeshEditor";
    char a1[] = "model.fbx";
    char* argv[] = {a0, a1, nullptr};
    EXPECT_FALSE(AppLaunchHandler::isCliInvocation(2, argv));
}

TEST(AppLaunchHandlerCoverageTest, IsCli_QtmesheditorLowercase_NotCli)
{
    char a0[] = "qtmesheditor";
    char* argv[] = {a0, nullptr};
    EXPECT_FALSE(AppLaunchHandler::isCliInvocation(1, argv));
}

TEST(AppLaunchHandlerCoverageTest, IsCli_EditorBinaryButHasCliFlag_StillCli)
{
    // Fast-path excluded by "editor", but the --cli flag branch fires.
    char a0[] = "QtMeshEditor";
    char a1[] = "--cli";
    char* argv[] = {a0, a1, nullptr};
    EXPECT_TRUE(AppLaunchHandler::isCliInvocation(2, argv));
}

// ---------------------------------------------------------------------------
// isCliInvocation: --cli / --version / -v / --help / -h flag branches
// ---------------------------------------------------------------------------

TEST(AppLaunchHandlerCoverageTest, IsCli_CliFlag)
{
    char a0[] = "QtMeshEditor";
    char a1[] = "--cli";
    char* argv[] = {a0, a1, nullptr};
    EXPECT_TRUE(AppLaunchHandler::isCliInvocation(2, argv));
}

TEST(AppLaunchHandlerCoverageTest, IsCli_VersionLongFlag)
{
    char a0[] = "QtMeshEditor";
    char a1[] = "--version";
    char* argv[] = {a0, a1, nullptr};
    EXPECT_TRUE(AppLaunchHandler::isCliInvocation(2, argv));
}

TEST(AppLaunchHandlerCoverageTest, IsCli_VersionShortFlag)
{
    char a0[] = "QtMeshEditor";
    char a1[] = "-v";
    char* argv[] = {a0, a1, nullptr};
    EXPECT_TRUE(AppLaunchHandler::isCliInvocation(2, argv));
}

TEST(AppLaunchHandlerCoverageTest, IsCli_HelpShortFlag)
{
    char a0[] = "QtMeshEditor";
    char a1[] = "-h";
    char* argv[] = {a0, a1, nullptr};
    EXPECT_TRUE(AppLaunchHandler::isCliInvocation(2, argv));
}

TEST(AppLaunchHandlerCoverageTest, IsCli_FlagAfterUnrelatedFlag)
{
    // The flag-scan loop walks every arg; --version is at index 2.
    char a0[] = "QtMeshEditor";
    char a1[] = "--verbose";
    char a2[] = "--version";
    char* argv[] = {a0, a1, a2, nullptr};
    EXPECT_TRUE(AppLaunchHandler::isCliInvocation(3, argv));
}

// ---------------------------------------------------------------------------
// isCliInvocation: first positional non-subcommand triggers break -> false
// ---------------------------------------------------------------------------

TEST(AppLaunchHandlerCoverageTest, IsCli_FirstPositionalNotSubcommand_Breaks_False)
{
    // No fast-path, no recognized flags, first positional ("model.fbx") is not a
    // subcommand => the subcommand loop hits `break` and returns false.
    char a0[] = "QtMeshEditor";
    char a1[] = "model.fbx";
    char a2[] = "info"; // a real subcommand, but it is AFTER the break, so unseen.
    char* argv[] = {a0, a1, a2, nullptr};
    EXPECT_FALSE(AppLaunchHandler::isCliInvocation(3, argv));
}

TEST(AppLaunchHandlerCoverageTest, IsCli_LeadingFlagThenPositionalNonSubcommand_False)
{
    // Leading "-" arg is skipped (continue), then non-subcommand positional breaks.
    char a0[] = "QtMeshEditor";
    char a1[] = "--verbose";
    char a2[] = "model.fbx";
    char* argv[] = {a0, a1, a2, nullptr};
    EXPECT_FALSE(AppLaunchHandler::isCliInvocation(3, argv));
}

TEST(AppLaunchHandlerCoverageTest, IsCli_LeadingFlagThenSubcommand_True)
{
    // Leading flag skipped, then a recognized subcommand => true.
    char a0[] = "QtMeshEditor";
    char a1[] = "--verbose";
    char a2[] = "convert";
    char* argv[] = {a0, a1, a2, nullptr};
    EXPECT_TRUE(AppLaunchHandler::isCliInvocation(3, argv));
}

TEST(AppLaunchHandlerCoverageTest, IsCli_NoArgs_False)
{
    char a0[] = "QtMeshEditor";
    char* argv[] = {a0, nullptr};
    EXPECT_FALSE(AppLaunchHandler::isCliInvocation(1, argv));
}

// ---------------------------------------------------------------------------
// isImportableMeshPath: double-extension + case-insensitive matching
// ---------------------------------------------------------------------------

TEST(AppLaunchHandlerCoverageTest, IsImportable_SceneGltfDoubleExtension)
{
    EXPECT_TRUE(AppLaunchHandler::isImportableMeshPath(QStringLiteral("/tmp/world.scene.gltf")));
}

TEST(AppLaunchHandlerCoverageTest, IsImportable_SceneGlbDoubleExtension)
{
    EXPECT_TRUE(AppLaunchHandler::isImportableMeshPath(QStringLiteral("/tmp/world.scene.glb")));
}

TEST(AppLaunchHandlerCoverageTest, IsImportable_SceneGltfUppercase)
{
    // path.toLower() is applied before the .scene.gltf comparison.
    EXPECT_TRUE(AppLaunchHandler::isImportableMeshPath(QStringLiteral("/tmp/WORLD.SCENE.GLTF")));
}

TEST(AppLaunchHandlerCoverageTest, IsImportable_KnownExtensionCaseInsensitive)
{
    // Manager extensions matched with Qt::CaseInsensitive on a lowercased path.
    EXPECT_TRUE(AppLaunchHandler::isImportableMeshPath(QStringLiteral("/tmp/HERO.FBX")));
    EXPECT_TRUE(AppLaunchHandler::isImportableMeshPath(QStringLiteral("/tmp/Level.Obj")));
}

TEST(AppLaunchHandlerCoverageTest, IsImportable_UnknownExtension_False)
{
    EXPECT_FALSE(AppLaunchHandler::isImportableMeshPath(QStringLiteral("/tmp/notes.txt")));
    EXPECT_FALSE(AppLaunchHandler::isImportableMeshPath(QStringLiteral("/tmp/image.png")));
}

TEST(AppLaunchHandlerCoverageTest, IsImportable_EmptyPath_False)
{
    EXPECT_FALSE(AppLaunchHandler::isImportableMeshPath(QString()));
}

TEST(AppLaunchHandlerCoverageTest, DefaultImportExtensions_NotEmpty)
{
    // Manager::defaultImportExtensions() is static and safe to call headlessly.
    EXPECT_FALSE(Manager::defaultImportExtensions().isEmpty());
}

// ---------------------------------------------------------------------------
// collectGuiLaunchPaths: --http-port two-arg skip
// ---------------------------------------------------------------------------

TEST(AppLaunchHandlerCoverageTest, Collect_HttpPortSkipsPortNumber)
{
    // "--http-port" is a GUI-mode flag; the numeric arg right after must be
    // consumed by the ++i advance and never treated as a path. The port number
    // also is not importable, but the ++i guarantees it's skipped entirely.
    const QStringList args = {
        QStringLiteral("QtMeshEditor"),
        QStringLiteral("--http-port"),
        QStringLiteral("8080"),
    };
    EXPECT_TRUE(AppLaunchHandler::collectGuiLaunchPaths(args).isEmpty());
}

TEST(AppLaunchHandlerCoverageTest, Collect_HttpPortAtEndNoFollowingArg)
{
    // i + 1 >= size guard: --http-port is the last token, so no ++i.
    const QStringList args = {
        QStringLiteral("QtMeshEditor"),
        QStringLiteral("--http-port"),
    };
    EXPECT_TRUE(AppLaunchHandler::collectGuiLaunchPaths(args).isEmpty());
}

TEST(AppLaunchHandlerCoverageTest, Collect_HttpPortThenRealFileStillCollected)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString meshPath = dir.filePath(QStringLiteral("cube.obj"));
    QFile obj(meshPath);
    ASSERT_TRUE(obj.open(QIODevice::WriteOnly | QIODevice::Truncate));
    obj.write("o cube\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    obj.close();

    const QStringList args = {
        QStringLiteral("QtMeshEditor"),
        QStringLiteral("--http-port"),
        QStringLiteral("9000"),
        meshPath,
    };
    const QStringList paths = AppLaunchHandler::collectGuiLaunchPaths(args);
    ASSERT_EQ(paths.size(), 1);
    EXPECT_EQ(paths.front(), QFileInfo(meshPath).absoluteFilePath());
}

// ---------------------------------------------------------------------------
// collectGuiLaunchPaths: .scene.gltf acceptance + multi-file ordering
// ---------------------------------------------------------------------------

TEST(AppLaunchHandlerCoverageTest, Collect_SceneGltfDoubleExtensionAccepted)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString scenePath = dir.filePath(QStringLiteral("level.scene.gltf"));
    QFile gltf(scenePath);
    ASSERT_TRUE(gltf.open(QIODevice::WriteOnly | QIODevice::Truncate));
    gltf.write("{}");
    gltf.close();

    const QStringList args = {
        QStringLiteral("QtMeshEditor"),
        scenePath,
    };
    const QStringList paths = AppLaunchHandler::collectGuiLaunchPaths(args);
    ASSERT_EQ(paths.size(), 1);
    EXPECT_EQ(paths.front(), QFileInfo(scenePath).absoluteFilePath());
}

TEST(AppLaunchHandlerCoverageTest, Collect_MultipleFilesPreserveOrder)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString firstPath = dir.filePath(QStringLiteral("first.obj"));
    QFile first(firstPath);
    ASSERT_TRUE(first.open(QIODevice::WriteOnly | QIODevice::Truncate));
    first.write("o first\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    first.close();

    const QString secondPath = dir.filePath(QStringLiteral("second.scene.gltf"));
    QFile second(secondPath);
    ASSERT_TRUE(second.open(QIODevice::WriteOnly | QIODevice::Truncate));
    second.write("{}");
    second.close();

    const QStringList args = {
        QStringLiteral("QtMeshEditor"),
        firstPath,
        secondPath,
    };
    const QStringList paths = AppLaunchHandler::collectGuiLaunchPaths(args);
    ASSERT_EQ(paths.size(), 2);
    EXPECT_EQ(paths.at(0), QFileInfo(firstPath).absoluteFilePath());
    EXPECT_EQ(paths.at(1), QFileInfo(secondPath).absoluteFilePath());
}

// ---------------------------------------------------------------------------
// collectGuiLaunchPaths: non-existent / unreadable-file skip gates
// ---------------------------------------------------------------------------

TEST(AppLaunchHandlerCoverageTest, Collect_NonExistentImportablePathSkipped)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // Importable extension but no file on disk => info.exists() false => skipped.
    const QString missing = dir.filePath(QStringLiteral("ghost.fbx"));

    const QStringList args = {
        QStringLiteral("QtMeshEditor"),
        missing,
    };
    EXPECT_TRUE(AppLaunchHandler::collectGuiLaunchPaths(args).isEmpty());
}

TEST(AppLaunchHandlerCoverageTest, Collect_DirectoryWithImportableSuffixSkipped)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // A directory named like a mesh: exists() true but isFile() false => skipped.
    const QString dirAsMesh = dir.filePath(QStringLiteral("bundle.obj"));
    ASSERT_TRUE(QDir(dir.path()).mkdir(QStringLiteral("bundle.obj")));

    const QStringList args = {
        QStringLiteral("QtMeshEditor"),
        dirAsMesh,
    };
    EXPECT_TRUE(AppLaunchHandler::collectGuiLaunchPaths(args).isEmpty());
}

TEST(AppLaunchHandlerCoverageTest, Collect_NonImportableExtensionSkipped)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString txtPath = dir.filePath(QStringLiteral("readme.txt"));
    QFile txt(txtPath);
    ASSERT_TRUE(txt.open(QIODevice::WriteOnly | QIODevice::Truncate));
    txt.write("hello");
    txt.close();

    const QStringList args = {
        QStringLiteral("QtMeshEditor"),
        txtPath,
    };
    EXPECT_TRUE(AppLaunchHandler::collectGuiLaunchPaths(args).isEmpty());
}

TEST(AppLaunchHandlerCoverageTest, Collect_SubcommandBreaksRemainingArgs)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString meshPath = dir.filePath(QStringLiteral("after.obj"));
    QFile obj(meshPath);
    ASSERT_TRUE(obj.open(QIODevice::WriteOnly | QIODevice::Truncate));
    obj.write("o c\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    obj.close();

    // "info" subcommand triggers break, so the real mesh after it is never seen.
    const QStringList args = {
        QStringLiteral("QtMeshEditor"),
        QStringLiteral("info"),
        meshPath,
    };
    EXPECT_TRUE(AppLaunchHandler::collectGuiLaunchPaths(args).isEmpty());
}

TEST(AppLaunchHandlerCoverageTest, Collect_LeadingDashArgSkipped)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString meshPath = dir.filePath(QStringLiteral("c.obj"));
    QFile obj(meshPath);
    ASSERT_TRUE(obj.open(QIODevice::WriteOnly | QIODevice::Truncate));
    obj.write("o c\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    obj.close();

    // A bare "-" / "--flag" arg hits the startsWith('-') continue.
    const QStringList args = {
        QStringLiteral("QtMeshEditor"),
        QStringLiteral("--verbose"),
        meshPath,
    };
    const QStringList paths = AppLaunchHandler::collectGuiLaunchPaths(args);
    ASSERT_EQ(paths.size(), 1);
    EXPECT_EQ(paths.front(), QFileInfo(meshPath).absoluteFilePath());
}

TEST(AppLaunchHandlerCoverageTest, Collect_EmptyAndProgramNameOnly)
{
    EXPECT_TRUE(AppLaunchHandler::collectGuiLaunchPaths(QStringList()).isEmpty());
    EXPECT_TRUE(AppLaunchHandler::collectGuiLaunchPaths(
        QStringList{QStringLiteral("QtMeshEditor")}).isEmpty());
}

} // namespace
