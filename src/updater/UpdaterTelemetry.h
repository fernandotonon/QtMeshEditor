#ifndef UPDATERTELEMETRY_H
#define UPDATERTELEMETRY_H

#include <QString>

/**
 * @brief Privacy-safe Sentry breadcrumbs for the auto-updater funnel (#451).
 *
 * Payloads must contain only enum values, version strings, channel, sizes, and
 * error classes — never filenames, IPs, or URLs.
 */
namespace UpdaterTelemetry {

void breadcrumb(const QString& category, const QString& message,
                const QString& level = QStringLiteral("info"));

/// Returns false when @p message contains disallowed substrings (paths, URLs, etc.).
bool isAllowedTelemetryMessage(const QString& message);

} // namespace UpdaterTelemetry

#endif // UPDATERTELEMETRY_H
