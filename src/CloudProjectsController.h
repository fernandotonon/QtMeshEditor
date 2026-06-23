#ifndef CLOUD_PROJECTS_CONTROLLER_H
#define CLOUD_PROJECTS_CONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QVariantList>

#include "QtMeshCloudClient.h"

class QtMeshCloudSession;

/// QML-facing controller for the My Cloud Projects dialog (#691).
class CloudProjectsController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QVariantList projects READ projects NOTIFY projectsChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool loadingMore READ loadingMore NOTIFY loadingChanged)
    Q_PROPERTY(bool hasMore READ hasMore NOTIFY hasMoreChanged)
    Q_PROPERTY(QString listError READ listError NOTIFY listErrorChanged)
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY signedInChanged)
    Q_PROPERTY(QVariantList projectFiles READ projectFiles NOTIFY projectFilesChanged)
    Q_PROPERTY(bool loadingProjectFiles READ loadingProjectFiles NOTIFY loadingProjectFilesChanged)
    Q_PROPERTY(bool viewingProjectFiles READ viewingProjectFiles NOTIFY activeProjectChanged)
    Q_PROPERTY(QString activeProjectName READ activeProjectName NOTIFY activeProjectChanged)
    Q_PROPERTY(QString activeProjectId READ activeProjectId NOTIFY activeProjectChanged)
    Q_PROPERTY(QString activeOwnerSlug READ activeOwnerSlug NOTIFY activeProjectChanged)
    Q_PROPERTY(QString activeProjectSlug READ activeProjectSlug NOTIFY activeProjectChanged)

public:
    static CloudProjectsController* instance();
    static CloudProjectsController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    QVariantList projects() const { return m_projects; }
    bool loading() const { return m_loading; }
    bool loadingMore() const { return m_loadingMore; }
    bool hasMore() const { return m_hasMore; }
    QString listError() const { return m_listError; }
    bool signedIn() const;
    QVariantList projectFiles() const { return m_projectFiles; }
    bool loadingProjectFiles() const { return m_loadingProjectFiles; }
    bool viewingProjectFiles() const { return m_viewingProjectFiles; }
    QString activeProjectName() const { return m_activeProjectName; }
    QString activeProjectId() const { return m_activeProjectId; }
    QString activeOwnerSlug() const { return m_activeOwnerSlug; }
    QString activeProjectSlug() const { return m_activeProjectSlug; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void deleteProject(const QString& projectId);
    Q_INVOKABLE void openInBrowser(const QString& projectId);
    Q_INVOKABLE void browseProjectFiles(const QString& projectId);
    Q_INVOKABLE void browseProjectBySlug(const QString& ownerSlug, const QString& projectSlug);
    Q_INVOKABLE void closeProjectFiles();
    Q_INVOKABLE void openProjectFile(const QString& fileId);
    Q_INVOKABLE void openInEditor(const QString& projectId);
    Q_INVOKABLE void openProjectBySlug(const QString& ownerSlug, const QString& projectSlug);
    Q_INVOKABLE void requestUpload();
    Q_INVOKABLE QString formatFileSize(qint64 bytes) const;
    Q_INVOKABLE QString formatUpdatedAt(const QString& isoTimestamp) const;
    Q_INVOKABLE QString formatIconForSource(const QString& sourceFormat) const;
    Q_INVOKABLE QString formatProjectSubtitle(const QVariant& project) const;
    Q_INVOKABLE QString formatFileRole(const QString& role) const;
    Q_INVOKABLE QString formatFileSubtitle(const QVariant& file) const;
    Q_INVOKABLE bool canOpenFile(const QVariant& file) const;

signals:
    void projectsChanged();
    void loadingChanged();
    void hasMoreChanged();
    void listErrorChanged();
    void signedInChanged();
    void projectFilesChanged();
    void loadingProjectFilesChanged();
    void activeProjectChanged();
    void browseProjectRequested();
    void signInRequired();
    void uploadRequested();
    void deleteFailed(const QString& projectId, const QString& error);
    void cloudProjectReady(const QString& localMainFile);
    void cloudOpenFailed(const QString& error);
    void cloudDownloadProgress(int current, int total, const QString& fileName);

private:
    explicit CloudProjectsController(QObject* parent = nullptr);

    void ensureSession();
    void appendProjects(const QVariantList& page);
    static QVariantMap projectToMap(const QtMeshCloudClient::ProjectSummary& project);
    QString browserUrlForProject(const QString& projectId) const;
    bool lookupProjectSlugs(const QString& projectId,
                            QString* ownerSlug,
                            QString* projectSlug,
                            QString* projectName = nullptr) const;
    void startDownloadBySlug(const QString& ownerSlug, const QString& projectSlug);
    void startFileDownload(const QString& fileId);
    void beginProjectFilesView(const QString& projectId,
                               const QString& ownerSlug,
                               const QString& projectSlug,
                               const QString& projectName);

    static CloudProjectsController* m_pSingleton;
    QtMeshCloudSession* m_session = nullptr;
    QVariantList m_projects;
    QString m_nextCursor;
    bool m_loading = false;
    bool m_loadingMore = false;
    bool m_hasMore = false;
    QString m_listError;
    bool m_downloading = false;

    QVariantList m_projectFiles;
    bool m_loadingProjectFiles = false;
    bool m_viewingProjectFiles = false;
    QString m_activeProjectId;
    QString m_activeProjectName;
    QString m_activeOwnerSlug;
    QString m_activeProjectSlug;
};

#endif // CLOUD_PROJECTS_CONTROLLER_H
