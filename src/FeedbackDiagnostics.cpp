#include "FeedbackDiagnostics.h"

#include "CloudCredentialStore.h"
#include "SentryReporter.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSysInfo>
#include <QUuid>

namespace {

struct OperationContext {
    QString operation;
    QString format;
    QString errorCode;
    QString errorMessage;
};

struct DiagnosticsState {
    QMutex mutex;
    QString sessionId;
    QList<QString> recentEvents;
    OperationContext operationContext;
};

DiagnosticsState& state()
{
    static DiagnosticsState s;
    return s;
}

QString appVersionString()
{
    const QString version = QCoreApplication::applicationVersion();
    return version.isEmpty() ? QStringLiteral("unknown") : version;
}

QJsonObject featureFlagsObject()
{
    QJsonObject flags;
#ifdef ENABLE_LOCAL_LLM
    flags.insert(QStringLiteral("localLlm"), true);
#else
    flags.insert(QStringLiteral("localLlm"), false);
#endif
#ifdef ENABLE_STABLE_DIFFUSION
    flags.insert(QStringLiteral("stableDiffusion"), true);
#else
    flags.insert(QStringLiteral("stableDiffusion"), false);
#endif
#ifdef ENABLE_PS1_RIP
    flags.insert(QStringLiteral("ps1Rip"), true);
#else
    flags.insert(QStringLiteral("ps1Rip"), false);
#endif
#ifdef ENABLE_SENTRY
    flags.insert(QStringLiteral("sentry"), true);
#else
    flags.insert(QStringLiteral("sentry"), false);
#endif
    flags.insert(QStringLiteral("crashReports"), SentryReporter::isEnabled());
    return flags;
}

bool looksLikeSecretKey(const QString& key)
{
    static const QRegularExpression pattern(
        QStringLiteral(R"((token|secret|password|authorization|api[_-]?key|signed|dsn))"),
        QRegularExpression::CaseInsensitiveOption);
    return pattern.match(key).hasMatch();
}

QString redactBearerTokens(const QString& value)
{
    QString out = value;
    static const QRegularExpression bearer(
        QStringLiteral(R"(Bearer\s+[A-Za-z0-9._~+/\-=]+)"),
        QRegularExpression::CaseInsensitiveOption);
    return out.replace(bearer, QStringLiteral("Bearer [redacted]"));
}

QString redactSignedUrls(const QString& value)
{
    QString out = value;
    static const QRegularExpression signedParam(
        QStringLiteral(R"(([?&](?:X-Amz-Signature|sig|signature|token|access_token)=[^&\s]+))"),
        QRegularExpression::CaseInsensitiveOption);
    return out.replace(signedParam, QStringLiteral("[redacted-url-param]"));
}

QString redactAbsolutePaths(const QString& value)
{
    QString out = value;
#if defined(Q_OS_WIN)
    static const QRegularExpression winPath(
        QStringLiteral(R"(([A-Za-z]:\\[^\s\"']+|\\\\[^\s\"']+))"));
    out.replace(winPath, QStringLiteral("[redacted-path]"));
#endif
    static const QRegularExpression unixPath(QStringLiteral(R"(/(?:[^\s\"'/]+/)+[^\s\"'/]+)"));
    out.replace(unixPath, QStringLiteral("[redacted-path]"));
    static const QRegularExpression homePath(QStringLiteral(R"(~(?:/[^\s\"']+)+)"));
    out.replace(homePath, QStringLiteral("[redacted-path]"));
    return out;
}

QString redactEnvAssignments(const QString& value)
{
    QString out = value;
    static const QRegularExpression envAssign(
        QStringLiteral(R"(([A-Z][A-Z0-9_]{2,})=([^\s\"']+))"));
    return out.replace(envAssign, QStringLiteral("\\1=[redacted]"));
}

QJsonObject redactJsonObject(const QJsonObject& object)
{
    QJsonObject out;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (looksLikeSecretKey(it.key()))
            continue;
        out.insert(it.key(), FeedbackDiagnostics::redactJsonValue(it.value()));
    }
    return out;
}

} // namespace

QString FeedbackDiagnostics::editorSessionId()
{
    QMutexLocker lock(&state().mutex);
    if (state().sessionId.isEmpty())
        state().sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return state().sessionId;
}

void FeedbackDiagnostics::recordRecentEvent(const QString& category, const QString& message)
{
    const QString sanitizedCategory = redactString(category.trimmed());
    const QString sanitizedMessage = redactString(message.trimmed());
    if (sanitizedCategory.isEmpty() && sanitizedMessage.isEmpty())
        return;

    QMutexLocker lock(&state().mutex);
    const QString line = sanitizedCategory.isEmpty()
        ? sanitizedMessage
        : QStringLiteral("%1: %2").arg(sanitizedCategory, sanitizedMessage);
    state().recentEvents.append(line);
    while (state().recentEvents.size() > kMaxRecentEvents)
        state().recentEvents.removeFirst();
}

void FeedbackDiagnostics::setOperationContext(const QString& operation,
                                              const QString& format,
                                              const QString& errorCode,
                                              const QString& errorMessage)
{
    QMutexLocker lock(&state().mutex);
    state().operationContext.operation = operation;
    state().operationContext.format = format;
    state().operationContext.errorCode = errorCode;
    state().operationContext.errorMessage = errorMessage;
}

void FeedbackDiagnostics::clearOperationContext()
{
    QMutexLocker lock(&state().mutex);
    state().operationContext = {};
}

QString FeedbackDiagnostics::redactPath(const QString& value)
{
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return trimmed;

    const QStringList parts =
        trimmed.split(QRegularExpression(QStringLiteral("[/\\\\]")), Qt::SkipEmptyParts);
    if (!parts.isEmpty())
        return parts.last();

    return QStringLiteral("[redacted-path]");
}

QString FeedbackDiagnostics::redactString(const QString& value)
{
    QString out = value;
    out = redactBearerTokens(out);
    out = redactSignedUrls(out);
    out = redactAbsolutePaths(out);
    out = redactEnvAssignments(out);
    if (out.size() > 512)
        out = out.left(512) + QStringLiteral("…");
    return out;
}

QJsonValue FeedbackDiagnostics::redactJsonValue(const QJsonValue& value)
{
    if (value.isString())
        return redactString(value.toString());
    if (value.isObject())
        return redactJsonObject(value.toObject());
    if (value.isArray()) {
        QJsonArray array;
        const QJsonArray in = value.toArray();
        const int limit = qMin(in.size(), 32);
        for (int i = 0; i < limit; ++i)
            array.append(redactJsonValue(in.at(i)));
        return array;
    }
    return value;
}

QJsonObject FeedbackDiagnostics::collectDiagnostics(bool includeOperationContext)
{
    QJsonObject diagnostics;
    diagnostics.insert(QStringLiteral("appVersion"), appVersionString());
    diagnostics.insert(QStringLiteral("osName"), QSysInfo::productType());
    diagnostics.insert(QStringLiteral("osVersion"), QSysInfo::productVersion());
    diagnostics.insert(QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture());
    diagnostics.insert(QStringLiteral("locale"), QLocale::system().name());
    diagnostics.insert(QStringLiteral("editorSessionId"), editorSessionId());
    diagnostics.insert(QStringLiteral("cloudConnected"), CloudCredentialStore::hasSession());
    diagnostics.insert(QStringLiteral("featureFlags"), featureFlagsObject());

    {
        QMutexLocker lock(&state().mutex);
        QJsonArray events;
        for (const QString& event : state().recentEvents)
            events.append(redactString(event));
        diagnostics.insert(QStringLiteral("recentEvents"), events);

        if (includeOperationContext && !state().operationContext.operation.isEmpty()) {
            QJsonObject op;
            op.insert(QStringLiteral("operation"), redactString(state().operationContext.operation));
            if (!state().operationContext.format.isEmpty())
                op.insert(QStringLiteral("format"), redactString(state().operationContext.format));
            if (!state().operationContext.errorCode.isEmpty())
                op.insert(QStringLiteral("errorCode"), redactString(state().operationContext.errorCode));
            if (!state().operationContext.errorMessage.isEmpty())
                op.insert(QStringLiteral("errorMessage"),
                          redactString(state().operationContext.errorMessage));
            diagnostics.insert(QStringLiteral("lastOperation"), op);
        }
    }

    return redactJsonObject(diagnostics);
}

QString FeedbackDiagnostics::diagnosticsJsonString(const QJsonObject& diagnostics)
{
    QByteArray json = QJsonDocument(diagnostics).toJson(QJsonDocument::Compact);
    if (json.size() <= kMaxDiagnosticsJsonBytes)
        return QString::fromUtf8(json);

    QJsonObject trimmed = diagnostics;
    trimmed.remove(QStringLiteral("recentEvents"));
    json = QJsonDocument(trimmed).toJson(QJsonDocument::Compact);
    if (json.size() <= kMaxDiagnosticsJsonBytes)
        return QString::fromUtf8(json);

    trimmed.remove(QStringLiteral("lastOperation"));
    json = QJsonDocument(trimmed).toJson(QJsonDocument::Compact);
    if (json.size() > kMaxDiagnosticsJsonBytes)
        json = json.left(kMaxDiagnosticsJsonBytes);
    return QString::fromUtf8(json);
}

void FeedbackDiagnostics::resetForTests()
{
    QMutexLocker lock(&state().mutex);
    state().sessionId.clear();
    state().recentEvents.clear();
    state().operationContext = {};
}
