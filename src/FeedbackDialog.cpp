#include "FeedbackDialog.h"

#include "CloudCredentialStore.h"
#include "FeedbackDiagnostics.h"
#include "QtMeshCloudClient.h"
#include "SentryReporter.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>

#ifndef QTMESH_CLOUD_WEB_URL
#define QTMESH_CLOUD_WEB_URL "https://qtmesh.dev"
#endif

namespace {

QString feedbackWebsiteUrl()
{
    QString base = QStringLiteral(QTMESH_CLOUD_WEB_URL);
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    return base + QStringLiteral("/#/feedback");
}

struct FeedbackTypeOption {
    const char* apiValue;
    const char* label;
};

constexpr FeedbackTypeOption kFeedbackTypes[] = {
    {"bug", QT_TRANSLATE_NOOP("FeedbackDialog", "Bug report")},
    {"feature_request", QT_TRANSLATE_NOOP("FeedbackDialog", "Feature request")},
    {"general", QT_TRANSLATE_NOOP("FeedbackDialog", "General feedback")},
    {"import_problem", QT_TRANSLATE_NOOP("FeedbackDialog", "Import problem")},
    {"export_problem", QT_TRANSLATE_NOOP("FeedbackDialog", "Export problem")},
};

int typeIndexForApiValue(const QString& apiValue)
{
    for (int i = 0; i < int(sizeof(kFeedbackTypes) / sizeof(kFeedbackTypes[0])); ++i) {
        if (apiValue == QLatin1String(kFeedbackTypes[i].apiValue))
            return i;
    }
    return 0;
}

constexpr QLatin1StringView kSettingsIncludeDiagnostics{"Feedback/includeDiagnostics"};
constexpr QLatin1StringView kSettingsContactAllowed{"Feedback/contactAllowed"};

void loadFeedbackCheckboxPrefs(QCheckBox* diagnosticsCheck, QCheckBox* contactCheck)
{
    QSettings settings;
    diagnosticsCheck->setChecked(settings.value(kSettingsIncludeDiagnostics, true).toBool());
    contactCheck->setChecked(settings.value(kSettingsContactAllowed, true).toBool());
}

void saveFeedbackCheckboxPrefs(bool includeDiagnostics, bool contactAllowed)
{
    QSettings settings;
    settings.setValue(kSettingsIncludeDiagnostics, includeDiagnostics);
    settings.setValue(kSettingsContactAllowed, contactAllowed);
}

} // namespace

FeedbackDialog::FeedbackDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Send Feedback"));
    setModal(true);
    setMinimumWidth(480);
    buildUi();
    setSignedInAccountLabel({}, CloudCredentialStore::hasSession());
    updateSubmitEnabled();
}

void FeedbackDialog::buildUi()
{
    auto* layout = new QVBoxLayout(this);

    m_accountLabel = new QLabel(this);
    m_accountLabel->setWordWrap(true);
    layout->addWidget(m_accountLabel);

    auto* form = new QFormLayout();
    m_typeCombo = new QComboBox(this);
    for (const FeedbackTypeOption& option : kFeedbackTypes)
        m_typeCombo->addItem(tr(option.label), QString::fromLatin1(option.apiValue));
    form->addRow(tr("Type:"), m_typeCombo);

    m_ratingCombo = new QComboBox(this);
    m_ratingCombo->addItem(tr("No rating"), QString());
    m_ratingCombo->addItem(tr("Great"), QStringLiteral("great"));
    m_ratingCombo->addItem(tr("Okay"), QStringLiteral("okay"));
    m_ratingCombo->addItem(tr("Problem"), QStringLiteral("problem"));
    form->addRow(tr("Rating (optional):"), m_ratingCombo);

    m_messageEdit = new QPlainTextEdit(this);
    m_messageEdit->setObjectName(QStringLiteral("feedbackMessageEdit"));
    m_messageEdit->setPlaceholderText(
        tr("Tell us what happened, what you expected, or what you'd like to see improved…"));
    m_messageEdit->setTabChangesFocus(true);
    form->addRow(tr("Message:"), m_messageEdit);
    layout->addLayout(form);

    m_diagnosticsCheck =
        new QCheckBox(tr("Include anonymous diagnostics (version, OS, locale, recent non-sensitive events)"),
                      this);
    m_diagnosticsCheck->setObjectName(QStringLiteral("feedbackIncludeDiagnosticsCheck"));
    m_contactCheck = new QCheckBox(tr("You may contact me about this feedback"), this);
    m_contactCheck->setObjectName(QStringLiteral("feedbackContactAllowedCheck"));
    loadFeedbackCheckboxPrefs(m_diagnosticsCheck, m_contactCheck);
    layout->addWidget(m_diagnosticsCheck);
    layout->addWidget(m_contactCheck);

    connect(m_diagnosticsCheck, &QCheckBox::toggled, this, [this](bool checked) {
        saveFeedbackCheckboxPrefs(checked, m_contactCheck->isChecked());
    });
    connect(m_contactCheck, &QCheckBox::toggled, this, [this](bool checked) {
        saveFeedbackCheckboxPrefs(m_diagnosticsCheck->isChecked(), checked);
    });

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->hide();
    layout->addWidget(m_statusLabel);

    auto* buttons = new QHBoxLayout();
    m_signInButton = new QPushButton(tr("Sign in to QtMesh Cloud…"), this);
    m_signInButton->setObjectName(QStringLiteral("feedbackSignInButton"));
    buttons->addWidget(m_signInButton);
    buttons->addStretch();
    auto* cancelButton = new QPushButton(tr("Cancel"), this);
    buttons->addWidget(cancelButton);
    m_sendButton = new QPushButton(tr("Send"), this);
    m_sendButton->setDefault(true);
    buttons->addWidget(m_sendButton);
    layout->addLayout(buttons);

    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_signInButton, &QPushButton::clicked, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Feedback dialog: Sign in requested"));
        if (signInRequested)
            signInRequested();
    });
    connect(m_sendButton, &QPushButton::clicked, this, &FeedbackDialog::onSendClicked);
    connect(m_typeCombo, &QComboBox::currentIndexChanged, this, &FeedbackDialog::updateSubmitEnabled);
    connect(m_messageEdit, &QPlainTextEdit::textChanged, this, &FeedbackDialog::updateSubmitEnabled);
}

void FeedbackDialog::applyPrefill(const FeedbackPrefill& prefill)
{
    if (!prefill.type.isEmpty())
        m_typeCombo->setCurrentIndex(typeIndexForApiValue(prefill.type));

    if (!prefill.errorMessage.isEmpty() || !prefill.relatedFormat.isEmpty()) {
        QString intro;
        if (!prefill.relatedFormat.isEmpty()) {
            intro = prefill.relatedOperation == QStringLiteral("export")
                ? tr("Export failed for %1.").arg(prefill.relatedFormat)
                : tr("Import failed for %1.").arg(prefill.relatedFormat);
        }
        if (!prefill.errorMessage.isEmpty()) {
            if (!intro.isEmpty())
                intro += QLatin1Char('\n');
            intro += prefill.errorMessage;
        }
        if (!intro.isEmpty() && m_messageEdit->toPlainText().trimmed().isEmpty())
            m_messageEdit->setPlainText(intro);
    }

    if (!prefill.relatedOperation.isEmpty())
        m_relatedOperation = prefill.relatedOperation;
    if (!prefill.relatedFormat.isEmpty())
        m_relatedFormat = prefill.relatedFormat;

    if (!prefill.relatedOperation.isEmpty() || !prefill.errorMessage.isEmpty()) {
        FeedbackDiagnostics::setOperationContext(prefill.relatedOperation,
                                                 prefill.relatedFormat,
                                                 prefill.errorCode,
                                                 prefill.errorMessage);
    }
}

void FeedbackDialog::setSignedInAccountLabel(const QString& accountLabel, bool signedIn)
{
    m_signedIn = signedIn;
    if (signedIn) {
        const QString label = accountLabel.trimmed().isEmpty()
            ? tr("Signed in to QtMesh Cloud")
            : tr("Sending as %1").arg(accountLabel.trimmed());
        m_accountLabel->setText(label);
        m_signInButton->hide();
    } else {
        m_accountLabel->setText(
            tr("Sign in to QtMesh Cloud to send feedback. Anonymous submissions are not available yet."));
        m_signInButton->show();
    }
    updateSubmitEnabled();
}

QString FeedbackDialog::selectedType() const
{
    return m_typeCombo->currentData().toString();
}

QString FeedbackDialog::messageText() const
{
    return m_messageEdit->toPlainText();
}

bool FeedbackDialog::includeDiagnosticsChecked() const
{
    return m_diagnosticsCheck->isChecked();
}

bool FeedbackDialog::contactAllowedChecked() const
{
    return m_contactCheck->isChecked();
}

bool FeedbackDialog::canSubmit() const
{
    return m_signedIn && !selectedType().isEmpty() && !messageText().trimmed().isEmpty()
        && messageText().size() <= kMaxMessageLength;
}

void FeedbackDialog::updateSubmitEnabled()
{
    const int length = messageText().size();
    if (length > kMaxMessageLength) {
        m_statusLabel->setText(tr("Message is too long (%1 / %2 characters).")
                                   .arg(length)
                                   .arg(kMaxMessageLength));
        m_statusLabel->setStyleSheet(QStringLiteral("color: #c0392b;"));
        m_statusLabel->show();
    } else {
        m_statusLabel->hide();
    }
    m_sendButton->setEnabled(canSubmit());
}

QString FeedbackDialog::selectedRatingApiValue() const
{
    return m_ratingCombo->currentData().toString();
}

QString FeedbackDialog::copyableFeedbackText() const
{
    return tr("Type: %1\nRating: %2\n\n%3")
        .arg(m_typeCombo->currentText(),
             m_ratingCombo->currentText(),
             messageText().trimmed());
}

void FeedbackDialog::onSendClicked()
{
    if (!canSubmit()) {
        if (!m_signedIn) {
            QMessageBox::information(this, tr("Sign in required"),
                                     tr("Sign in to QtMesh Cloud before sending feedback."));
        }
        return;
    }

    const CloudSession session = CloudCredentialStore::loadSession();
    if (!session.hasToken()) {
        setSignedInAccountLabel({}, false);
        QMessageBox::information(this, tr("Sign in required"),
                                 tr("Your QtMesh Cloud session is missing. Sign in and try again."));
        return;
    }

    QtMeshCloudClient::FeedbackSubmission submission;
    submission.type = selectedType();
    submission.rating = selectedRatingApiValue();
    submission.message = messageText().trimmed();
    submission.relatedOperation = m_relatedOperation;
    submission.relatedFormat = m_relatedFormat;
    submission.includeDiagnostics = includeDiagnosticsChecked();
    submission.contactAllowed = contactAllowedChecked();
    if (submission.includeDiagnostics)
        submission.diagnosticsJson = FeedbackDiagnostics::collectDiagnostics(true);

    m_sendButton->setEnabled(false);
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Feedback dialog: Send clicked type=%1")
                                      .arg(submission.type));

    const auto result = QtMeshCloudClient::submitFeedback(session.token, submission);
    m_sendButton->setEnabled(true);

    if (result.ok) {
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.feedback"),
                                      QStringLiteral("Feedback sent id=%1").arg(result.id));
        QMessageBox::information(
            this, tr("Thank you"),
            tr("Thanks — your feedback was sent to the QtMesh team."));
        accept();
        return;
    }

    showSendFailure(result.userMessage.isEmpty() ? result.errorString : result.userMessage,
                    result.httpStatus);
}

void FeedbackDialog::showSendFailure(const QString& userMessage, int httpStatus)
{
    Q_UNUSED(httpStatus);

    const QString websiteUrl = feedbackWebsiteUrl();
    QMessageBox failureBox(this);
    failureBox.setIcon(QMessageBox::Warning);
    failureBox.setWindowTitle(tr("Could not send feedback"));
    failureBox.setText(userMessage);
    failureBox.setInformativeText(
        tr("Your message is still in this dialog.\n\n"
           "You can send the same feedback on the QtMesh Cloud website while signed in:\n"
           "%1\n\n"
           "Use \"Copy Message\" to paste your text there, or \"Open Website\" to open the form.")
            .arg(websiteUrl));
    QPushButton* websiteButton = failureBox.addButton(tr("Open Website"), QMessageBox::ActionRole);
    QPushButton* copyButton = failureBox.addButton(tr("Copy Message"), QMessageBox::ActionRole);
    failureBox.addButton(QMessageBox::Ok);
    failureBox.setDefaultButton(copyButton);
    failureBox.exec();

    if (failureBox.clickedButton() == websiteButton) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Feedback failure: Open Website"));
        if (!QDesktopServices::openUrl(QUrl(websiteUrl))) {
            QMessageBox::warning(this, tr("QtMesh Cloud"),
                                 tr("Could not open %1 in your browser.").arg(websiteUrl));
        }
    } else if (failureBox.clickedButton() == copyButton && QApplication::clipboard()) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Feedback failure: Copy Message"));
        QApplication::clipboard()->setText(copyableFeedbackText());
    }
}

void FeedbackDialog::accept()
{
    saveFeedbackCheckboxPrefs(includeDiagnosticsChecked(), contactAllowedChecked());
    QDialog::accept();
}
