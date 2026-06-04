#ifndef FEEDBACK_PREFILL_H
#define FEEDBACK_PREFILL_H

#include <QString>

/// Optional values used to pre-populate the feedback dialog (e.g. after a failed import).
struct FeedbackPrefill {
    QString type;
    QString relatedOperation;
    QString relatedFormat;
    QString errorCode;
    QString errorMessage;
};

#endif
