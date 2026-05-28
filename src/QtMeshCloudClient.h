#ifndef QTMESH_CLOUD_CLIENT_H
#define QTMESH_CLOUD_CLIENT_H

#include <QString>
#include <QStringList>
#include <QJsonObject>

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
};

#endif
