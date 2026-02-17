#ifndef SENTRYREPORTER_H
#define SENTRYREPORTER_H

#include <QString>
#include <cstdint>

/**
 * @brief Thin static wrapper isolating all Sentry SDK usage behind ENABLE_SENTRY.
 *
 * All methods are no-ops when Sentry is disabled at compile time or when the
 * user has opted out via QSettings.
 */
class SentryReporter
{
public:
    // Lifecycle
    static void initialize();
    static void shutdown();

    // Opt-in / opt-out (persisted in QSettings)
    static bool isEnabled();
    static void setEnabled(bool enabled);
    static bool isFirstLaunch();
    static void showConsentDialog();

    // Breadcrumbs
    static void addBreadcrumb(const QString &category, const QString &message,
                              const QString &level = "info");

    // Manual events
    static void captureMessage(const QString &message, const QString &level = "info");

    // Performance monitoring (opaque handles)
    static uintptr_t startTransaction(const QString &name, const QString &op);
    static uintptr_t startSpan(uintptr_t transaction, const QString &op, const QString &description);
    static void finishSpan(uintptr_t span);
    static void finishTransaction(uintptr_t transaction);

private:
    SentryReporter() = delete;
    static bool s_initialized;
};

#endif // SENTRYREPORTER_H
