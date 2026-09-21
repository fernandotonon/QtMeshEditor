#include <gtest/gtest.h>

#include "AppSettingsKeys.h"
#include "FeedbackPromptController.h"
#include "SentryReporter.h"

#include <QDateTime>
#include <QSettings>
#include <QSignalSpy>

// The prompt's decision logic is pure policy — no Ogre, no GL — so it is
// testable headlessly. These lock down the rules from #1058: one prompt per
// session, telemetry-gated, cooldown, and dismissal backoff.
class FeedbackPromptControllerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        QSettings settings;
        m_hadTelemetry = settings.contains(AppSettingsKeys::sentryEnabled());
        m_telemetry = settings.value(AppSettingsKeys::sentryEnabled(), true).toBool();
        settings.setValue(AppSettingsKeys::sentryEnabled(), true);
        settings.remove(AppSettingsKeys::feedbackLastPromptedAt());
        settings.remove(AppSettingsKeys::feedbackDismissCount());
        settings.remove(AppSettingsKeys::feedbackFirstImportDone());
        settings.remove(AppSettingsKeys::feedbackFirstExportDone());
        settings.setValue(AppSettingsKeys::feedbackPromptEnabled(), true);
        FeedbackPromptController::kill();
    }
    void TearDown() override
    {
        FeedbackPromptController::kill();
        QSettings settings;
        settings.remove(AppSettingsKeys::feedbackLastPromptedAt());
        settings.remove(AppSettingsKeys::feedbackDismissCount());
        settings.remove(AppSettingsKeys::feedbackFirstImportDone());
        settings.remove(AppSettingsKeys::feedbackFirstExportDone());
        settings.remove(AppSettingsKeys::feedbackPromptEnabled());
        if (m_hadTelemetry) settings.setValue(AppSettingsKeys::sentryEnabled(), m_telemetry);
        else settings.remove(AppSettingsKeys::sentryEnabled());
    }
    bool m_hadTelemetry = false;
    bool m_telemetry = true;
};

TEST_F(FeedbackPromptControllerTest, DisabledPreferenceBlocksPrompting)
{
    QSettings().setValue(AppSettingsKeys::feedbackPromptEnabled(), false);
    EXPECT_FALSE(FeedbackPromptController::instance()->canPrompt());
}

TEST_F(FeedbackPromptControllerTest, TelemetryOptOutBlocksPrompting)
{
    // No telemetry means no anonymous install id, so a response could not be
    // joined to that install's retention data — the only reason we ask.
    QSettings().setValue(AppSettingsKeys::sentryEnabled(), false);
    EXPECT_FALSE(FeedbackPromptController::instance()->canPrompt());
}

TEST_F(FeedbackPromptControllerTest, RecentPromptIsInCooldown)
{
    QSettings().setValue(AppSettingsKeys::feedbackLastPromptedAt(),
                         QDateTime::currentMSecsSinceEpoch());
    EXPECT_FALSE(FeedbackPromptController::instance()->canPrompt());
}

TEST_F(FeedbackPromptControllerTest, OldPromptIsOutOfCooldown)
{
    const qint64 longAgo = QDateTime::currentMSecsSinceEpoch()
        - qint64(FeedbackPromptController::kCooldownDays + 1) * 24 * 60 * 60 * 1000;
    QSettings().setValue(AppSettingsKeys::feedbackLastPromptedAt(), longAgo);
    EXPECT_TRUE(FeedbackPromptController::instance()->canPrompt());
}

TEST_F(FeedbackPromptControllerTest, RepeatedDismissalsStopPrompting)
{
    auto* ctrl = FeedbackPromptController::instance();
    for (int i = 0; i < FeedbackPromptController::kMaxDismissals; ++i)
        ctrl->reportDismissed();
    // Clear the cooldown so the ONLY thing under test is the dismissal budget.
    QSettings().remove(AppSettingsKeys::feedbackLastPromptedAt());
    EXPECT_FALSE(ctrl->canPrompt());
}

TEST_F(FeedbackPromptControllerTest, AnsweringResetsTheDismissalBudget)
{
    auto* ctrl = FeedbackPromptController::instance();
    ctrl->reportDismissed();
    ctrl->reportDismissed();
    ctrl->reportPositive();
    EXPECT_EQ(QSettings().value(AppSettingsKeys::feedbackDismissCount(), -1).toInt(), 0);
}

TEST_F(FeedbackPromptControllerTest, NoListenerDoesNotConsumeTheSessionBudget)
{
    // A headless CLI/MCP run has nothing connected; it must not burn the
    // one-per-session prompt (the GamificationManager::maybeRequestConsent
    // guard). With no listener, lastPromptedAt must stay unset.
    auto* ctrl = FeedbackPromptController::instance();
    ctrl->noteExport(true, QStringLiteral("glb"));
    EXPECT_FALSE(QSettings().contains(AppSettingsKeys::feedbackLastPromptedAt()));
}

TEST_F(FeedbackPromptControllerTest, FirstExportPromptsOnceThenNotAgain)
{
    auto* ctrl = FeedbackPromptController::instance();
    QSignalSpy spy(ctrl, &FeedbackPromptController::promptRequested);

    ctrl->noteExport(true, QStringLiteral("glb"));
    ASSERT_EQ(spy.count(), 1);

    // Second export in the same session: already prompted, stays quiet.
    ctrl->noteExport(true, QStringLiteral("fbx"));
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(FeedbackPromptControllerTest, FirstImportSuccessDoesNotPrompt)
{
    // A successful import is the START of the workflow; asking there would
    // interrupt someone who has not yet had the chance to succeed or fail.
    auto* ctrl = FeedbackPromptController::instance();
    QSignalSpy spy(ctrl, &FeedbackPromptController::promptRequested);
    ctrl->noteImport(true, QStringLiteral("fbx"));
    EXPECT_EQ(spy.count(), 0);
    EXPECT_TRUE(QSettings().value(AppSettingsKeys::feedbackFirstImportDone(), false).toBool());
}

TEST_F(FeedbackPromptControllerTest, ImportFailurePromptsWithPrefill)
{
    auto* ctrl = FeedbackPromptController::instance();
    QSignalSpy spy(ctrl, &FeedbackPromptController::promptRequested);

    ctrl->noteImport(false, QStringLiteral("fbx"), QStringLiteral("import_failed"));
    ASSERT_EQ(spy.count(), 1);

    const FeedbackPrefill prefill = ctrl->prefillForLastTrigger();
    EXPECT_EQ(prefill.type, QStringLiteral("import_problem"));
    EXPECT_EQ(prefill.relatedOperation, QStringLiteral("import"));
    EXPECT_EQ(prefill.relatedFormat, QStringLiteral("fbx"));
}

TEST_F(FeedbackPromptControllerTest, SessionTriggerNeedsAnImportAndNoExport)
{
    auto* ctrl = FeedbackPromptController::instance();
    QSignalSpy spy(ctrl, &FeedbackPromptController::promptRequested);

    // Nothing imported: an app left open on an empty scene is not churn.
    ctrl->evaluateSession();
    EXPECT_EQ(spy.count(), 0);

    // Imported but the session is still short — no nagging mid-work.
    ctrl->noteImport(true, QStringLiteral("fbx"));
    ctrl->evaluateSession();
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(FeedbackPromptControllerTest, TriggerTagsAreStable)
{
    // These land in telemetry as workflow_stage; renaming one silently breaks
    // every saved query built on it.
    using T = FeedbackPromptController::Trigger;
    EXPECT_EQ(FeedbackPromptController::triggerTag(T::FirstImport), QStringLiteral("first_import"));
    EXPECT_EQ(FeedbackPromptController::triggerTag(T::ImportFailure), QStringLiteral("import_failure"));
    EXPECT_EQ(FeedbackPromptController::triggerTag(T::FirstExport), QStringLiteral("first_export"));
    EXPECT_EQ(FeedbackPromptController::triggerTag(T::ExportFailure), QStringLiteral("export_failure"));
    EXPECT_EQ(FeedbackPromptController::triggerTag(T::SessionNoExport), QStringLiteral("session_no_export"));
}

// Positive answers must reach the DURABLE store too, not only Sentry —
// otherwise the feedback table collects complaints exclusively and cannot
// answer "did they finish?", which is half the churn question.
TEST_F(FeedbackPromptControllerTest, PositiveCarriesTheTriggerContext)
{
    auto* ctrl = FeedbackPromptController::instance();
    QSignalSpy spy(ctrl, &FeedbackPromptController::promptRequested);

    ctrl->noteExport(true, QStringLiteral("glb"));
    ASSERT_EQ(spy.count(), 1);

    // The silent positive post is built from this prefill, so the row carries
    // the moment that produced it rather than being context-free.
    const FeedbackPrefill prefill = ctrl->prefillForLastTrigger();
    EXPECT_EQ(prefill.relatedOperation, QStringLiteral("export"));
    EXPECT_EQ(prefill.relatedFormat, QStringLiteral("glb"));
    EXPECT_EQ(FeedbackPromptController::triggerTag(
                  FeedbackPromptController::Trigger::FirstExport),
              QStringLiteral("first_export"));

    // NB deliberately NOT calling reportPositive() here: it starts a detached
    // worker that POSTs to QTMESH_API_BASE, which defaults to the LIVE
    // https://api.qtmesh.dev — a unit test must never create real feedback
    // rows in production. The dismissal-budget reset is covered by
    // AnsweringResetsTheDismissalBudget, which does not reach the prompt (so
    // there is no frozen context and postSilentRating returns early).
}
