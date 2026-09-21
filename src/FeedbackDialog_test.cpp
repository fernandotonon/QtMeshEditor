#include "FeedbackDialog.h"
#include "SentryReporter.h"
#include "AppSettingsKeys.h"

#include "CloudCredentialStore.h"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <gtest/gtest.h>

class FeedbackDialogTest : public ::testing::Test {
protected:
    QString previousOrganizationName;
    QString previousApplicationName;

    void SetUp() override
    {
        previousOrganizationName = QCoreApplication::organizationName();
        previousApplicationName = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("QtMeshEditorTests"));
        QCoreApplication::setApplicationName(QStringLiteral("FeedbackDialogTest"));
        QSettings().clear();
        CloudCredentialStore::clearSession();
    }

    void TearDown() override
    {
        CloudCredentialStore::clearSession();
        QSettings().clear();
        QCoreApplication::setOrganizationName(previousOrganizationName);
        QCoreApplication::setApplicationName(previousApplicationName);
    }

    static QPlainTextEdit* messageEdit(FeedbackDialog& dialog)
    {
        return dialog.findChild<QPlainTextEdit*>(QStringLiteral("feedbackMessageEdit"));
    }
};

TEST_F(FeedbackDialogTest, RejectsEmptyMessageWhenSignedIn)
{
    FeedbackDialog dialog;
    dialog.setSignedInAccountLabel(QStringLiteral("Dev User"), true);
    EXPECT_FALSE(dialog.canSubmit());
}

TEST_F(FeedbackDialogTest, RejectsOverlongMessage)
{
    FeedbackDialog dialog;
    dialog.setSignedInAccountLabel(QStringLiteral("Dev User"), true);
    auto* edit = messageEdit(dialog);
    ASSERT_NE(edit, nullptr);
    edit->setPlainText(QString(FeedbackDialog::kMaxMessageLength + 1, QLatin1Char('x')));
    QApplication::processEvents();
    EXPECT_FALSE(dialog.canSubmit());
}

// #1058 changed this contract: a signed-out user with telemetry enabled has
// an anonymous installation id, which the server accepts as an identity, so
// they CAN submit. Only a user with neither identity is blocked.
TEST_F(FeedbackDialogTest, LoggedOutWithoutInstallIdCannotSubmit)
{
    QSettings settings;
    const bool had = settings.contains(AppSettingsKeys::sentryEnabled());
    const bool prev = settings.value(AppSettingsKeys::sentryEnabled(), true).toBool();
    // Telemetry off => anonymousInstallationId() is empty => no identity.
    settings.setValue(AppSettingsKeys::sentryEnabled(), false);

    FeedbackDialog dialog;
    dialog.setSignedInAccountLabel({}, false);
    auto* edit = messageEdit(dialog);
    ASSERT_NE(edit, nullptr);
    edit->setPlainText(QStringLiteral("Something broke"));
    QApplication::processEvents();
    EXPECT_FALSE(dialog.canSubmit());

    if (had) settings.setValue(AppSettingsKeys::sentryEnabled(), prev);
    else settings.remove(AppSettingsKeys::sentryEnabled());
}

TEST_F(FeedbackDialogTest, LoggedOutWithInstallIdCanSubmitAnonymously)
{
    QSettings settings;
    const bool had = settings.contains(AppSettingsKeys::sentryEnabled());
    const bool prev = settings.value(AppSettingsKeys::sentryEnabled(), true).toBool();
    settings.setValue(AppSettingsKeys::sentryEnabled(), true);
    // Mint the id (lazily created on first read) so the dialog sees one.
    const QString installId = SentryReporter::anonymousInstallationId();

    FeedbackDialog dialog;
    dialog.setSignedInAccountLabel({}, false);
    auto* edit = messageEdit(dialog);
    ASSERT_NE(edit, nullptr);
    edit->setPlainText(QStringLiteral("Something broke"));
    QApplication::processEvents();
    // Guard: in a build without telemetry support there is no id, and the
    // signed-out user genuinely cannot submit.
    if (!installId.isEmpty())
        EXPECT_TRUE(dialog.canSubmit());

    if (had) settings.setValue(AppSettingsKeys::sentryEnabled(), prev);
    else settings.remove(AppSettingsKeys::sentryEnabled());
}

TEST_F(FeedbackDialogTest, PrefillSetsImportProblemType)
{
    FeedbackDialog dialog;
    FeedbackPrefill prefill;
    prefill.type = QStringLiteral("import_problem");
    prefill.relatedOperation = QStringLiteral("import");
    prefill.relatedFormat = QStringLiteral("fbx");
    prefill.errorMessage = QStringLiteral("Parse failed");
    dialog.applyPrefill(prefill);
    EXPECT_EQ(dialog.selectedType(), QStringLiteral("import_problem"));
}

TEST_F(FeedbackDialogTest, SignInRequestedCallbackFires)
{
    FeedbackDialog dialog;
    dialog.setSignedInAccountLabel({}, false);
    bool signInCalled = false;
    dialog.signInRequested = [&signInCalled]() { signInCalled = true; };

    auto* signInButton = dialog.findChild<QPushButton*>(QStringLiteral("feedbackSignInButton"));
    ASSERT_NE(signInButton, nullptr);
    signInButton->click();
    QApplication::processEvents();
    EXPECT_TRUE(signInCalled);
}

TEST_F(FeedbackDialogTest, SignedInWithMessageCanSubmit)
{
    FeedbackDialog dialog;
    dialog.setSignedInAccountLabel(QStringLiteral("Dev User"), true);
    auto* edit = messageEdit(dialog);
    ASSERT_NE(edit, nullptr);
    edit->setPlainText(QStringLiteral("Great editor, thanks!"));
    QApplication::processEvents();
    EXPECT_TRUE(dialog.canSubmit());
}

TEST_F(FeedbackDialogTest, CheckboxDefaultsAreCheckedWhenNoSavedPrefs)
{
    FeedbackDialog dialog;
    EXPECT_TRUE(dialog.includeDiagnosticsChecked());
    EXPECT_TRUE(dialog.contactAllowedChecked());
}

TEST_F(FeedbackDialogTest, RestoresCheckboxPrefsFromSettings)
{
    {
        QSettings settings;
        settings.setValue(QStringLiteral("Feedback/includeDiagnostics"), false);
        settings.setValue(QStringLiteral("Feedback/contactAllowed"), true);
    }
    FeedbackDialog dialog;
    EXPECT_FALSE(dialog.includeDiagnosticsChecked());
    EXPECT_TRUE(dialog.contactAllowedChecked());
}

TEST_F(FeedbackDialogTest, PersistsCheckboxChangesToSettings)
{
    FeedbackDialog dialog;
    auto* diagnostics =
        dialog.findChild<QCheckBox*>(QStringLiteral("feedbackIncludeDiagnosticsCheck"));
    auto* contact = dialog.findChild<QCheckBox*>(QStringLiteral("feedbackContactAllowedCheck"));
    ASSERT_NE(diagnostics, nullptr);
    ASSERT_NE(contact, nullptr);
    diagnostics->setChecked(false);
    contact->setChecked(false);
    QApplication::processEvents();

    QSettings settings;
    EXPECT_FALSE(settings.value(QStringLiteral("Feedback/includeDiagnostics")).toBool());
    EXPECT_FALSE(settings.value(QStringLiteral("Feedback/contactAllowed")).toBool());

    FeedbackDialog dialogAgain;
    EXPECT_FALSE(dialogAgain.includeDiagnosticsChecked());
    EXPECT_FALSE(dialogAgain.contactAllowedChecked());
}
