#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>

#include "PS1/runtime/EmuCore.h"
#include "PS1/runtime/EmuCoreLoader.h"

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

#endif // ENABLE_PS1_RIP
