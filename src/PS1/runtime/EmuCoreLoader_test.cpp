#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>

#include "PS1/runtime/EmuCore.h"
#include "PS1/runtime/EmuCoreLoader.h"
#include "PS1/runtime/RipperHooks.h"
#include "PS1/runtime/VramSnapshot.h"

class EmuCoreLoaderTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_NE(QCoreApplication::instance(), nullptr);
        qputenv("QTMESH_PS1_FORCE_STUB", "1");
    }

    void TearDown() override
    {
        qunsetenv("QTMESH_PS1_FORCE_STUB");
    }
};

TEST_F(EmuCoreLoaderTest, SearchPathsIncludePs1CoresNextToBinary)
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList paths = EmuCoreLoader::coreSearchPaths();
    EXPECT_TRUE(paths.contains(QDir(appDir).filePath(QStringLiteral("PS1Cores"))));
}

TEST_F(EmuCoreLoaderTest, LoadStubCoreWhenPluginPresent)
{
    const QDir coresDir(QCoreApplication::applicationDirPath() + QStringLiteral("/PS1Cores"));
    const QStringList candidates = {
#if defined(Q_OS_WIN)
        coresDir.filePath(QStringLiteral("qtmesh_ps1core_stub.dll")),
#elif defined(Q_OS_MACOS)
        coresDir.filePath(QStringLiteral("libqtmesh_ps1core_stub.dylib")),
        coresDir.filePath(QStringLiteral("qtmesh_ps1core_stub.dylib")),
#else
        coresDir.filePath(QStringLiteral("libqtmesh_ps1core_stub.so")),
        coresDir.filePath(QStringLiteral("qtmesh_ps1core_stub.so")),
#endif
    };
    bool found = false;
    for (const QString &p : candidates) {
        if (QFileInfo::exists(p)) {
            found = true;
            break;
        }
    }
    if (!found) {
        GTEST_SKIP() << "PS1 stub core plugin not built beside test binary";
    }

    QString err;
    std::unique_ptr<EmuCore> core = EmuCoreLoader::loadCore(&err);
    ASSERT_TRUE(core) << err.toStdString();
    EXPECT_EQ(core->coreId(), QStringLiteral("stub"));

    QTemporaryFile bios(QDir::tempPath() + "/qtmesh_bios_XXXXXX.bin");
    QTemporaryFile iso(QDir::tempPath() + "/qtmesh_iso_XXXXXX.bin");
    ASSERT_TRUE(bios.open());
    ASSERT_TRUE(iso.open());
    bios.write("bios");
    iso.write("iso");
    bios.close();
    iso.close();

    ASSERT_TRUE(core->loadBios(bios.fileName()));
    ASSERT_TRUE(core->loadIso(iso.fileName()));

    for (int i = 0; i < 3; ++i)
        core->runFrame();

    const EmuFramebuffer &fb = core->framebuffer();
    EXPECT_TRUE(fb.isValid());
    EXPECT_GE(fb.frameIndex, 1u);
}

TEST_F(EmuCoreLoaderTest, LoadsLibretroWhenMednafenCorePresent)
{
    const QDir coresDir(QCoreApplication::applicationDirPath() + QStringLiteral("/PS1Cores"));
#if defined(Q_OS_WIN)
    const QString mednafenPath = coresDir.filePath(QStringLiteral("mednafen_psx_libretro.dll"));
#else
    const QString mednafenPath = coresDir.filePath(QStringLiteral("mednafen_psx_libretro.so"));
#endif
    if (!QFileInfo::exists(mednafenPath))
        GTEST_SKIP() << "mednafen_psx_libretro not installed in PS1Cores";

    qunsetenv("QTMESH_PS1_FORCE_STUB");

    QString err;
    std::unique_ptr<EmuCore> core = EmuCoreLoader::loadCore(&err);
    ASSERT_TRUE(core) << err.toStdString();
    EXPECT_EQ(core->coreId(), QStringLiteral("libretro"));

    qputenv("QTMESH_PS1_FORCE_STUB", "1");
}

TEST_F(EmuCoreLoaderTest, StubMirrorsVramAfterSync)
{
    const QDir coresDir(QCoreApplication::applicationDirPath() + QStringLiteral("/PS1Cores"));
#if defined(Q_OS_WIN)
    const QString stubPath = coresDir.filePath(QStringLiteral("qtmesh_ps1core_stub.dll"));
#else
    const QString stubPath = coresDir.filePath(QStringLiteral("libqtmesh_ps1core_stub.so"));
#endif
    if (!QFileInfo::exists(stubPath))
        GTEST_SKIP() << "PS1 stub core plugin not built beside test binary";

    QString err;
    std::unique_ptr<EmuCore> core = EmuCoreLoader::loadCore(&err);
    ASSERT_TRUE(core) << err.toStdString();
    ASSERT_EQ(core->coreId(), QStringLiteral("stub"));

    VramSnapshot vram;
    RipperHooks hooks;
    hooks.setVram(&vram);
    core->setHooks(&hooks);

    QTemporaryFile bios(QDir::tempPath() + "/qtmesh_bios_XXXXXX.bin");
    QTemporaryFile iso(QDir::tempPath() + "/qtmesh_iso_XXXXXX.bin");
    ASSERT_TRUE(bios.open());
    ASSERT_TRUE(iso.open());
    bios.write("bios");
    iso.write("iso");
    bios.close();
    iso.close();

    ASSERT_TRUE(core->loadBios(bios.fileName()));
    ASSERT_TRUE(core->loadIso(iso.fileName()));

    QString bootErr;
    ASSERT_TRUE(core->boot(&bootErr)) << bootErr.toStdString();

    for (int i = 0; i < 5; ++i) {
        core->runFrame();
        core->syncCaptureMirrors();
    }

    EXPECT_TRUE(vram.hasVisibleContent(8)) << "Stub VRAM pattern should mirror after syncCaptureMirrors";
}

#if defined(QTMESH_PS1_LIBRETRO_INTEGRATION_TESTS)
TEST_F(EmuCoreLoaderTest, LibretroBootsDiscWhenTestPathsSet)
{
    const QString biosPath = qEnvironmentVariable("QTMESH_PS1_TEST_BIOS");
    const QString isoPath = qEnvironmentVariable("QTMESH_PS1_TEST_ISO");
    ASSERT_FALSE(biosPath.isEmpty()) << "Set QTMESH_PS1_TEST_BIOS";
    ASSERT_FALSE(isoPath.isEmpty()) << "Set QTMESH_PS1_TEST_ISO";

    const QDir coresDir(QCoreApplication::applicationDirPath() + QStringLiteral("/PS1Cores"));
#if defined(Q_OS_WIN)
    ASSERT_TRUE(QFileInfo::exists(coresDir.filePath(QStringLiteral("mednafen_psx_libretro.dll"))));
#else
    ASSERT_TRUE(QFileInfo::exists(coresDir.filePath(QStringLiteral("mednafen_psx_libretro.so"))));
#endif

    qunsetenv("QTMESH_PS1_FORCE_STUB");

    QString err;
    std::unique_ptr<EmuCore> core = EmuCoreLoader::loadCore(&err);
    ASSERT_TRUE(core) << err.toStdString();
    ASSERT_EQ(core->coreId(), QStringLiteral("libretro"));

    ASSERT_TRUE(core->loadBios(biosPath)) << "BIOS load failed";
    ASSERT_TRUE(core->loadIso(isoPath)) << core->lastError().toStdString();

    QString bootErr;
    ASSERT_TRUE(core->boot(&bootErr)) << bootErr.toStdString();

    for (int i = 0; i < 30; ++i)
        core->runFrame();

    const EmuFramebuffer &fb = core->framebuffer();
    ASSERT_TRUE(fb.isValid());
    EXPECT_GE(fb.frameIndex, 1u);
    EXPECT_GT(fb.rgb24.size(), 0);

    const uchar r0 = fb.rgb24[0];
    const uchar g0 = fb.rgb24[1];
    const uchar b0 = fb.rgb24[2];
    const bool looksLikeStubGradient =
        (r0 == static_cast<uchar>(fb.frameIndex % 256) && g0 == 0 && b0 == 0);
    EXPECT_FALSE(looksLikeStubGradient) << "Framebuffer still looks like stub test pattern";

    qputenv("QTMESH_PS1_FORCE_STUB", "1");
}

TEST_F(EmuCoreLoaderTest, LibretroMirrorsVramAfterGameplay)
{
    const QString biosPath = qEnvironmentVariable("QTMESH_PS1_TEST_BIOS");
    const QString isoPath = qEnvironmentVariable("QTMESH_PS1_TEST_ISO");
    ASSERT_FALSE(biosPath.isEmpty()) << "Set QTMESH_PS1_TEST_BIOS";
    ASSERT_FALSE(isoPath.isEmpty()) << "Set QTMESH_PS1_TEST_ISO";

    qunsetenv("QTMESH_PS1_FORCE_STUB");

    QString err;
    std::unique_ptr<EmuCore> core = EmuCoreLoader::loadCore(&err);
    ASSERT_TRUE(core) << err.toStdString();
    ASSERT_EQ(core->coreId(), QStringLiteral("libretro"));

    VramSnapshot vram;
    RipperHooks hooks;
    hooks.setVram(&vram);
    core->setHooks(&hooks);

    ASSERT_TRUE(core->loadBios(biosPath)) << "BIOS load failed";
    ASSERT_TRUE(core->loadIso(isoPath)) << core->lastError().toStdString();

    QString bootErr;
    ASSERT_TRUE(core->boot(&bootErr)) << bootErr.toStdString();

    for (int i = 0; i < 180; ++i) {
        core->runFrame();
        if ((i % 30) == 29)
            core->syncCaptureMirrors();
    }
    core->syncCaptureMirrors();

    const EmuFramebuffer &fb = core->framebuffer();
    EXPECT_TRUE(fb.isValid()) << "No video frames from libretro";
    EXPECT_GT(fb.frameIndex, 0u);

    EXPECT_TRUE(vram.hasVisibleContent(8))
        << "VRAM mirror empty after 180 frames — core may not expose RETRO_MEMORY_VIDEO_RAM";

    qputenv("QTMESH_PS1_FORCE_STUB", "1");
}
#endif // QTMESH_PS1_LIBRETRO_INTEGRATION_TESTS

#endif // ENABLE_PS1_RIP
