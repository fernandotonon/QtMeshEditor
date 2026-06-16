#include <gtest/gtest.h>

#include "InstallFlavor.h"

#include <QString>

using InstallFlavor::Flavor;

TEST(InstallFlavor, SlugAndDisplayNamesAreStable)
{
    EXPECT_EQ(InstallFlavor::toSlug(Flavor::Portable), QStringLiteral("portable"));
    EXPECT_EQ(InstallFlavor::displayName(Flavor::WinGet), QStringLiteral("WinGet"));
}

TEST(InstallFlavor, PackageManagerManagedExcludesPortableAndUnknown)
{
    EXPECT_FALSE(InstallFlavor::isPackageManagerManaged(Flavor::Portable));
    EXPECT_FALSE(InstallFlavor::isPackageManagerManaged(Flavor::Unknown));
    EXPECT_TRUE(InstallFlavor::isPackageManagerManaged(Flavor::Snap));
    EXPECT_TRUE(InstallFlavor::isPackageManagerManaged(Flavor::Homebrew));
}

TEST(InstallFlavor, UpdateHintsMatchDocumentedCommands)
{
    EXPECT_EQ(InstallFlavor::updateCommandHint(Flavor::Homebrew),
              QStringLiteral("brew upgrade --cask qtmesheditor"));
    EXPECT_EQ(InstallFlavor::updateCommandHint(Flavor::WinGet),
              QStringLiteral("winget upgrade FernandoTonon.QtMeshEditor"));
    EXPECT_EQ(InstallFlavor::updateCommandHint(Flavor::Snap),
              QStringLiteral("sudo snap refresh qtmesheditor"));
    EXPECT_TRUE(InstallFlavor::updateCommandHint(Flavor::Portable).isEmpty());
}

#if defined(Q_OS_LINUX)
TEST(InstallFlavor, LinuxDetectsSnapFlatpakDebianDockerPortable)
{
    EXPECT_EQ(InstallFlavor::detect(QStringLiteral("/snap/qtmesheditor/42/usr/bin/QtMeshEditor")),
              Flavor::Snap);
    EXPECT_EQ(InstallFlavor::detect(QStringLiteral("/var/lib/flatpak/app/io.github.app/x/active/files/bin/QtMeshEditor")),
              Flavor::Flatpak);
    EXPECT_EQ(InstallFlavor::detect(QStringLiteral("/usr/bin/QtMeshEditor")), Flavor::Debian);
    EXPECT_EQ(InstallFlavor::detect(QStringLiteral("/opt/QtMeshEditor/bin/QtMeshEditor")),
              Flavor::Portable);
}

TEST(InstallFlavor, LinuxCiBuildTreeIsUnknown)
{
    // GitHub Actions / local build trees are not package-manager installs.
    const Flavor flavor = InstallFlavor::detect(
        QStringLiteral("/home/runner/work/QtMeshEditor/build/bin/QtMeshEditor"));
    EXPECT_EQ(flavor, Flavor::Unknown);
}
#elif defined(Q_OS_MACOS)
TEST(InstallFlavor, MacDetectsHomebrewAndPortable)
{
    EXPECT_EQ(InstallFlavor::detect(QStringLiteral("/Applications/Homebrew/QtMeshEditor.app/Contents/MacOS/QtMeshEditor")),
              Flavor::Homebrew);
    EXPECT_EQ(InstallFlavor::detect(QStringLiteral("/Applications/QtMeshEditor.app/Contents/MacOS/QtMeshEditor")),
              Flavor::Portable);
}

TEST(InstallFlavor, MacCiBuildTreeIsUnknown)
{
    EXPECT_EQ(InstallFlavor::detect(QStringLiteral("/Users/runner/work/QtMeshEditor/build/bin/QtMeshEditor")),
              Flavor::Unknown);
}
#elif defined(Q_OS_WIN)
TEST(InstallFlavor, WindowsPortableZipLayout)
{
    EXPECT_EQ(InstallFlavor::detect(QStringLiteral("C:/Tools/QtMeshEditor/bin/QtMeshEditor.exe")),
              Flavor::Portable);
}

TEST(InstallFlavor, WindowsCiBuildTreeIsUnknown)
{
    EXPECT_EQ(InstallFlavor::detect(QStringLiteral("D:/a/QtMeshEditor/QtMeshEditor/build/bin/QtMeshEditor.exe")),
              Flavor::Unknown);
}
#endif
