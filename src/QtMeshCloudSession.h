#ifndef QTMESH_CLOUD_SESSION_H
#define QTMESH_CLOUD_SESSION_H

#include "ProjectPackager.h"
#include "QtMeshCloudClient.h"

#include <QObject>
#include <atomic>
#include <memory>

struct CloudPackageUploadRequest {
    QString mainAssetPath;
    QStringList selectedAbsolutePaths;
    QString projectName;
    QString ownerSlug;
    QString projectSlug;
    bool createNewProject = false;
    bool runLocalScan = false;
};

/// Async, progress-reporting QtMesh Cloud upload session (Slice C / #687).
class QtMeshCloudSession : public QObject {
    Q_OBJECT

public:
    explicit QtMeshCloudSession(const QString& bearerToken, QObject* parent = nullptr);

    QString bearerToken() const { return m_bearerToken; }

    /// Lists projects for the authenticated user.
    void listProjects();

    /// Creates a project and uploads the manifest files.
    void uploadPackage(const PackageMetadata& metadata,
                       const QString& ownerSlug = QString(),
                       const QString& projectSlug = QString(),
                       bool createNewProject = true);

    /// Scans (optional), builds the manifest, and uploads on a worker thread.
    void uploadPackageFromAssets(const CloudPackageUploadRequest& request);

    void cancel();

signals:
    void projectsListed(const QList<QtMeshCloudClient::ProjectSummary>& projects, const QString& error);
    void uploadProgress(int current, int total, const QString& fileName);
    /// Non-fatal prep issues (e.g. scan failed) surfaced before/during upload.
    void uploadPrepareWarning(const QString& warning);
    /// When ok is true, error may carry a non-fatal report-upload warning.
    void uploadFinished(bool ok,
                        const QString& error,
                        const QString& projectUrl,
                        const QString& scanStatus);
    void uploadCanceled();

private:
    void startUploadWorker(const PackageMetadata& package,
                           const QString& ownerSlug,
                           const QString& projectSlug,
                           bool createNewProject);

    QString m_bearerToken;
    std::atomic_bool m_canceled{false};
    std::shared_ptr<std::atomic_bool> m_uploadCancelFlag;
};

#endif // QTMESH_CLOUD_SESSION_H
