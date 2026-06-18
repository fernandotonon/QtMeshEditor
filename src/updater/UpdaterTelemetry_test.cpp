#include "UpdaterTelemetry.h"

#include "SentryReporter.h"

#include <gtest/gtest.h>

TEST(UpdaterTelemetryTest, RejectsUrlsAndPaths)
{
    EXPECT_FALSE(UpdaterTelemetry::isAllowedTelemetryMessage(
        QStringLiteral("https://api.github.com/repos/foo/releases")));
    EXPECT_FALSE(UpdaterTelemetry::isAllowedTelemetryMessage(
        QStringLiteral("staged=/home/user/AppData/updater/staging/3.5.4/foo.zip")));
    EXPECT_FALSE(UpdaterTelemetry::isAllowedTelemetryMessage(
        QStringLiteral("manifest=/tmp/install-manifest.txt")));
}

TEST(UpdaterTelemetryTest, AllowsVersionAndChannelPayloads)
{
    EXPECT_TRUE(UpdaterTelemetry::isAllowedTelemetryMessage(
        QStringLiteral("channel=stable local=3.5.3")));
    EXPECT_TRUE(UpdaterTelemetry::isAllowedTelemetryMessage(
        QStringLiteral("remote=3.5.4 comparison=1")));
    EXPECT_TRUE(UpdaterTelemetry::isAllowedTelemetryMessage(
        QStringLiteral("version=3.5.4")));
}

TEST(UpdaterTelemetryTest, BreadcrumbHonorsTelemetryOptOut)
{
    const bool previous = SentryReporter::isEnabled();
    SentryReporter::setEnabled(false);
    // Must not crash when telemetry is disabled.
    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.check.start"),
                                 QStringLiteral("channel=stable local=3.5.3"));
    SentryReporter::setEnabled(previous);
}

TEST(UpdaterTelemetryTest, BreadcrumbDropsDisallowedPayloads)
{
    const bool previous = SentryReporter::isEnabled();
    SentryReporter::setEnabled(true);
    // Must not crash; disallowed payloads are silently dropped.
    UpdaterTelemetry::breadcrumb(QStringLiteral("updater.download.complete"),
                                 QStringLiteral("file=/tmp/foo.zip"));
    SentryReporter::setEnabled(previous);
}
