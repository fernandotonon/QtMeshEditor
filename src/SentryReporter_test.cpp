#include <gtest/gtest.h>
#include <QJsonObject>
#include <QSettings>
#include <QUuid>
#include "AppSettingsKeys.h"
#include "SentryReporter.h"

class SentryReporterTest : public ::testing::Test {
protected:
    void SetUp() override {
        SentryReporter::shutdown();
        SentryReporter::clearCapturedTelemetryEventsForTest();
        QSettings settings;
        settings.remove(AppSettingsKeys::sentryEnabled());
        settings.remove(AppSettingsKeys::anonymousInstallationId());
        qunsetenv("QTMESH_TELEMETRY_ROLE");
    }

    void TearDown() override {
        SentryReporter::shutdown();
        SentryReporter::clearCapturedTelemetryEventsForTest();
        QSettings settings;
        settings.remove(AppSettingsKeys::sentryEnabled());
        settings.remove(AppSettingsKeys::anonymousInstallationId());
        qunsetenv("QTMESH_TELEMETRY_ROLE");
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

TEST_F(SentryReporterTest, AnonymousInstallationIdCreatedReusedAndReset)
{
    SentryReporter::setEnabled(true);
    const QString first = SentryReporter::anonymousInstallationId();
    EXPECT_FALSE(first.isEmpty());
    EXPECT_FALSE(QUuid(first).isNull());

    const QString second = SentryReporter::anonymousInstallationId();
    EXPECT_EQ(first, second);

    SentryReporter::resetAnonymousInstallationId();
    QSettings settings;
    EXPECT_FALSE(settings.contains(AppSettingsKeys::anonymousInstallationId()));

    const QString third = SentryReporter::anonymousInstallationId();
    EXPECT_FALSE(third.isEmpty());
    EXPECT_NE(first, third);
}

TEST_F(SentryReporterTest, OptOutDoesNotGenerateInstallationIdOrEvents)
{
    SentryReporter::setEnabled(true);
    SentryReporter::configureSession(QStringLiteral("gui"));
    EXPECT_FALSE(SentryReporter::sessionId().isEmpty());

    SentryReporter::setEnabled(false);
    EXPECT_TRUE(SentryReporter::anonymousInstallationId().isEmpty());
    EXPECT_TRUE(SentryReporter::sessionId().isEmpty());
    SentryReporter::captureTelemetryEvent(QStringLiteral("app.startup"));
    EXPECT_TRUE(SentryReporter::capturedTelemetryEventsForTest().isEmpty());

    QSettings settings;
    EXPECT_FALSE(settings.contains(AppSettingsKeys::anonymousInstallationId()));
}

TEST_F(SentryReporterTest, SessionsShareInstallationIdButHaveDifferentSessionIds)
{
    SentryReporter::setEnabled(true);
    SentryReporter::configureSession(QStringLiteral("gui"));
    const QString installation = SentryReporter::anonymousInstallationId();
    const QString firstSession = SentryReporter::sessionId();

    SentryReporter::shutdown();
    SentryReporter::clearCapturedTelemetryEventsForTest();
    SentryReporter::configureSession(QStringLiteral("cli"));
    const QString secondInstallation = SentryReporter::anonymousInstallationId();
    const QString secondSession = SentryReporter::sessionId();

    EXPECT_EQ(installation, secondInstallation);
    EXPECT_NE(firstSession, secondSession);
}

TEST_F(SentryReporterTest, SanitizesPathsFilenamesAndPrompts)
{
    const QString unsafe = QStringLiteral(
        "Open /home/alice/secret/model.fbx and C:\\Users\\Bob\\Desktop\\dragon.obj prompt: make it blue");
    const QString safe = SentryReporter::sanitizedValue(unsafe);
    EXPECT_FALSE(safe.contains(QStringLiteral("/home/alice")));
    EXPECT_FALSE(safe.contains(QStringLiteral("C:\\Users")));
    EXPECT_FALSE(safe.contains(QStringLiteral("model.fbx")));
    EXPECT_FALSE(safe.contains(QStringLiteral("dragon.obj")));

    SentryReporter::setEnabled(true);
    SentryReporter::configureSession(QStringLiteral("mcp"));
    SentryReporter::captureTelemetryEvent(QStringLiteral("mcp.tool.started"),
        QJsonObject{{QStringLiteral("source_surface"), QStringLiteral("mcp")},
                    {QStringLiteral("prompt"), QStringLiteral("make a private character")},
                    {QStringLiteral("path"), QStringLiteral("/tmp/private/model.fbx")}});
    const auto events = SentryReporter::capturedTelemetryEventsForTest();
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].context.value(QStringLiteral("prompt")).toString(), QStringLiteral("[redacted]"));
    EXPECT_EQ(events[0].context.value(QStringLiteral("path")).toString(), QStringLiteral("[redacted]"));
}

TEST_F(SentryReporterTest, EventNamesAreAllowListedAndRequiredTagsArePresent)
{
    SentryReporter::setEnabled(true);
    SentryReporter::configureSession(QStringLiteral("cli"));
    SentryReporter::captureTelemetryEvent(QStringLiteral("mcp.argument.leak"),
                                          QJsonObject{{QStringLiteral("source_surface"), QStringLiteral("mcp")}});
    EXPECT_TRUE(SentryReporter::capturedTelemetryEventsForTest().isEmpty());

    SentryReporter::captureTelemetryEvent(QStringLiteral("file.import.completed"),
        QJsonObject{{QStringLiteral("source_surface"), QStringLiteral("cli")},
                    {QStringLiteral("duration_ms"), 42}});
    const auto events = SentryReporter::capturedTelemetryEventsForTest();
    ASSERT_EQ(events.size(), 1);
    const QJsonObject tags = events[0].tags;
    EXPECT_EQ(events[0].name, QStringLiteral("file.import.completed"));
    EXPECT_TRUE(tags.contains(QStringLiteral("app.version")));
    EXPECT_TRUE(tags.contains(QStringLiteral("release")));
    EXPECT_EQ(tags.value(QStringLiteral("launch_mode")).toString(), QStringLiteral("cli"));
    EXPECT_EQ(tags.value(QStringLiteral("source_surface")).toString(), QStringLiteral("cli"));
    EXPECT_EQ(tags.value(QStringLiteral("telemetry.role")).toString(), QStringLiteral("user"));
    EXPECT_FALSE(tags.value(QStringLiteral("session.id")).toString().isEmpty());
    EXPECT_TRUE(tags.contains(QStringLiteral("os")));
    EXPECT_TRUE(tags.contains(QStringLiteral("arch")));

    EXPECT_TRUE(SentryReporter::isKnownTelemetryEvent(QStringLiteral("ai.model_download.canceled")));
    EXPECT_TRUE(SentryReporter::isKnownTelemetryEvent(QStringLiteral("ai.model_delete.started")));
    EXPECT_TRUE(SentryReporter::isKnownTelemetryEvent(QStringLiteral("ai.model_download_all.started")));
}

TEST_F(SentryReporterTest, InvocationEventsUseSurfaceSpecificIdentifier)
{
    SentryReporter::setEnabled(true);
    SentryReporter::configureSession(QStringLiteral("cli"));
    SentryReporter::captureInvocationEvent(QStringLiteral("cli"), QStringLiteral("segment"),
                                           QStringLiteral("started"), -1, false, QString(),
                                           QStringLiteral("invocation-1"));
    SentryReporter::captureInvocationEvent(QStringLiteral("mcp"), QStringLiteral("load_mesh"),
                                           QStringLiteral("started"), -1, false, QString(),
                                           QStringLiteral("invocation-2"));

    const auto events = SentryReporter::capturedTelemetryEventsForTest();
    ASSERT_EQ(events.size(), 2);
    EXPECT_EQ(events[0].context.value(QStringLiteral("command")).toString(), QStringLiteral("segment"));
    EXPECT_FALSE(events[0].context.contains(QStringLiteral("tool")));
    EXPECT_EQ(events[1].context.value(QStringLiteral("tool")).toString(), QStringLiteral("load_mesh"));
    EXPECT_FALSE(events[1].context.contains(QStringLiteral("command")));
}

TEST_F(SentryReporterTest, TelemetryRoleDefaultsAndHonorsAllowedValues)
{
    EXPECT_EQ(SentryReporter::telemetryRole(), QStringLiteral("user"));
    qputenv("QTMESH_TELEMETRY_ROLE", "developer");
    EXPECT_EQ(SentryReporter::telemetryRole(), QStringLiteral("developer"));
    qputenv("QTMESH_TELEMETRY_ROLE", "ci");
    EXPECT_EQ(SentryReporter::telemetryRole(), QStringLiteral("ci"));
    qputenv("QTMESH_TELEMETRY_ROLE", "tester");
    EXPECT_EQ(SentryReporter::telemetryRole(), QStringLiteral("tester"));
    qputenv("QTMESH_TELEMETRY_ROLE", "fernando");
    EXPECT_EQ(SentryReporter::telemetryRole(), QStringLiteral("user"));
}

TEST_F(SentryReporterTest, FileWorkflowUsesExtensionsOnly)
{
    SentryReporter::setEnabled(true);
    SentryReporter::configureSession(QStringLiteral("gui"));
    SentryReporter::captureFileWorkflowEvent({QStringLiteral("import"), QStringLiteral("completed"),
        QStringLiteral("gui"), QStringLiteral("/Users/me/Secret Character.fbx"), QString(), 5, true});
    const auto events = SentryReporter::capturedTelemetryEventsForTest();
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].context.value(QStringLiteral("input_format")).toString(), QStringLiteral("fbx"));
    EXPECT_FALSE(events[0].context.contains(QStringLiteral("input_path")));
}
