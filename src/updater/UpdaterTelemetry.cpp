#include "UpdaterTelemetry.h"
#include "SentryReporter.h"

#include <QRegularExpression>

namespace UpdaterTelemetry {

void breadcrumb(const QString& category, const QString& message, const QString& level)
{
    if (!SentryReporter::isEnabled() || !isAllowedTelemetryMessage(message)) {
        return;
    }
    SentryReporter::addBreadcrumb(category, message, level);
}

bool isAllowedTelemetryMessage(const QString& message)
{
    if (message.contains(QStringLiteral("http://"), Qt::CaseInsensitive)
        || message.contains(QStringLiteral("https://"), Qt::CaseInsensitive)) {
        return false;
    }
    if (message.contains(QRegularExpression(QStringLiteral(R"([A-Za-z]:\\|/home/|/Users/|/tmp/|AppData|\.zip|\.dmg|\.exe|\.AppImage|manifest)"),
                                          QRegularExpression::CaseInsensitiveOption))) {
        return false;
    }
    return true;
}

} // namespace UpdaterTelemetry
