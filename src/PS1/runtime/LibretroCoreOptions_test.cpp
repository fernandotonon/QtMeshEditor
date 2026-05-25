#ifdef ENABLE_PS1_RIP

#include "LibretroCoreOptions.h"

#include <gtest/gtest.h>

namespace {

// RAII helper that snapshots QTMESH_PS1_LIBRETRO_RENDERER for the lifetime of a
// test and restores the original value (or unsets the variable) on destruction.
// Prevents tests from leaking process-global env state to neighbouring tests.
class RendererEnvGuard
{
public:
    RendererEnvGuard()
        : m_hadValue(qEnvironmentVariableIsSet("QTMESH_PS1_LIBRETRO_RENDERER")),
          m_previous(qgetenv("QTMESH_PS1_LIBRETRO_RENDERER"))
    {
    }

    ~RendererEnvGuard()
    {
        if (m_hadValue)
            qputenv("QTMESH_PS1_LIBRETRO_RENDERER", m_previous);
        else
            qunsetenv("QTMESH_PS1_LIBRETRO_RENDERER");
    }

private:
    bool m_hadValue;
    QByteArray m_previous;
};

} // namespace

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
    RendererEnvGuard guard;
    qunsetenv("QTMESH_PS1_LIBRETRO_RENDERER");
    EXPECT_EQ(LibretroCoreOptions::rendererPreferenceFromEnv(), QByteArray("software"));
    EXPECT_STREQ(LibretroCoreOptions::valueForKey("beetle_psx_renderer",
                                                 LibretroCoreOptions::rendererPreferenceFromEnv()),
                "software");
}

TEST(LibretroCoreOptionsTest, AutoRendererDoesNotOverride)
{
    RendererEnvGuard guard;
    qputenv("QTMESH_PS1_LIBRETRO_RENDERER", "auto");
    EXPECT_TRUE(LibretroCoreOptions::rendererPreferenceFromEnv().isEmpty());
    EXPECT_EQ(LibretroCoreOptions::valueForKey("beetle_psx_renderer",
                                               LibretroCoreOptions::rendererPreferenceFromEnv()),
              nullptr);
}

TEST(LibretroCoreOptionsTest, SkipBiosVariablesAlwaysProvided)
{
    EXPECT_STREQ(LibretroCoreOptions::valueForKey("beetle_psx_skip_bios", QByteArray("software")),
                "enabled");
    EXPECT_STREQ(
        LibretroCoreOptions::valueForKey("beetle_psx_override_bios", QByteArray("software")),
        "disabled");
}

TEST(LibretroCoreOptionsTest, MergePreservesUnrelatedKeysAndComments)
{
    QStringList existing;
    existing << QStringLiteral("# user config")
             << QStringLiteral("beetle_psx_renderer = \"hardware_gl\"") // gets replaced
             << QStringLiteral("beetle_psx_widescreen_hack = \"enabled\"") // preserved
             << QString()                                                   // blank line
             << QStringLiteral("beetle_psx_skip_bios = \"disabled\""); // gets replaced

    const QList<QPair<QByteArray, QByteArray>> managed = {
        {QByteArrayLiteral("beetle_psx_renderer"), QByteArrayLiteral("software")},
        {QByteArrayLiteral("beetle_psx_skip_bios"), QByteArrayLiteral("enabled")},
        {QByteArrayLiteral("beetle_psx_override_bios"), QByteArrayLiteral("disabled")},
    };

    const QStringList merged = LibretroCoreOptions::mergeCoreConfigLines(existing, managed);

    EXPECT_TRUE(merged.contains(QStringLiteral("# user config")));
    EXPECT_TRUE(merged.contains(QStringLiteral("beetle_psx_widescreen_hack = \"enabled\"")));
    EXPECT_TRUE(merged.contains(QString()));

    EXPECT_FALSE(merged.contains(QStringLiteral("beetle_psx_renderer = \"hardware_gl\"")));
    EXPECT_FALSE(merged.contains(QStringLiteral("beetle_psx_skip_bios = \"disabled\"")));

    EXPECT_TRUE(merged.contains(QStringLiteral("beetle_psx_renderer = \"software\"")));
    EXPECT_TRUE(merged.contains(QStringLiteral("beetle_psx_skip_bios = \"enabled\"")));
    EXPECT_TRUE(merged.contains(QStringLiteral("beetle_psx_override_bios = \"disabled\"")));
}

TEST(LibretroCoreOptionsTest, MergeOnEmptyInputProducesManagedKeysOnly)
{
    const QList<QPair<QByteArray, QByteArray>> managed = {
        {QByteArrayLiteral("beetle_psx_renderer"), QByteArrayLiteral("software")},
    };

    const QStringList merged = LibretroCoreOptions::mergeCoreConfigLines({}, managed);

    ASSERT_EQ(merged.size(), 1);
    EXPECT_EQ(merged.first(), QStringLiteral("beetle_psx_renderer = \"software\""));
}

#endif // ENABLE_PS1_RIP
