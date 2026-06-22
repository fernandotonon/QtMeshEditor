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

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void deleteProject(const QString& projectId);
    Q_INVOKABLE void openInBrowser(const QString& projectId);
    Q_INVOKABLE void requestUpload();
    Q_INVOKABLE QString formatFileSize(qint64 bytes) const;
    Q_INVOKABLE QString formatUpdatedAt(const QString& isoTimestamp) const;
    Q_INVOKABLE QString formatIconForSource(const QString& sourceFormat) const;

signals:
    void projectsChanged();
    void loadingChanged();
    void hasMoreChanged();
    void listErrorChanged();
    void signedInChanged();
    void signInRequired();
    void uploadRequested();
    void deleteFailed(const QString& projectId, const QString& error);

private:
    explicit CloudProjectsController(QObject* parent = nullptr);

    void ensureSession();
    void appendProjects(const QVariantList& page);
    static QVariantMap projectToMap(const QtMeshCloudClient::ProjectSummary& project);
    QString browserUrlForProject(const QString& projectId) const;

    static CloudProjectsController* m_pSingleton;
    QtMeshCloudSession* m_session = nullptr;
    QVariantList m_projects;
    QString m_nextCursor;
    bool m_loading = false;
    bool m_loadingMore = false;
    bool m_hasMore = false;
    QString m_listError;
};

#endif // CLOUD_PROJECTS_CONTROLLER_H
