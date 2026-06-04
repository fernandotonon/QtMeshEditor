#ifndef FEEDBACK_DIAGNOSTICS_H
#define FEEDBACK_DIAGNOSTICS_H

#include <QJsonObject>
#include <QString>

/// Privacy-safe diagnostics collection and redaction for in-app feedback (#701).
class FeedbackDiagnostics {
public:
    FeedbackDiagnostics() = delete;

    static constexpr int kMaxDiagnosticsJsonBytes = 8192;
    static constexpr int kMaxRecentEvents = 20;

    /// Stable per-process UUID (non-PII).
    static QString editorSessionId();

    /// Record a non-sensitive app event for optional diagnostics attachment.
    static void recordRecentEvent(const QString& category, const QString& message);

    /// Remember the last import/export failure context for diagnostics / prefill.
    static void setOperationContext(const QString& operation,
                                    const QString& format,
                                    const QString& errorCode,
                                    const QString& errorMessage);
    static void clearOperationContext();

    /// Redact a path-like string to filename + extension only.
    static QString redactPath(const QString& value);

    /// Redact tokens, signed URLs, absolute paths, and env-var-like secrets from free text.
    static QString redactString(const QString& value);

    /// Deep-redact a JSON value (paths, secrets, oversized strings).
    static QJsonValue redactJsonValue(const QJsonValue& value);

    /// Build bounded diagnostics JSON suitable for `diagnosticsJson` when opted in.
    static QJsonObject collectDiagnostics(bool includeOperationContext = true);

    /// Serialize diagnostics and enforce the byte-size cap.
    static QString diagnosticsJsonString(const QJsonObject& diagnostics);

    /// Clear in-memory state (tests).
    static void resetForTests();
};

#endif
