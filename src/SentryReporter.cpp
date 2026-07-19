#include "SentryReporter.h"
#include "FeedbackDiagnostics.h"
#include "AppSettingsKeys.h"
#include <QSettings>
#include <QMessageBox>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QSysInfo>
#include <QUuid>

#ifdef ENABLE_SENTRY
#include <sentry.h>
#endif

bool SentryReporter::s_initialized = false;
QString SentryReporter::s_sessionId;
QString SentryReporter::s_launchMode = QStringLiteral("unknown");
qint64 SentryReporter::s_sessionStartedMs = 0;
#ifdef QTMESH_UNIT_TESTS
QVector<SentryReporter::CapturedTelemetryEvent> SentryReporter::s_capturedTelemetryEvents;
#endif

namespace {
QString normalizedRole(QString role)
{
    role = role.trimmed().toLower();
    if (role == QStringLiteral("developer") || role == QStringLiteral("ci")
        || role == QStringLiteral("tester"))
        return role;
    return QStringLiteral("user");
}

QJsonObject sanitizedObject(const QJsonObject& in)
{
    QJsonObject out;
    for (auto it = in.constBegin(); it != in.constEnd(); ++it) {
        const QString key = it.key();
        const QString lower = key.toLower();
        if (lower.contains(QStringLiteral("prompt"))
            || lower.contains(QStringLiteral("path"))
            || lower.contains(QStringLiteral("filename"))
            || lower.contains(QStringLiteral("file_name"))
            || lower.contains(QStringLiteral("email"))
            || lower.contains(QStringLiteral("github"))
            || lower.contains(QStringLiteral("token"))) {
            out.insert(key, QStringLiteral("[redacted]"));
            continue;
        }
        if (it.value().isString())
            out.insert(key, SentryReporter::sanitizedValue(it.value().toString()));
        else if (it.value().isObject())
            out.insert(key, sanitizedObject(it.value().toObject()));
        else
            out.insert(key, it.value());
    }
    return out;
}

#ifdef ENABLE_SENTRY
sentry_level_t sentryLevelFromString(const QString& level)
{
    if (level == "warning") return SENTRY_LEVEL_WARNING;
    if (level == "error") return SENTRY_LEVEL_ERROR;
    if (level == "fatal") return SENTRY_LEVEL_FATAL;
    if (level == "debug") return SENTRY_LEVEL_DEBUG;
    return SENTRY_LEVEL_INFO;
}

sentry_value_t jsonToSentryValue(const QJsonValue& value)
{
    if (value.isBool()) return sentry_value_new_bool(value.toBool());
    if (value.isDouble()) return sentry_value_new_double(value.toDouble());
    if (value.isString()) return sentry_value_new_string(value.toString().toUtf8().constData());
    if (value.isArray()) {
        sentry_value_t arr = sentry_value_new_list();
        const QJsonArray json = value.toArray();
        for (const QJsonValue& item : json)
            sentry_value_append(arr, jsonToSentryValue(item));
        return arr;
    }
    if (value.isObject()) {
        sentry_value_t obj = sentry_value_new_object();
        const QJsonObject json = value.toObject();
        for (auto it = json.constBegin(); it != json.constEnd(); ++it)
            sentry_value_set_by_key(obj, it.key().toUtf8().constData(), jsonToSentryValue(it.value()));
        return obj;
    }
    return sentry_value_new_null();
}
#endif
} // namespace

void SentryReporter::initialize()
{
#ifdef ENABLE_SENTRY
    if (s_initialized) return;
    if (!isEnabled()) return;

    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options,
        "https://51f59f98ea62a3b7523ce52de2b2387d@o4509454943322112.ingest.us.sentry.io/4510896994254848");
    // sentry-native versions bundled by this project do not collect default PII
    // through a sendDefaultPii option. Keep identity limited to opaque user.id.

    QString dbPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                     + "/sentry-db";
    QDir().mkpath(dbPath);
    QByteArray dbPathUtf8 = dbPath.toUtf8();
    sentry_options_set_database_path(options, dbPathUtf8.constData());

    QByteArray release = QStringLiteral("qtmesheditor@%1")
        .arg(QCoreApplication::applicationVersion()).toUtf8();
    sentry_options_set_release(options, release.constData());
    sentry_options_set_traces_sample_rate(options, 1.0);

    int result = sentry_init(options);
    if (result == 0) {
        s_initialized = true;
        configureSession(s_launchMode);
        qDebug() << "Sentry initialized successfully (db:" << dbPath << ")";
    } else {
        qWarning() << "Sentry initialization failed with code:" << result;
    }
#endif
}

void SentryReporter::shutdown()
{
    if (!s_sessionId.isEmpty() && s_sessionStartedMs > 0) {
        QJsonObject props;
        props["duration_ms"] = QDateTime::currentMSecsSinceEpoch() - s_sessionStartedMs;
        captureTelemetryEvent(QStringLiteral("app.shutdown"), props);
    }
#ifdef ENABLE_SENTRY
    if (!s_initialized) return;
    sentry_close();
    s_initialized = false;
#endif
}

void SentryReporter::configureSession(const QString &launchMode)
{
    s_launchMode = launchMode;
    if (!isEnabled())
        return;
    if (s_sessionId.isEmpty()) {
        s_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        s_sessionStartedMs = QDateTime::currentMSecsSinceEpoch();
    }
    setTag(QStringLiteral("os"), QSysInfo::prettyProductName());
    setTag(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    setTag(QStringLiteral("qt_version"), qVersion());
    setTag(QStringLiteral("launch_mode"), launchMode);
    setTag(QStringLiteral("telemetry.role"), telemetryRole());
    setTag(QStringLiteral("session.id"), s_sessionId);
    setTag(QStringLiteral("app.version"), QCoreApplication::applicationVersion());
#ifdef ENABLE_SENTRY
    if (s_initialized) {
        sentry_value_t user = sentry_value_new_object();
        const QString id = QStringLiteral("install:%1").arg(anonymousInstallationId());
        sentry_value_set_by_key(user, "id", sentry_value_new_string(id.toUtf8().constData()));
        sentry_set_user(user);
    }
#endif
}

bool SentryReporter::isEnabled()
{
    QSettings settings;
    return settings.value(AppSettingsKeys::sentryEnabled(), true).toBool();
}

void SentryReporter::setEnabled(bool enabled)
{
    QSettings settings;
    settings.setValue(AppSettingsKeys::sentryEnabled(), enabled);
    if (!enabled)
        resetAnonymousInstallationId();
}

bool SentryReporter::isFirstLaunch()
{
    QSettings settings;
    return !settings.contains(AppSettingsKeys::sentryEnabled());
}

void SentryReporter::showConsentDialog()
{
    QMessageBox msgBox;
    msgBox.setWindowTitle(QObject::tr("Crash Reporting"));
    msgBox.setText(QObject::tr(
        "QtMeshEditor automatically sends anonymous crash reports "
        "to help improve the application.\n\n"
        "You can disable this at any time in Help > Send Crash Reports."));
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.setIcon(QMessageBox::Information);

    msgBox.exec();
    setEnabled(true);
}

QString SentryReporter::anonymousInstallationId()
{
    if (!isEnabled())
        return {};
    QSettings settings;
    const QString key = AppSettingsKeys::anonymousInstallationId();
    QString id = settings.value(key).toString();
    if (QUuid(id).isNull()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue(key, id);
    }
    return id;
}

void SentryReporter::resetAnonymousInstallationId()
{
    QSettings settings;
    settings.remove(AppSettingsKeys::anonymousInstallationId());
    s_sessionId.clear();
    s_sessionStartedMs = 0;
#ifdef ENABLE_SENTRY
    if (s_initialized) {
        sentry_set_user(sentry_value_new_object());
        sentry_close();
        s_initialized = false;
    }
#endif
}

QString SentryReporter::sessionId()
{
    if (!isEnabled())
        return {};
    if (s_sessionId.isEmpty()) {
        s_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        s_sessionStartedMs = QDateTime::currentMSecsSinceEpoch();
    }
    return s_sessionId;
}

QString SentryReporter::telemetryRole()
{
    return normalizedRole(QString::fromUtf8(qgetenv("QTMESH_TELEMETRY_ROLE")));
}

void SentryReporter::setTag(const QString &key, const QString &value)
{
#ifdef ENABLE_SENTRY
    if (!isEnabled() || !s_initialized) return;
    sentry_set_tag(key.toUtf8().constData(), sanitizedValue(value).toUtf8().constData());
#else
    Q_UNUSED(key);
    Q_UNUSED(value);
#endif
}

void SentryReporter::addBreadcrumb(const QString &category, const QString &message,
                                   const QString &level)
{
    const QString safeCategory = sanitizedValue(category);
    const QString safeMessage = sanitizedValue(message);
    FeedbackDiagnostics::recordRecentEvent(safeCategory, safeMessage);

#ifdef ENABLE_SENTRY
    if (!s_initialized) return;

    sentry_value_t crumb = sentry_value_new_breadcrumb("default",
        safeMessage.toUtf8().constData());
    sentry_value_set_by_key(crumb, "category",
        sentry_value_new_string(safeCategory.toUtf8().constData()));
    sentry_value_set_by_key(crumb, "level",
        sentry_value_new_string(level.toUtf8().constData()));
    sentry_add_breadcrumb(crumb);
#else
    Q_UNUSED(level);
#endif
}

void SentryReporter::captureMessage(const QString &message, const QString &level)
{
#ifdef ENABLE_SENTRY
    if (!isEnabled() || !s_initialized) return;

    sentry_value_t event = sentry_value_new_message_event(sentryLevelFromString(level),
        "qtmesheditor", sanitizedValue(message).toUtf8().constData());
    sentry_capture_event(event);
#else
    Q_UNUSED(message);
    Q_UNUSED(level);
#endif
}

void SentryReporter::captureTelemetryEvent(const QString &eventName,
                                           const QJsonObject &properties,
                                           const QString &level)
{
    if (!isEnabled() || eventName.isEmpty() || !isKnownTelemetryEvent(eventName))
        return;
    sessionId();
    anonymousInstallationId();

    QJsonObject tags;
    tags["app.version"] = QCoreApplication::applicationVersion();
    tags["release"] = QStringLiteral("qtmesheditor@%1").arg(QCoreApplication::applicationVersion());
    tags["launch_mode"] = s_launchMode;
    tags["telemetry.role"] = telemetryRole();
    tags["session.id"] = s_sessionId;
    tags["os"] = QSysInfo::prettyProductName();
    tags["arch"] = QSysInfo::currentCpuArchitecture();
    tags["qt_version"] = qVersion();
    if (properties.contains("source_surface"))
        tags["source_surface"] = properties.value("source_surface").toString();
    if (properties.contains("capability"))
        tags["capability"] = properties.value("capability").toString();

    QJsonObject context = sanitizedObject(properties);
    context["event_name"] = eventName;
#ifdef QTMESH_UNIT_TESTS
    s_capturedTelemetryEvents.push_back({eventName, level, tags, context});
#endif

#ifdef ENABLE_SENTRY
    if (!s_initialized)
        return;
    sentry_value_t event = sentry_value_new_message_event(
        sentryLevelFromString(level), "qtmesheditor.telemetry", eventName.toUtf8().constData());
    sentry_value_set_by_key(event, "tags", jsonToSentryValue(tags));
    sentry_value_set_by_key(event, "contexts",
                            jsonToSentryValue(QJsonObject{{QStringLiteral("telemetry"), context}}));
    sentry_capture_event(event);
#endif
}

void SentryReporter::captureInvocationEvent(const QString &surface, const QString &name,
                                            const QString &phase, qint64 durationMs,
                                            bool changedScene,
                                            const QString &failureCategory,
                                            const QString &invocationId)
{
    QJsonObject props;
    props["source_surface"] = surface;
    if (surface == QStringLiteral("mcp"))
        props["tool"] = sanitizedValue(name);
    else
        props["command"] = sanitizedValue(name);
    props["phase"] = phase;
    props["invocation.id"] = invocationId.isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces) : invocationId;
    props["changed_scene"] = changedScene;
    props["success"] = phase != QStringLiteral("failed");
    if (durationMs >= 0)
        props["duration_ms"] = durationMs;
    if (!failureCategory.isEmpty())
        props["failure_category"] = sanitizedErrorCategory(failureCategory);
    captureTelemetryEvent(QStringLiteral("%1.%2.%3").arg(surface, surface == "mcp" ? "tool" : "command", phase),
                          props, phase == QStringLiteral("failed") ? QStringLiteral("error") : QStringLiteral("info"));
}

void SentryReporter::captureFileWorkflowEvent(const FileWorkflowTelemetry &telemetry)
{
    QJsonObject props;
    props["source_surface"] = telemetry.sourceSurface;
    props["input_format"] = extensionOnly(telemetry.inputPath);
    props["output_format"] = extensionOnly(telemetry.outputPath);
    props["asset_kind"] = QStringLiteral("unknown");
    props["success"] = telemetry.success;
    props["phase"] = telemetry.phase;
    if (telemetry.durationMs >= 0)
        props["duration_ms"] = telemetry.durationMs;
    if (!telemetry.failureCategory.isEmpty())
        props["failure_category"] = sanitizedErrorCategory(telemetry.failureCategory);
    if (telemetry.modelCount >= 0)
        props["model_count"] = telemetry.modelCount;
    if (telemetry.animationCount >= 0)
        props["animation_count"] = telemetry.animationCount;
    if (telemetry.approximateBytes >= 0)
        props["size_bucket"] = sizeBucket(telemetry.approximateBytes);
    captureTelemetryEvent(QStringLiteral("file.%1.%2").arg(telemetry.operation, telemetry.phase), props,
                          telemetry.success ? QStringLiteral("info") : QStringLiteral("error"));
}

QString SentryReporter::sanitizedValue(const QString &value)
{
    QString out = value;
    static const QRegularExpression pathRe(
        QStringLiteral(R"((?:[A-Za-z]:[\\/]|/|~[/\\])(?:[^\\/\s]+[\\/])*[^\\/\s]+)"));
    static const QRegularExpression fileRe(
        QStringLiteral(R"(\b[\w .-]+\.(fbx|obj|dae|gltf|glb|mesh|skeleton|png|jpg|jpeg|tga|hdr|exr|onnx|bin|json|txt|ply|stl|tmd|rsd)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    out.replace(pathRe, QStringLiteral("[redacted-path]"));
    out.replace(fileRe, QStringLiteral("[redacted-file]"));
    if (out.size() > 160)
        out = out.left(160) + QStringLiteral("...");
    return out;
}

QString SentryReporter::sanitizedErrorCategory(const QString &error)
{
    const QString e = error.toLower();
    if (e.contains("permission")) return QStringLiteral("permission");
    if (e.contains("not found") || e.contains("missing")) return QStringLiteral("not_found");
    if (e.contains("network") || e.contains("host") || e.contains("timeout")) return QStringLiteral("network");
    if (e.contains("parse") || e.contains("invalid")) return QStringLiteral("invalid_input");
    if (e.contains("download")) return QStringLiteral("download");
    if (e.contains("export")) return QStringLiteral("export");
    if (e.contains("import")) return QStringLiteral("import");
    if (e.contains("ogre") || e.contains("renderer")) return QStringLiteral("renderer");
    return QStringLiteral("other");
}

QString SentryReporter::extensionOnly(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix.isEmpty() ? QStringLiteral("unknown") : suffix;
}

QString SentryReporter::sizeBucket(qint64 bytes)
{
    if (bytes < 0) return QStringLiteral("unknown");
    if (bytes < 1024 * 1024) return QStringLiteral("<1mb");
    if (bytes < 10 * 1024 * 1024) return QStringLiteral("1-10mb");
    if (bytes < 100 * 1024 * 1024) return QStringLiteral("10-100mb");
    return QStringLiteral("100mb+");
}

bool SentryReporter::isKnownTelemetryEvent(const QString &eventName)
{
    static const QSet<QString> events = {
        QStringLiteral("app.startup"), QStringLiteral("app.shutdown"),
        QStringLiteral("file.import.started"), QStringLiteral("file.import.completed"),
        QStringLiteral("file.import.failed"), QStringLiteral("file.export.started"),
        QStringLiteral("file.export.completed"), QStringLiteral("file.export.failed"),
        QStringLiteral("cli.command.started"), QStringLiteral("cli.command.completed"),
        QStringLiteral("cli.command.failed"), QStringLiteral("mcp.tool.started"),
        QStringLiteral("mcp.tool.completed"), QStringLiteral("mcp.tool.failed"),
        QStringLiteral("ai.model_catalog.opened"), QStringLiteral("ai.model_download.started"),
        QStringLiteral("ai.model_download.completed"), QStringLiteral("ai.model_download.failed"),
        QStringLiteral("ai.model_download.canceled"), QStringLiteral("ai.model_delete.started"),
        QStringLiteral("ai.model_delete.completed"), QStringLiteral("ai.model_delete.failed"),
        QStringLiteral("ai.model_download_all.started"), QStringLiteral("ai.model_download_all.completed"),
        QStringLiteral("ai.feature_model_missing"),
        QStringLiteral("edit.mode.entered"), QStringLiteral("selection.mesh"),
        QStringLiteral("selection.bone"), QStringLiteral("transform.completed"),
        QStringLiteral("segmentation.started"), QStringLiteral("segmentation.completed"),
        QStringLiteral("segmentation.failed"), QStringLiteral("animation.played"),
        QStringLiteral("animation.exported")
    };
    return events.contains(eventName);
}

void SentryReporter::clearCapturedTelemetryEventsForTest()
{
#ifdef QTMESH_UNIT_TESTS
    s_capturedTelemetryEvents.clear();
#endif
    s_sessionId.clear();
    s_sessionStartedMs = 0;
}

QVector<SentryReporter::CapturedTelemetryEvent> SentryReporter::capturedTelemetryEventsForTest()
{
#ifdef QTMESH_UNIT_TESTS
    return s_capturedTelemetryEvents;
#else
    return {};
#endif
}

uintptr_t SentryReporter::startTransaction(const QString &name, const QString &op)
{
#ifdef ENABLE_SENTRY
    if (!s_initialized) return 0;

    sentry_transaction_context_t *txn_ctx =
        sentry_transaction_context_new(sanitizedValue(name).toUtf8().constData(),
                                       sanitizedValue(op).toUtf8().constData());
    sentry_transaction_t *txn = sentry_transaction_start(txn_ctx, sentry_value_new_null());
    return reinterpret_cast<uintptr_t>(txn);
#else
    Q_UNUSED(name);
    Q_UNUSED(op);
    return 0;
#endif
}

uintptr_t SentryReporter::startSpan(uintptr_t transaction, const QString &op,
                                    const QString &description)
{
#ifdef ENABLE_SENTRY
    if (!s_initialized || transaction == 0) return 0;

    auto *txn = reinterpret_cast<sentry_transaction_t *>(transaction);
    sentry_span_t *span = sentry_transaction_start_child(txn,
        sanitizedValue(op).toUtf8().constData(), sanitizedValue(description).toUtf8().constData());
    return reinterpret_cast<uintptr_t>(span);
#else
    Q_UNUSED(transaction);
    Q_UNUSED(op);
    Q_UNUSED(description);
    return 0;
#endif
}

void SentryReporter::finishSpan(uintptr_t span)
{
#ifdef ENABLE_SENTRY
    if (!s_initialized || span == 0) return;
    sentry_span_finish(reinterpret_cast<sentry_span_t *>(span));
#else
    Q_UNUSED(span);
#endif
}

void SentryReporter::finishTransaction(uintptr_t transaction)
{
#ifdef ENABLE_SENTRY
    if (!s_initialized || transaction == 0) return;
    sentry_transaction_finish(reinterpret_cast<sentry_transaction_t *>(transaction));
#else
    Q_UNUSED(transaction);
#endif
}
