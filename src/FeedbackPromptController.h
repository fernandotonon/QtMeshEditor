/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef FEEDBACKPROMPTCONTROLLER_H
#define FEEDBACKPROMPTCONTROLLER_H

#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include "FeedbackPrefill.h"

/**
 * @brief Decides when to show a lightweight contextual feedback prompt (#1058).
 *
 * The product question this exists to answer: users open the editor once and
 * never return. Retention data cannot distinguish "they got what they came
 * for" from "something didn't work" — those look identical, and they call for
 * opposite responses. So the prompt's job is to SEPARATE those two, not merely
 * to detect failure.
 *
 * Design constraints, all from the issue:
 *
 *  - At most ONE prompt per session, ever.
 *  - Never blocks or interrupts editing (the prompt is non-modal).
 *  - Dismissals back off and eventually stop asking.
 *  - Nothing here may make an import/export fail; every entry point is a
 *    no-op-on-error observer.
 *
 * Identity is the anonymous installation id the telemetry pipeline already
 * tags, so feedback joins against that install's activation/retention data.
 * That is the entire point, and it is why prompts are gated on telemetry
 * consent: without it there is no id, and an unjoinable comment does not
 * answer the question being asked.
 *
 * This class only DECIDES and records. It does not build UI — it emits
 * `promptRequested` and MainWindow presents it, mirroring how
 * GamificationManager::consentPromptRequested works. That split also means a
 * headless CLI/MCP process, where nothing is connected to the signal, silently
 * never prompts.
 */
class FeedbackPromptController : public QObject
{
    Q_OBJECT

public:
    /// Why the prompt is being shown. Recorded as the `workflow_stage` tag so
    /// responses can be split by the moment that produced them.
    enum class Trigger {
        FirstImport,        ///< First ever successful import on this install.
        ImportFailure,      ///< An import failed.
        FirstExport,        ///< First ever successful export on this install.
        ExportFailure,      ///< An export failed.
        SessionNoExport,    ///< Long session, imported something, never exported.
    };
    Q_ENUM(Trigger)

    static FeedbackPromptController* instance();
    static void kill();

    /// Session-duration threshold for the SessionNoExport trigger. Kept
    /// generous: a prompt that fires while someone is still working reads as
    /// nagging, and the users being studied are the ones who leave.
    static constexpr int kSessionNoExportMinutes = 12;

    /// How long after a dismissal before asking again, and how many
    /// consecutive dismissals before giving up on this install entirely.
    static constexpr int kCooldownDays = 14;
    static constexpr int kMaxDismissals = 3;

    // ---- Observers. All are no-ops unless a prompt is actually warranted. --

    /// An import finished. `ok == false` means it failed.
    void noteImport(bool ok, const QString& format, const QString& errorCode = {});
    /// An export finished. `ok == false` means it failed.
    void noteExport(bool ok, const QString& format, const QString& errorCode = {});
    /// Called periodically (and on quit) to evaluate the session trigger.
    void evaluateSession();

    // ---- Outcomes, reported back by whoever presented the prompt. ----------

    /// The user said they got what they needed. Records and closes the loop —
    /// this is the "achieved their goal" answer to the churn question.
    void reportPositive();
    /// The user said something didn't work; the detailed dialog follows.
    void reportNegative();
    /// Dismissed without answering.
    void reportDismissed();
    /// A detailed submission was actually sent.
    void reportSubmitted(const QString& category);

    /// True when prompts are permitted at all: enabled in Preferences, the
    /// user has consented to telemetry (so there is an install id to join on),
    /// none shown yet this session, and the cooldown/dismissal budget allows.
    bool canPrompt() const;

    /// Prefill for the detailed dialog, derived from the trigger that fired.
    FeedbackPrefill prefillForLastTrigger() const;

    /// Human-readable tag for a trigger, used in telemetry.
    static QString triggerTag(Trigger t);

signals:
    /// Ask the GUI to present the prompt. Not emitted when nothing is
    /// connected, so headless runs never consume the one-per-session budget.
    void promptRequested(FeedbackPromptController::Trigger trigger);

private:
    explicit FeedbackPromptController(QObject* parent = nullptr);
    ~FeedbackPromptController() override;

    /// Emit the prompt if allowed, marking the session + persisting the time.
    void maybePrompt(Trigger trigger);

    /// Fire-and-forget POST of a short, message-less submission so EVERY
    /// outcome lands in the durable store — not just the negative ones that
    /// go through the detailed dialog. Without this the feedback table would
    /// hold only complaints, and "they finished happily" would live solely in
    /// Sentry on shorter retention — exactly the half of the churn question
    /// we most need to keep.
    ///
    /// Runs on a detached worker (submitFeedback blocks) and ignores the
    /// result: feedback must never interrupt editing or surface an error.
    static void postSilentRating(const QString& rating,
                                 const QString& workflowStage,
                                 const QString& relatedOperation,
                                 const QString& relatedFormat);
    void recordLifecycleEvent(const QString& event, const QString& category = {}) const;
    qint64 sessionSeconds() const;

    static FeedbackPromptController* s_instance;

    bool          m_promptedThisSession = false;
    bool          m_importedThisSession = false;
    bool          m_exportedThisSession = false;
    Trigger       m_lastTrigger = Trigger::SessionNoExport;
    QString       m_lastFormat;
    QString       m_lastErrorCode;
    /// Context FROZEN when the prompt was emitted. The prompt is non-modal,
    /// so a later import/export would otherwise overwrite m_lastFormat /
    /// m_lastErrorCode while m_lastTrigger still named the original moment —
    /// producing a prefill that mixes the two.
    Trigger       m_promptTrigger = Trigger::SessionNoExport;
    QString       m_promptFormat;
    QString       m_promptErrorCode;
    QElapsedTimer m_sessionTimer;
};

#endif // FEEDBACKPROMPTCONTROLLER_H
