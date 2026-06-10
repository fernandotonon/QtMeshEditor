#include "AppLaunchHandler.h"
#include "Manager.h"

#include <QFile>
#include <QTemporaryDir>
#include <gtest/gtest.h>

namespace {

char arg0[] = "QtMeshEditor";
char argInfo[] = "info";
char argModel[] = "hero.fbx";
char argHelp[] = "--help";

TEST(AppLaunchHandlerTest, IsCliInvocation_Subcommand)
{
    char* argv[] = {arg0, argInfo, argModel, nullptr};
    EXPECT_TRUE(AppLaunchHandler::isCliInvocation(3, argv));
}

TEST(AppLaunchHandlerTest, IsCliInvocation_HelpFlag)
{
    char* argv[] = {arg0, argHelp, nullptr};
    EXPECT_TRUE(AppLaunchHandler::isCliInvocation(2, argv));
}

TEST(AppLaunchHandlerTest, IsCliInvocation_GuiModelPath)
{
    char* argv[] = {arg0, argModel, nullptr};
    EXPECT_FALSE(AppLaunchHandler::isCliInvocation(2, argv));
}

TEST(AppLaunchHandlerTest, IsImportableMeshPath_KnownExtensions)
{
    EXPECT_TRUE(AppLaunchHandler::isImportableMeshPath(QStringLiteral("/tmp/hero.fbx")));
    EXPECT_TRUE(AppLaunchHandler::isImportableMeshPath(QStringLiteral("/tmp/level.mesh")));
    EXPECT_TRUE(AppLaunchHandler::isImportableMeshPath(QStringLiteral("/tmp/scene.scene.glb")));
    EXPECT_FALSE(AppLaunchHandler::isImportableMeshPath(QStringLiteral("/tmp/readme.pdf")));
}

TEST(AppLaunchHandlerTest, CollectGuiLaunchPaths_SkipsFlagsAndSubcommands)
{
    const QStringList args = {
        QStringLiteral("QtMeshEditor"),
        QStringLiteral("--with-mcp"),
        QStringLiteral("scan"),
        QStringLiteral("./assets"),
    };
    EXPECT_TRUE(AppLaunchHandler::collectGuiLaunchPaths(args).isEmpty());
}

TEST(AppLaunchHandlerTest, CollectGuiLaunchPaths_ReadsExistingMeshFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString meshPath = dir.filePath(QStringLiteral("cube.obj"));
    QFile obj(meshPath);
    if (!obj.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        GTEST_SKIP() << "Could not create temp OBJ";
    }
    obj.write("o cube\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    obj.close();

    const QStringList args = {
        QStringLiteral("QtMeshEditor"),
        QStringLiteral("--verbose"),
        meshPath,
    };
    const QStringList paths = AppLaunchHandler::collectGuiLaunchPaths(args);
    ASSERT_EQ(paths.size(), 1);
    EXPECT_EQ(paths.front(), QFileInfo(meshPath).absoluteFilePath());
}

TEST(AppLaunchHandlerTest, DefaultImportExtensions_AlignsWithManager)
{
    EXPECT_FALSE(Manager::defaultImportExtensions().isEmpty());
    EXPECT_TRUE(AppLaunchHandler::isImportableMeshPath(QStringLiteral("x.glb")));
}

} // namespace
