#include "FeedbackReportHelper.h"

#include "FeedbackDiagnostics.h"
#include "SentryReporter.h"

#include <QAbstractButton>
#include <QMessageBox>
#include <QPushButton>

namespace {

FeedbackReportHelper::OpenFeedbackHandler g_openFeedbackHandler;

} // namespace

void FeedbackReportHelper::setOpenFeedbackHandler(OpenFeedbackHandler handler)
{
    g_openFeedbackHandler = std::move(handler);
}

bool FeedbackReportHelper::showFailureWithReportOption(QWidget* parent,
                                                         const QString& title,
                                                         const QString& text,
                                                         const FeedbackPrefill& prefill)
{
    FeedbackDiagnostics::setOperationContext(prefill.relatedOperation,
                                             prefill.relatedFormat,
                                             prefill.errorCode,
                                             prefill.errorMessage);

    QMessageBox box(parent);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(title);
    box.setText(text);
    box.setStandardButtons(QMessageBox::Ok);
    QPushButton* reportButton =
        box.addButton(QObject::tr("Report Problem…"), QMessageBox::ActionRole);
    if (auto* okButton = qobject_cast<QPushButton*>(box.button(QMessageBox::Ok)))
        box.setDefaultButton(okButton);

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Import/export failure shown: %1").arg(title));

    box.exec();
    if (box.clickedButton() == reportButton) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Report Problem chosen after failure"));
        if (g_openFeedbackHandler)
            g_openFeedbackHandler(prefill);
        return true;
    }
    return false;
}

FeedbackPrefill FeedbackReportHelper::importFailurePrefill(const QString& format,
                                                           const QString& errorMessage,
                                                           const QString& errorCode)
{
    FeedbackPrefill prefill;
    prefill.type = QStringLiteral("import_problem");
    prefill.relatedOperation = QStringLiteral("import");
    prefill.relatedFormat = format;
    prefill.errorCode = errorCode;
    prefill.errorMessage = errorMessage;
    return prefill;
}

FeedbackPrefill FeedbackReportHelper::exportFailurePrefill(const QString& format,
                                                           const QString& errorMessage,
                                                           const QString& errorCode)
{
    FeedbackPrefill prefill;
    prefill.type = QStringLiteral("export_problem");
    prefill.relatedOperation = QStringLiteral("export");
    prefill.relatedFormat = format;
    prefill.errorCode = errorCode;
    prefill.errorMessage = errorMessage;
    return prefill;
}

void FeedbackReportHelper::resetForTests()
{
    g_openFeedbackHandler = {};
}
