#ifndef FEEDBACK_DIALOG_H
#define FEEDBACK_DIALOG_H

#include "FeedbackPrefill.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <functional>

/// Lightweight Help → Send Feedback dialog (#701).
class FeedbackDialog : public QDialog {
    Q_OBJECT

public:
    static constexpr int kMaxMessageLength = 4000;

    explicit FeedbackDialog(QWidget* parent = nullptr);

    void applyPrefill(const FeedbackPrefill& prefill);
    void setSignedInAccountLabel(const QString& accountLabel, bool signedIn);

    /// Called when the user needs to sign in before sending (v1: no anonymous submit).
    std::function<void()> signInRequested;

    /// Exposed for unit tests.
    QString selectedType() const;
    QString messageText() const;
    bool includeDiagnosticsChecked() const;
    bool contactAllowedChecked() const;
    bool canSubmit() const;

public slots:
    void accept() override;

private slots:
    void updateSubmitEnabled();
    void onSendClicked();

private:
    void buildUi();
    QString selectedRatingApiValue() const;
    QString copyableFeedbackText() const;
    void showSendFailure(const QString& userMessage, int httpStatus = 0);

    QComboBox* m_typeCombo = nullptr;
    QComboBox* m_ratingCombo = nullptr;
    QPlainTextEdit* m_messageEdit = nullptr;
    QCheckBox* m_diagnosticsCheck = nullptr;
    QCheckBox* m_contactCheck = nullptr;
    QLabel* m_accountLabel = nullptr;
    QLabel* m_statusLabel = nullptr;
    QPushButton* m_sendButton = nullptr;
    QPushButton* m_signInButton = nullptr;
    QString m_relatedOperation;
    QString m_relatedFormat;
    bool m_signedIn = false;
};

#endif
