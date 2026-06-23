#ifndef QTMESH_CLOUD_SESSION_H
#define QTMESH_CLOUD_SESSION_H

#include "ProjectPackager.h"
#include "QtMeshCloudClient.h"

#include <QObject>
#include <atomic>

/// Async, progress-reporting QtMesh Cloud upload session (Slice C / #687).
class QtMeshCloudSession : public QObject {
    Q_OBJECT

public:
    explicit QtMeshCloudSession(const QString& bearerToken, QObject* parent = nullptr);

    QString bearerToken() const { return m_bearerToken; }
    void setBearerToken(const QString& token) { m_bearerToken = token; }

    /// Lists projects for the authenticated user (first page when @p cursor is empty).
    void listProjects(const QString& cursor = QString());

    /// DELETE /v1/projects/{id} on a worker thread.
    void deleteProject(const QString& projectId);

    /// Stub for listing dialog — prefer downloadProjectBySlug().
    void downloadProject(const QString& projectId, const QString& destDir);

    /// Downloads a cloud project by owner/project slug on a worker thread.
    void downloadProjectBySlug(const QString& ownerSlug,
                               const QString& projectSlug,
                               const QString& destDir = QString());

    /// Fetches manifest file listing on a worker thread.
    void fetchProjectFiles(const QString& ownerSlug, const QString& projectSlug);

    /// Downloads and opens one manifest file (plus rendering companions).
    void downloadProjectFile(const QString& ownerSlug,
                             const QString& projectSlug,
                             const QString& fileId,
                             const QString& destDir = QString());

    /// Creates a project and uploads the manifest files.
    void uploadPackage(const PackageMetadata& metadata,
                       const QString& ownerSlug = QString(),
                       const QString& projectSlug = QString(),
                       bool createNewProject = true);

    void cancel();

signals:
    void projectsListed(const QList<QtMeshCloudClient::ProjectSummary>& projects,
                        const QString& error,
                        const QString& nextCursor,
                        bool hasMore);
    void projectDeleted(const QString& projectId, const QString& error);
    void projectFilesFetched(const QVariantList& files, const QString& error);
    void downloadProgress(int current, int total, const QString& fileName);
    void downloadComplete(bool ok, const QString& message, const QString& detail);
    void uploadProgress(int current, int total, const QString& fileName);
    void uploadFinished(bool ok,
                        const QString& error,
                        const QString& projectUrl,
                        const QString& scanStatus);
    void uploadCanceled();

private:
    QString m_bearerToken;
    std::atomic_bool m_canceled{false};
};

#endif // QTMESH_CLOUD_SESSION_H
