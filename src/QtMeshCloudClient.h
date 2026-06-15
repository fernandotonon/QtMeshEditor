#ifndef QTMESH_CLOUD_CLIENT_H
#define QTMESH_CLOUD_CLIENT_H

#include <QString>
#include <QStringList>
#include <QJsonObject>

#include <atomic>

/// HTTP client for QtMesh Cloud ingest API (remote rules + scan upload).
class QtMeshCloudClient {
public:
    QtMeshCloudClient() = delete;

    /// Base URL without trailing slash. Override with env `QTMESH_API_BASE` (e.g. for tests).
    static QString apiBaseUrl();

    /// Minimal validation: JSON numeric `version` (not a string), object `scan`, object `rules`.
    static bool validateCloudConfigJson(const QJsonObject& root);

    struct RulesResult {
        bool ok = false;
        QString errorString;
        QJsonObject config;
        QString source;
    };

    /// GET /v1/ingest/rules — retries transient failures; returns parsed `config` object.
    static RulesResult fetchRules(const QString& bearerToken, int timeoutMs = 20000);

    struct UploadResult {
        bool ok = false;
        int httpStatus = 0;
        QString errorString;
        QString responseBodySnippet;
    };

    /// POST /v1/ingest/scan with compact JSON body (same schema as `qtmesh scan --json`).
    static UploadResult uploadScanReport(const QString& bearerToken, const QJsonObject& reportJson,
                                         int timeoutMs = 120000);

    struct DeviceCodeResult {
        bool ok = false;
        int httpStatus = 0;
        QString errorString;
        QString responseBodySnippet;
        QString deviceCode;
        QString userCode;
        QString verificationUri;
        QString verificationUriComplete;
        int expiresInSeconds = 0;
        int intervalSeconds = 5;
    };

    /// POST /v1/oauth/device/code — starts browser-based login for desktop clients.
    static DeviceCodeResult requestDeviceCode(const QString& clientName = QStringLiteral("QtMeshEditor"),
                                              int timeoutMs = 30000);

    struct DeviceTokenResult {
        bool ok = false;
        int httpStatus = 0;
        QString errorString;
        QString responseBodySnippet;
        QString errorCode;
        QString token;
        qint64 expiresAt = 0;
        QJsonObject user;
        int intervalSeconds = 5;
    };

    /// POST /v1/oauth/device/token — polls until the browser approval grants a session token.
    static DeviceTokenResult pollDeviceToken(const QString& deviceCode, int timeoutMs = 30000);

    struct CurrentUserResult {
        bool ok = false;
        int httpStatus = 0;
        QString errorString;
        QString responseBodySnippet;
        QJsonObject user;
    };

    /// GET /v1/auth/me — validates a stored session token and returns the current user.
    static CurrentUserResult fetchCurrentUser(const QString& bearerToken, int timeoutMs = 30000);

    /// POST /v1/auth/logout — best-effort session invalidation.
    static UploadResult logout(const QString& bearerToken, int timeoutMs = 30000);

    struct ProjectResult {
        bool ok = false;
        int httpStatus = 0;
        QString errorString;
        QString responseBodySnippet;
        QString ownerSlug;
        QString projectSlug;
        QString projectId;
        QString projectUrl;
    };

    struct ProjectSummary {
        QString id;
        QString ownerSlug;
        QString projectSlug;
        QString name;
        QString projectUrl;
    };

    struct ProjectsListResult {
        bool ok = false;
        int httpStatus = 0;
        QString errorString;
        QString responseBodySnippet;
        QList<ProjectSummary> projects;
    };

    /// GET /v1/projects — lists cloud projects the authenticated user can access.
    static ProjectsListResult fetchProjects(const QString& bearerToken, int timeoutMs = 30000);

    /// DELETE /v1/projects/{id} — removes a cloud project.
    static UploadResult deleteProject(const QString& bearerToken,
                                      const QString& projectId,
                                      int timeoutMs = 30000);

    /// POST /v1/projects — creates a private cloud project owned by the authenticated user.
    static ProjectResult createProject(const QString& bearerToken,
                                       const QString& name,
                                       const QString& slug,
                                       const QString& description = QString(),
                                       int timeoutMs = 30000);

    struct AssetFileDescriptor {
        QString path;
        QString uploadName;
        QString role;
        QString mimeType;
        qint64 sizeBytes = -1;
    };

    struct UploadTarget {
        QString fileId;
        QString uploadUrl;
        QString sanitizedName;
        QString role;
        QString extension;
        QString mimeType;
        qint64 sizeBytes = 0;
        qint64 expiresAt = 0;
    };

    struct UploadUrlsResult {
        bool ok = false;
        int httpStatus = 0;
        QString errorString;
        QString responseBodySnippet;
        QString uploadMethod;
        qint64 expiresAt = 0;
        QList<UploadTarget> uploads;
    };

    /// POST /v1/u/:owner/p/:project/files/upload-urls.
    static UploadUrlsResult requestUploadUrls(const QString& bearerToken,
                                              const QString& ownerSlug,
                                              const QString& projectSlug,
                                              const QList<AssetFileDescriptor>& files,
                                              int timeoutMs = 30000);

    struct FileUploadResult {
        bool ok = false;
        bool canceled = false;
        int httpStatus = 0;
        QString errorString;
        QString responseBodySnippet;
        QString fileId;
        qint64 sizeBytes = 0;
    };

    /// PUT binary content to an upload URL returned by requestUploadUrls().
    static FileUploadResult uploadFileContent(const QString& bearerToken,
                                              const UploadTarget& target,
                                              const QString& localPath,
                                              const std::atomic_bool* canceled = nullptr,
                                              int timeoutMs = 120000);

    struct CompleteUploadResult {
        bool ok = false;
        int httpStatus = 0;
        QString errorString;
        QString responseBodySnippet;
        QString scanStatus;
        QStringList fileIds;
    };

    /// POST /v1/u/:owner/p/:project/files/complete.
    static CompleteUploadResult completeUpload(const QString& bearerToken,
                                               const QString& ownerSlug,
                                               const QString& projectSlug,
                                               const QStringList& fileIds,
                                               const QString& mainFileId = QString(),
                                               int timeoutMs = 30000);

    struct ManifestResult {
        bool ok = false;
        int httpStatus = 0;
        QString errorString;
        QString responseBodySnippet;
        QJsonObject manifest;
    };

    /// GET /v1/u/:owner/p/:project/manifest — listing/download handoff foundation.
    static ManifestResult fetchProjectManifest(const QString& bearerToken,
                                               const QString& ownerSlug,
                                               const QString& projectSlug,
                                               int timeoutMs = 30000);

    struct FeedbackSubmission {
        QString type;
        QString rating;
        QString message;
        QString relatedOperation;
        QString relatedFormat;
        bool includeDiagnostics = false;
        QJsonObject diagnosticsJson;
        bool contactAllowed = false;
    };

    struct FeedbackResult {
        bool ok = false;
        int httpStatus = 0;
        QString errorString;
        QString userMessage;
        QString responseBodySnippet;
        QString id;
        QString status;
        QString createdAt;
    };

    static constexpr int kFeedbackMaxMessageLength = 4000;
    static constexpr QLatin1StringView kFeedbackApiPath{"/v1/feedback"};

    /// Normalize editor/API type strings (e.g. legacy `feature` → `feature_request`).
    static QString normalizeFeedbackType(const QString& type);

    /// Build POST /v1/feedback JSON body (also used by unit tests).
    static QJsonObject buildFeedbackPayload(const FeedbackSubmission& submission);

    /// Map API / transport failures to user-facing copy.
    static QString friendlyFeedbackError(int httpStatus,
                                         const QString& errorCode,
                                         const QString& fallback);

    /// POST /v1/feedback — authenticated in-app feedback (#701).
    static FeedbackResult submitFeedback(const QString& bearerToken,
                                         const FeedbackSubmission& submission,
                                         int timeoutMs = 30000);
};

#endif
