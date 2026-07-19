#ifndef SENTRYREPORTER_H
#define SENTRYREPORTER_H

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <cstdint>

/**
 * @brief Thin static wrapper isolating all Sentry SDK usage behind ENABLE_SENTRY.
 *
 * All methods are no-ops when Sentry is disabled at compile time or when the
 * user has opted out via QSettings. Product telemetry must go through this
 * wrapper so event names, privacy filtering, session tags and the anonymous
 * installation ID stay consistent across GUI, CLI and MCP.
 */
class SentryReporter
{
public:
    struct CapturedTelemetryEvent {
        QString name;
        QString level;
        QJsonObject tags;
        QJsonObject context;
    };

    // Lifecycle
    static void initialize();
    static void shutdown();
    static void configureSession(const QString &launchMode);

    // Opt-in / opt-out (persisted in QSettings)
    static bool isEnabled();
    static void setEnabled(bool enabled);
    static bool isFirstLaunch();
    static void showConsentDialog();
    static QString anonymousInstallationId();
    static void resetAnonymousInstallationId();
    static QString sessionId();
    static QString telemetryRole();

    // Breadcrumbs
    static void addBreadcrumb(const QString &category, const QString &message,
                              const QString &level = "info");

    // Tags (persist for the entire session, appear on every event)
    static void setTag(const QString &key, const QString &value);

    // Manual events
    static void captureMessage(const QString &message, const QString &level = "info");
    static void captureTelemetryEvent(const QString &eventName,
                                      const QJsonObject &properties = {},
                                      const QString &level = "info");
    static void captureInvocationEvent(const QString &surface, const QString &name,
                                       const QString &phase, qint64 durationMs = -1,
                                       bool changedScene = false,
                                       const QString &failureCategory = {},
                                       const QString &invocationId = {});
    static void captureFileWorkflowEvent(const QString &operation, const QString &phase,
                                         const QString &sourceSurface,
                                         const QString &inputPath = {},
                                         const QString &outputPath = {},
                                         qint64 durationMs = -1,
                                         bool success = true,
                                         const QString &failureCategory = {},
                                         int modelCount = -1,
                                         int animationCount = -1,
                                         qint64 approximateBytes = -1);

    static QString sanitizedValue(const QString &value);
    static QString sanitizedErrorCategory(const QString &error);
    static QString extensionOnly(const QString &path);
    static QString sizeBucket(qint64 bytes);
    static bool isKnownTelemetryEvent(const QString &eventName);
    static void clearCapturedTelemetryEventsForTest();
    static QVector<CapturedTelemetryEvent> capturedTelemetryEventsForTest();

    // Performance monitoring (opaque handles)
    static uintptr_t startTransaction(const QString &name, const QString &op);
    static uintptr_t startSpan(uintptr_t transaction, const QString &op, const QString &description);
    static void finishSpan(uintptr_t span);
    static void finishTransaction(uintptr_t transaction);

private:
    SentryReporter() = delete;
    static bool s_initialized;
    static QString s_sessionId;
    static QString s_launchMode;
    static qint64 s_sessionStartedMs;
    static QVector<CapturedTelemetryEvent> s_capturedTelemetryEvents;
};

#endif // SENTRYREPORTER_H
