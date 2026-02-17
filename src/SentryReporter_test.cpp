#include <gtest/gtest.h>
#include <QSettings>
#include "SentryReporter.h"

class SentryReporterTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Clear any previous Sentry settings
        QSettings settings;
        settings.remove("Sentry/enabled");
    }

    void TearDown() override {
        QSettings settings;
        settings.remove("Sentry/enabled");
    }
};

TEST_F(SentryReporterTest, IsFirstLaunchWhenNoSettingsExist)
{
    EXPECT_TRUE(SentryReporter::isFirstLaunch());
}

TEST_F(SentryReporterTest, IsNotFirstLaunchAfterSetEnabled)
{
    SentryReporter::setEnabled(true);
    EXPECT_FALSE(SentryReporter::isFirstLaunch());
}

TEST_F(SentryReporterTest, SetEnabledPersists)
{
    SentryReporter::setEnabled(true);
    EXPECT_TRUE(SentryReporter::isEnabled());

    SentryReporter::setEnabled(false);
    EXPECT_FALSE(SentryReporter::isEnabled());
}

TEST_F(SentryReporterTest, DefaultIsEnabled)
{
    // When no setting exists, isEnabled returns true (opt-out)
    EXPECT_TRUE(SentryReporter::isEnabled());
}

TEST_F(SentryReporterTest, AllMethodsSafeWhenNotInitialized)
{
    // These should all be no-ops and not crash
    SentryReporter::addBreadcrumb("test", "message");
    SentryReporter::addBreadcrumb("test", "message", "warning");
    SentryReporter::captureMessage("test message");
    SentryReporter::captureMessage("test message", "error");

    uintptr_t txn = SentryReporter::startTransaction("test", "op");
    EXPECT_EQ(txn, 0u);

    uintptr_t span = SentryReporter::startSpan(txn, "op", "desc");
    EXPECT_EQ(span, 0u);

    SentryReporter::finishSpan(span);
    SentryReporter::finishSpan(0);
    SentryReporter::finishTransaction(txn);
    SentryReporter::finishTransaction(0);
}

TEST_F(SentryReporterTest, ShutdownSafeWhenNotInitialized)
{
    // Should not crash
    SentryReporter::shutdown();
}

TEST_F(SentryReporterTest, InitializeRespectsDisabledSetting)
{
    SentryReporter::setEnabled(false);
    // Should be a no-op since disabled
    SentryReporter::initialize();
    SentryReporter::shutdown();
}
