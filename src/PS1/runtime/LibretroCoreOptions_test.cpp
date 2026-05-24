#ifdef ENABLE_PS1_RIP

#include "LibretroCoreOptions.h"

#include <gtest/gtest.h>

TEST(LibretroCoreOptionsTest, DetectsHardwareOnlyCorePaths)
{
    EXPECT_TRUE(LibretroCoreOptions::isHardwareOnlyCorePath(
        QStringLiteral("/PS1Cores/beetle_psx_hw_libretro.so")));
    EXPECT_TRUE(LibretroCoreOptions::isHardwareOnlyCorePath(
        QStringLiteral("beetle_psx_hw_libretro.dll")));
    EXPECT_FALSE(LibretroCoreOptions::isHardwareOnlyCorePath(
        QStringLiteral("/PS1Cores/mednafen_psx_libretro.so")));
}

TEST(LibretroCoreOptionsTest, SoftwareRendererIsDefaultOverride)
{
    qunsetenv("QTMESH_PS1_LIBRETRO_RENDERER");
    EXPECT_EQ(LibretroCoreOptions::rendererPreferenceFromEnv(), QByteArray("software"));
    EXPECT_STREQ(LibretroCoreOptions::valueForKey("beetle_psx_renderer",
                                                 LibretroCoreOptions::rendererPreferenceFromEnv()),
                "software");
}

TEST(LibretroCoreOptionsTest, AutoRendererDoesNotOverride)
{
    qputenv("QTMESH_PS1_LIBRETRO_RENDERER", "auto");
    EXPECT_TRUE(LibretroCoreOptions::rendererPreferenceFromEnv().isEmpty());
    EXPECT_EQ(LibretroCoreOptions::valueForKey("beetle_psx_renderer",
                                               LibretroCoreOptions::rendererPreferenceFromEnv()),
              nullptr);
    qunsetenv("QTMESH_PS1_LIBRETRO_RENDERER");
}

TEST(LibretroCoreOptionsTest, SkipBiosVariablesAlwaysProvided)
{
    EXPECT_STREQ(LibretroCoreOptions::valueForKey("beetle_psx_skip_bios", QByteArray("software")),
                "enabled");
    EXPECT_STREQ(
        LibretroCoreOptions::valueForKey("beetle_psx_override_bios", QByteArray("software")),
        "disabled");
}

#endif // ENABLE_PS1_RIP
