#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>

#include "UpdaterInstaller.h"

TEST(UpdaterInstaller, DetectArtifactKindByExtension)
{
    EXPECT_EQ(UpdaterInstaller::detectArtifactKind(QStringLiteral("QtMeshEditor-bin-Windows.zip")),
              UpdaterInstaller::ArtifactKind::Zip);
    EXPECT_EQ(UpdaterInstaller::detectArtifactKind(QStringLiteral("release.tar.gz")),
              UpdaterInstaller::ArtifactKind::TarGz);
    EXPECT_EQ(UpdaterInstaller::detectArtifactKind(QStringLiteral("release.tgz")),
              UpdaterInstaller::ArtifactKind::TarGz);
    EXPECT_EQ(UpdaterInstaller::detectArtifactKind(QStringLiteral("release.tar.xz")),
              UpdaterInstaller::ArtifactKind::TarXz);
    EXPECT_EQ(UpdaterInstaller::detectArtifactKind(QStringLiteral("QtMeshEditor-MacOS.dmg")),
              UpdaterInstaller::ArtifactKind::Dmg);
    EXPECT_EQ(UpdaterInstaller::detectArtifactKind(QStringLiteral("QtMeshEditor-x86_64.AppImage")),
              UpdaterInstaller::ArtifactKind::AppImage);
    EXPECT_EQ(UpdaterInstaller::detectArtifactKind(QStringLiteral("readme.txt")),
              UpdaterInstaller::ArtifactKind::Unknown);
}

TEST(UpdaterInstaller, ResolveInstallRootFromExecutablePath)
{
#if defined(Q_OS_MACOS)
    const QString root = UpdaterInstaller::resolveInstallRootForTest(
        QStringLiteral("/Applications/QtMeshEditor.app/Contents/MacOS/QtMeshEditor"));
    EXPECT_EQ(root, QStringLiteral("/Applications/QtMeshEditor.app"));
#elif defined(Q_OS_LINUX)
    const QString optRoot = UpdaterInstaller::resolveInstallRootForTest(
        QStringLiteral("/opt/QtMeshEditor/bin/QtMeshEditor"));
    EXPECT_EQ(optRoot, QStringLiteral("/opt/QtMeshEditor"));

    const QString localRoot = UpdaterInstaller::resolveInstallRootForTest(
        QStringLiteral("/home/user/QtMeshEditor/bin/QtMeshEditor"));
    EXPECT_EQ(localRoot, QStringLiteral("/home/user/QtMeshEditor"));
#elif defined(Q_OS_WIN)
    const QString root = UpdaterInstaller::resolveInstallRootForTest(
        QStringLiteral("C:/QtMeshEditor/bin/QtMeshEditor.exe"));
    EXPECT_EQ(root, QStringLiteral("C:/QtMeshEditor"));
#endif
}

TEST(UpdaterInstaller, PrepareInstallFailsWhenArtifactMissing)
{
    UpdaterInstaller::InstallContext context;
    context.stagedArtifactPath = QStringLiteral("/tmp/does-not-exist/QtMeshEditor.zip");
    context.releaseTag = QStringLiteral("9.9.9");
    context.executablePath = QCoreApplication::applicationFilePath();
    context.installRoot = UpdaterInstaller::resolveInstallRoot(context.executablePath);

    const UpdaterInstaller::InstallPlan plan = UpdaterInstaller::prepareInstall(context);
    EXPECT_FALSE(plan.ok);
    EXPECT_FALSE(plan.errorMessage.isEmpty());
}

TEST(UpdaterInstaller, IsInstallLocationWritableRejectsEmptyRoot)
{
    UpdaterInstaller::InstallContext context;
    context.installRoot.clear();
    EXPECT_FALSE(UpdaterInstaller::isInstallLocationWritable(context));
}

TEST(UpdaterInstaller, IsInstallLocationWritableUsesTempDir)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    UpdaterInstaller::InstallContext context;
    context.installRoot = tempDir.path();
    EXPECT_TRUE(UpdaterInstaller::isInstallLocationWritable(context));
}
