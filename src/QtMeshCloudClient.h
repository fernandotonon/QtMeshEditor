#ifndef QTMESH_CLOUD_CLIENT_H
#define QTMESH_CLOUD_CLIENT_H

#include <QString>
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
};

#endif
