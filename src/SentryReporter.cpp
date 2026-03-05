#include "SentryReporter.h"
#include <QSettings>
#include <QMessageBox>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDir>
#include <QDebug>

#ifdef ENABLE_SENTRY
#include <sentry.h>
#endif

bool SentryReporter::s_initialized = false;

void SentryReporter::initialize()
{
#ifdef ENABLE_SENTRY
    if (s_initialized) return;
    if (!isEnabled()) return;

    sentry_options_t *options = sentry_options_new();
    sentry_options_set_dsn(options,
        "https://51f59f98ea62a3b7523ce52de2b2387d@o4509454943322112.ingest.us.sentry.io/4510896994254848");

    // Set database path for crash report persistence
    QString dbPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                     + "/sentry-db";
    QDir().mkpath(dbPath);
    QByteArray dbPathUtf8 = dbPath.toUtf8();
    sentry_options_set_database_path(options, dbPathUtf8.constData());

    // Set release version (keep QByteArray alive for constData pointer)
    QByteArray release = QStringLiteral("qtmesheditor@%1")
        .arg(QCoreApplication::applicationVersion()).toUtf8();
    sentry_options_set_release(options, release.constData());

    // Sample rate for performance transactions (1.0 = 100%)
    sentry_options_set_traces_sample_rate(options, 1.0);

    int result = sentry_init(options);
    if (result == 0) {
        s_initialized = true;
        qDebug() << "Sentry initialized successfully (db:" << dbPath << ")";
    } else {
        qWarning() << "Sentry initialization failed with code:" << result;
    }
#endif
}

void SentryReporter::shutdown()
{
#ifdef ENABLE_SENTRY
    if (!s_initialized) return;
    sentry_close();
    s_initialized = false;
#endif
}

bool SentryReporter::isEnabled()
{
    QSettings settings;
    return settings.value("Sentry/enabled", true).toBool();
}

void SentryReporter::setEnabled(bool enabled)
{
    QSettings settings;
    settings.setValue("Sentry/enabled", enabled);
}

bool SentryReporter::isFirstLaunch()
{
    QSettings settings;
    return !settings.contains("Sentry/enabled");
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

void SentryReporter::setTag(const QString &key, const QString &value)
{
#ifdef ENABLE_SENTRY
    if (!s_initialized) return;
    sentry_set_tag(key.toUtf8().constData(), value.toUtf8().constData());
#else
    Q_UNUSED(key);
    Q_UNUSED(value);
#endif
}

void SentryReporter::addBreadcrumb(const QString &category, const QString &message,
                                   const QString &level)
{
#ifdef ENABLE_SENTRY
    if (!s_initialized) return;

    sentry_value_t crumb = sentry_value_new_breadcrumb("default",
        message.toUtf8().constData());
    sentry_value_set_by_key(crumb, "category",
        sentry_value_new_string(category.toUtf8().constData()));
    sentry_value_set_by_key(crumb, "level",
        sentry_value_new_string(level.toUtf8().constData()));
    sentry_add_breadcrumb(crumb);
#else
    Q_UNUSED(category);
    Q_UNUSED(message);
    Q_UNUSED(level);
#endif
}

void SentryReporter::captureMessage(const QString &message, const QString &level)
{
#ifdef ENABLE_SENTRY
    if (!s_initialized) return;

    sentry_level_t sentryLevel = SENTRY_LEVEL_INFO;
    if (level == "warning") sentryLevel = SENTRY_LEVEL_WARNING;
    else if (level == "error") sentryLevel = SENTRY_LEVEL_ERROR;
    else if (level == "fatal") sentryLevel = SENTRY_LEVEL_FATAL;
    else if (level == "debug") sentryLevel = SENTRY_LEVEL_DEBUG;

    sentry_value_t event = sentry_value_new_message_event(sentryLevel,
        "qtmesheditor", message.toUtf8().constData());
    sentry_capture_event(event);
#else
    Q_UNUSED(message);
    Q_UNUSED(level);
#endif
}

uintptr_t SentryReporter::startTransaction(const QString &name, const QString &op)
{
#ifdef ENABLE_SENTRY
    if (!s_initialized) return 0;

    sentry_transaction_context_t *txn_ctx =
        sentry_transaction_context_new(name.toUtf8().constData(),
                                       op.toUtf8().constData());
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
        op.toUtf8().constData(), description.toUtf8().constData());
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
