/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "FeedbackPromptController.h"

#include "AppSettingsKeys.h"
#include "CloudCredentialStore.h"
#include "QtMeshCloudClient.h"
#include "SentryReporter.h"

#include <QDateTime>
#include <QJsonObject>
#include <QMetaMethod>
#include <QSettings>
#include <QThread>

namespace {

constexpr qint64 kMsPerDay = 24LL * 60LL * 60LL * 1000LL;

} // namespace

FeedbackPromptController* FeedbackPromptController::s_instance = nullptr;

FeedbackPromptController* FeedbackPromptController::instance()
{
    if (!s_instance) s_instance = new FeedbackPromptController();
    return s_instance;
}

void FeedbackPromptController::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

FeedbackPromptController::FeedbackPromptController(QObject* parent) : QObject(parent)
{
    m_sessionTimer.start();
}

FeedbackPromptController::~FeedbackPromptController() = default;

QString FeedbackPromptController::triggerTag(Trigger t)
{
    switch (t) {
    case Trigger::FirstImport:     return QStringLiteral("first_import");
    case Trigger::ImportFailure:   return QStringLiteral("import_failure");
    case Trigger::FirstExport:     return QStringLiteral("first_export");
    case Trigger::ExportFailure:   return QStringLiteral("export_failure");
    case Trigger::SessionNoExport: return QStringLiteral("session_no_export");
    }
    return QStringLiteral("unknown");
}

qint64 FeedbackPromptController::sessionSeconds() const
{
    return m_sessionTimer.isValid() ? m_sessionTimer.elapsed() / 1000 : 0;
}

bool FeedbackPromptController::canPrompt() const
{
    if (m_promptedThisSession) return false;

    QSettings settings;
    if (!settings.value(AppSettingsKeys::feedbackPromptEnabled(), true).toBool())
        return false;

    // Prompts are identified by the anonymous installation id, which only
    // exists when telemetry is on. Without it a response cannot be joined to
    // the install's retention data — which is the only reason we are asking.
    if (!SentryReporter::isEnabled()) return false;
    if (SentryReporter::anonymousInstallationId().isEmpty()) return false;

    // Someone who has waved us away repeatedly is answering the question by
    // not answering it. Stop asking.
    if (settings.value(AppSettingsKeys::feedbackDismissCount(), 0).toInt() >= kMaxDismissals)
        return false;

    const qint64 last = settings.value(AppSettingsKeys::feedbackLastPromptedAt(), 0).toLongLong();
    if (last > 0) {
        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - last;
        if (elapsed < kCooldownDays * kMsPerDay) return false;
    }
    return true;
}

void FeedbackPromptController::maybePrompt(Trigger trigger)
{
    if (!canPrompt()) return;

    // Only burn the one-per-session budget when something can actually show
    // the prompt. A headless CLI/MCP run has no listener and must not consume
    // it — the same guard GamificationManager::maybeRequestConsent uses.
    if (!isSignalConnected(QMetaMethod::fromSignal(&FeedbackPromptController::promptRequested)))
        return;

    m_promptedThisSession = true;
    m_lastTrigger = trigger;

    QSettings settings;
    settings.setValue(AppSettingsKeys::feedbackLastPromptedAt(),
                      QDateTime::currentMSecsSinceEpoch());

    recordLifecycleEvent(QStringLiteral("feedback.prompt_shown"));
    SentryReporter::addBreadcrumb(QStringLiteral("ui.feedback"),
        QStringLiteral("prompt shown (%1)").arg(triggerTag(trigger)));

    emit promptRequested(trigger);
}

void FeedbackPromptController::noteImport(bool ok, const QString& format, const QString& errorCode)
{
    m_lastFormat = format;
    m_lastErrorCode = errorCode;

    if (!ok) {
        maybePrompt(Trigger::ImportFailure);
        return;
    }

    m_importedThisSession = true;

    QSettings settings;
    const bool firstEver = !settings.value(AppSettingsKeys::feedbackFirstImportDone(), false).toBool();
    if (firstEver) {
        settings.setValue(AppSettingsKeys::feedbackFirstImportDone(), true);
        // Deliberately do NOT prompt here. A successful first import is the
        // start of the workflow, not the end of it — asking now would
        // interrupt someone who has not yet had the chance to succeed or fail
        // at what they actually came to do.
    }
}

void FeedbackPromptController::noteExport(bool ok, const QString& format, const QString& errorCode)
{
    m_lastFormat = format;
    m_lastErrorCode = errorCode;

    if (!ok) {
        maybePrompt(Trigger::ExportFailure);
        return;
    }

    m_exportedThisSession = true;

    QSettings settings;
    const bool firstEver = !settings.value(AppSettingsKeys::feedbackFirstExportDone(), false).toBool();
    if (firstEver) {
        settings.setValue(AppSettingsKeys::feedbackFirstExportDone(), true);
        // A completed first export is the clearest "did this work for you?"
        // moment there is, and it is the one case where a positive answer is
        // genuinely informative rather than polite.
        maybePrompt(Trigger::FirstExport);
    }
}

void FeedbackPromptController::evaluateSession()
{
    // The churn-relevant case: they opened something, worked for a while, and
    // are leaving without producing anything. Requires an import so we do not
    // prompt someone who merely left the app open on an empty scene.
    if (!m_importedThisSession) return;
    if (m_exportedThisSession) return;
    if (sessionSeconds() < kSessionNoExportMinutes * 60) return;
    maybePrompt(Trigger::SessionNoExport);
}

void FeedbackPromptController::postSilentRating(const QString& rating,
                                                const QString& workflowStage,
                                                const QString& relatedOperation,
                                                const QString& relatedFormat)
{
    const CloudSession session = CloudCredentialStore::loadSession();
    const QString installId = SentryReporter::anonymousInstallationId();
    // No identity means no joinable row; the server would 401 anyway.
    if (!session.hasToken() && installId.isEmpty()) return;

    QtMeshCloudClient::FeedbackSubmission submission;
    submission.type = QStringLiteral("general");
    submission.rating = rating;
    // The server requires a non-empty message. There is no user text on this
    // path by design, so record the moment instead — that is the datum: which
    // workflow stage produced this outcome.
    submission.message = QStringLiteral("[prompt] %1 at %2")
                             .arg(rating, workflowStage);
    submission.relatedOperation = relatedOperation;
    submission.relatedFormat = relatedFormat;
    submission.includeDiagnostics = false;
    submission.contactAllowed = false;
    if (!session.hasToken())
        submission.anonymousInstallationId = installId;

    const QString token = session.token;
    // submitFeedback blocks on a QEventLoop, so it must not run on the UI
    // thread. Detached worker, result deliberately ignored — a failed POST is
    // invisible to the user and must never interrupt editing (#1058).
    QThread* worker = QThread::create([token, submission]() {
        QtMeshCloudClient::submitFeedback(token, submission);
    });
    QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void FeedbackPromptController::reportPositive()
{
    QSettings settings;
    settings.setValue(AppSettingsKeys::feedbackDismissCount(), 0);
    recordLifecycleEvent(QStringLiteral("feedback.positive"));
    SentryReporter::addBreadcrumb(QStringLiteral("ui.feedback"), QStringLiteral("positive"));

    // "Got what I needed" never opens the detailed dialog, so without this it
    // would exist only as a Sentry event. Persist it too, or the durable store
    // collects complaints exclusively and cannot answer "did they finish?".
    const FeedbackPrefill prefill = prefillForLastTrigger();
    postSilentRating(QStringLiteral("great"), triggerTag(m_lastTrigger),
                     prefill.relatedOperation, prefill.relatedFormat);
}

void FeedbackPromptController::reportNegative()
{
    QSettings settings;
    settings.setValue(AppSettingsKeys::feedbackDismissCount(), 0);
    recordLifecycleEvent(QStringLiteral("feedback.negative"));
    SentryReporter::addBreadcrumb(QStringLiteral("ui.feedback"), QStringLiteral("negative"));
}

void FeedbackPromptController::reportDismissed()
{
    QSettings settings;
    const int count = settings.value(AppSettingsKeys::feedbackDismissCount(), 0).toInt();
    settings.setValue(AppSettingsKeys::feedbackDismissCount(), count + 1);
    recordLifecycleEvent(QStringLiteral("feedback.dismissed"));
    SentryReporter::addBreadcrumb(QStringLiteral("ui.feedback"), QStringLiteral("dismissed"));
}

void FeedbackPromptController::reportSubmitted(const QString& category)
{
    recordLifecycleEvent(QStringLiteral("feedback.submitted"), category);
    SentryReporter::addBreadcrumb(QStringLiteral("ui.feedback"),
        QStringLiteral("submitted (%1)").arg(category.isEmpty() ? QStringLiteral("no category")
                                                                : category));
}

FeedbackPrefill FeedbackPromptController::prefillForLastTrigger() const
{
    FeedbackPrefill prefill;
    prefill.relatedFormat = m_lastFormat;
    prefill.errorCode = m_lastErrorCode;

    switch (m_lastTrigger) {
    case Trigger::ImportFailure:
        prefill.type = QStringLiteral("import_problem");
        prefill.relatedOperation = QStringLiteral("import");
        break;
    case Trigger::ExportFailure:
        prefill.type = QStringLiteral("export_problem");
        prefill.relatedOperation = QStringLiteral("export");
        break;
    case Trigger::FirstImport:
        prefill.type = QStringLiteral("general");
        prefill.relatedOperation = QStringLiteral("import");
        break;
    case Trigger::FirstExport:
        prefill.type = QStringLiteral("general");
        prefill.relatedOperation = QStringLiteral("export");
        break;
    case Trigger::SessionNoExport:
        prefill.type = QStringLiteral("general");
        // No related operation: the defining feature of this trigger is that
        // the user did not reach one.
        break;
    }
    return prefill;
}

void FeedbackPromptController::recordLifecycleEvent(const QString& event,
                                                    const QString& category) const
{
    // Non-sensitive context only. Never a filename, path, or anything read
    // out of the user's model — see the privacy section of #1058.
    QJsonObject props;
    props.insert(QStringLiteral("workflow_stage"), triggerTag(m_lastTrigger));
    props.insert(QStringLiteral("session_seconds"), static_cast<double>(sessionSeconds()));
    props.insert(QStringLiteral("imported_this_session"), m_importedThisSession);
    props.insert(QStringLiteral("exported_this_session"), m_exportedThisSession);
    if (!m_lastFormat.isEmpty())
        props.insert(QStringLiteral("file_format"), m_lastFormat);
    if (!m_lastErrorCode.isEmpty())
        props.insert(QStringLiteral("error_category"),
                     SentryReporter::sanitizedErrorCategory(m_lastErrorCode));
    if (!category.isEmpty())
        props.insert(QStringLiteral("feedback_category"), category);

    SentryReporter::captureTelemetryEvent(event, props);
}
