#include <gtest/gtest.h>

#include <QSettings>

#include "AppSettingsKeys.h"
#include "UpdaterController.h"

namespace {

class UpdaterControllerTestEnv : public ::testing::Test
{
protected:
    void SetUp() override
    {
        QSettings settings;
        settings.remove(AppSettingsKeys::updaterChannel());
        settings.remove(AppSettingsKeys::updaterCheckOnStartup());
        settings.remove(AppSettingsKeys::updaterAutoDownload());
        settings.remove(AppSettingsKeys::updaterSkippedVersion());
    }

    void TearDown() override
    {
        // Leave singleton alive — explicit kill() during QApplication teardown
        // can race Qt widget cleanup on some platforms.
    }
};

} // namespace

TEST_F(UpdaterControllerTestEnv, ChannelDefaultsToStableAndPersists)
{
    UpdaterController* controller = UpdaterController::instance();
    ASSERT_NE(controller, nullptr);
    EXPECT_EQ(controller->channel(), QStringLiteral("stable"));

    controller->setChannel(QStringLiteral("beta"));
    EXPECT_EQ(controller->channel(), QStringLiteral("beta"));

    QSettings settings;
    EXPECT_EQ(settings.value(AppSettingsKeys::updaterChannel()).toString(),
              QStringLiteral("beta"));
}

TEST_F(UpdaterControllerTestEnv, StartupAndAutoDownloadSettingsPersist)
{
    UpdaterController* controller = UpdaterController::instance();
    controller->setCheckOnStartup(false);
    controller->setAutoDownload(true);

    QSettings settings;
    EXPECT_FALSE(settings.value(AppSettingsKeys::updaterCheckOnStartup(), true).toBool());
    EXPECT_TRUE(settings.value(AppSettingsKeys::updaterAutoDownload(), false).toBool());
}

TEST_F(UpdaterControllerTestEnv, SkipVersionWritesSettingsKey)
{
    UpdaterController* controller = UpdaterController::instance();
    controller->setLatestVersionForTest(QStringLiteral("3.5.4-beta.1"));
    controller->skipThisVersion();

    QSettings settings;
    EXPECT_EQ(settings.value(AppSettingsKeys::updaterSkippedVersion()).toString(),
              QStringLiteral("3.5.4-beta.1"));
    EXPECT_EQ(controller->state(), UpdaterController::State::UpToDate);
}
