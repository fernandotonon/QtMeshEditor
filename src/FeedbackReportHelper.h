#ifndef FEEDBACK_REPORT_HELPER_H
#define FEEDBACK_REPORT_HELPER_H

#include "FeedbackPrefill.h"

#include <functional>

class QWidget;

/// Shows import/export failures with an optional "Report Problem…" affordance (#701).
class FeedbackReportHelper {
public:
    FeedbackReportHelper() = delete;

    using OpenFeedbackHandler = std::function<void(const FeedbackPrefill&)>;

    static void setOpenFeedbackHandler(OpenFeedbackHandler handler);

    /// Warning dialog with OK + optional Report Problem. Returns true if report was chosen.
    static bool showFailureWithReportOption(QWidget* parent,
                                            const QString& title,
                                            const QString& text,
                                            const FeedbackPrefill& prefill);

    static FeedbackPrefill importFailurePrefill(const QString& format,
                                                const QString& errorMessage,
                                                const QString& errorCode = QString());
    static FeedbackPrefill exportFailurePrefill(const QString& format,
                                                const QString& errorMessage,
                                                const QString& errorCode = QString());

    static void resetForTests();
};

#endif
