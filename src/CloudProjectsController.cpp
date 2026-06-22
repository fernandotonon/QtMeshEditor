#include "CloudProjectsController.h"

#include "CloudCredentialStore.h"
#include "QtMeshCloudSession.h"
#include "SentryReporter.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QLocale>
#include <QUrl>

CloudProjectsController* CloudProjectsController::m_pSingleton = nullptr;

CloudProjectsController* CloudProjectsController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new CloudProjectsController();
    return m_pSingleton;
}

CloudProjectsController* CloudProjectsController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void CloudProjectsController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

CloudProjectsController::CloudProjectsController(QObject* parent)
    : QObject(parent)
{
}

bool CloudProjectsController::signedIn() const
{
    return CloudCredentialStore::hasSession()
        && !CloudCredentialStore::loadSession().token.isEmpty();
}

void CloudProjectsController::ensureSession()
{
    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    const QString token = CloudCredentialStore::loadSession().token;
    if (!m_session || m_session->bearerToken() != token) {
        if (m_session)
            m_session->deleteLater();
        m_session = new QtMeshCloudSession(token, this);

        connect(m_session, &QtMeshCloudSession::projectsListed, this,
                [this](const QList<QtMeshCloudClient::ProjectSummary>& page,
                       const QString& error,
                       const QString& nextCursor,
                       bool hasMore) {
                    const bool wasLoadingMore = m_loadingMore;
                    m_loading = false;
                    m_loadingMore = false;
                    emit loadingChanged();

                    if (!error.isEmpty()) {
                        m_listError = error;
                        emit listErrorChanged();
                        if (error.contains(QStringLiteral("unauthorized"), Qt::CaseInsensitive)) {
                            emit signInRequired();
                        }
                        return;
                    }

                    m_listError.clear();
                    emit listErrorChanged();
                    m_nextCursor = nextCursor;
                    m_hasMore = hasMore;
                    emit hasMoreChanged();

                    QVariantList mapped;
                    for (const auto& project : page)
                        mapped.append(projectToMap(project));
                    if (wasLoadingMore) {
                        appendProjects(mapped);
                    } else {
                        m_projects = mapped;
                        emit projectsChanged();
                    }

                    SentryReporter::addBreadcrumb(
                        QStringLiteral("cloud.projects.list"),
                        QStringLiteral("count=%1").arg(m_projects.size()));
                });

        connect(m_session, &QtMeshCloudSession::projectDeleted, this,
                [this](const QString& projectId, const QString& error) {
                    if (!error.isEmpty()) {
                        emit deleteFailed(projectId, error);
                        return;
                    }

                    SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.delete"),
                                                  QStringLiteral("projectId=%1").arg(projectId));

                    QVariantList remaining;
                    for (const QVariant& value : std::as_const(m_projects)) {
                        const QVariantMap map = value.toMap();
                        if (map.value(QStringLiteral("id")).toString() != projectId)
                            remaining.append(map);
                    }
                    m_projects = remaining;
                    emit projectsChanged();
                });
    }
}

void CloudProjectsController::refresh()
{
    if (!signedIn()) {
        emit signInRequired();
        return;
    }

    m_projects.clear();
    emit projectsChanged();
    m_nextCursor.clear();
    m_hasMore = false;
    emit hasMoreChanged();
    m_listError.clear();
    emit listErrorChanged();

    ensureSession();
    m_loading = true;
    m_loadingMore = false;
    emit loadingChanged();
    m_session->listProjects();
}

void CloudProjectsController::loadMore()
{
    if (!signedIn() || m_loading || m_loadingMore || !m_hasMore || m_nextCursor.isEmpty())
        return;

    ensureSession();
    m_loadingMore = true;
    emit loadingChanged();
    m_session->listProjects(m_nextCursor);
}

void CloudProjectsController::openInBrowser(const QString& projectId)
{
    const QString url = browserUrlForProject(projectId);
    if (url.isEmpty())
        return;
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.open_in_browser"),
                                  QStringLiteral("projectId=%1").arg(projectId));
    QDesktopServices::openUrl(QUrl(url));
}

void CloudProjectsController::requestUpload()
{
    emit uploadRequested();
}

void CloudProjectsController::deleteProject(const QString& projectId)
{
    if (projectId.trimmed().isEmpty())
        return;
    if (!signedIn()) {
        emit signInRequired();
        return;
    }
    ensureSession();
    m_session->deleteProject(projectId.trimmed());
}

QString CloudProjectsController::formatFileSize(qint64 bytes) const
{
    if (bytes <= 0)
        return QStringLiteral("—");
    return QLocale().formattedDataSize(bytes);
}

QString CloudProjectsController::formatUpdatedAt(const QString& isoTimestamp) const
{
    if (isoTimestamp.isEmpty())
        return QStringLiteral("—");
    const QDateTime parsed = QDateTime::fromString(isoTimestamp, Qt::ISODate);
    if (!parsed.isValid())
        return isoTimestamp;
    return QLocale().toString(parsed.toLocalTime(), QLocale::ShortFormat);
}

QString CloudProjectsController::formatIconForSource(const QString& sourceFormat) const
{
    const QString fmt = sourceFormat.trimmed().toLower();
    if (fmt.isEmpty())
        return QStringLiteral("📦");
    if (fmt == QStringLiteral("fbx"))
        return QStringLiteral("🎬");
    if (fmt == QStringLiteral("gltf") || fmt == QStringLiteral("glb") || fmt == QStringLiteral("gltf2"))
        return QStringLiteral("✨");
    if (fmt == QStringLiteral("obj"))
        return QStringLiteral("📐");
    if (fmt == QStringLiteral("dae"))
        return QStringLiteral("🌐");
    if (fmt == QStringLiteral("stl") || fmt == QStringLiteral("ply"))
        return QStringLiteral("🖨");
    return QStringLiteral("📦");
}

void CloudProjectsController::appendProjects(const QVariantList& page)
{
    for (const QVariant& value : page)
        m_projects.append(value);
    emit projectsChanged();
}

QVariantMap CloudProjectsController::projectToMap(const QtMeshCloudClient::ProjectSummary& project)
{
    QVariantMap map;
    map.insert(QStringLiteral("id"), project.id);
    map.insert(QStringLiteral("name"),
               project.name.isEmpty() ? project.projectSlug : project.name);
    map.insert(QStringLiteral("ownerSlug"), project.ownerSlug);
    map.insert(QStringLiteral("projectSlug"), project.projectSlug);
    map.insert(QStringLiteral("projectUrl"), project.projectUrl);
    map.insert(QStringLiteral("browserUrl"),
               project.browserUrl.isEmpty()
                   ? QStringLiteral("https://qtmesh.dev/projects/%1").arg(project.id)
                   : project.browserUrl);
    map.insert(QStringLiteral("sourceFormat"), project.sourceFormat);
    map.insert(QStringLiteral("sizeBytes"), project.sizeBytes);
    map.insert(QStringLiteral("updatedAt"), project.updatedAt);
    map.insert(QStringLiteral("mainFile"), project.mainFile);
    return map;
}

QString CloudProjectsController::browserUrlForProject(const QString& projectId) const
{
    for (const QVariant& value : m_projects) {
        const QVariantMap map = value.toMap();
        if (map.value(QStringLiteral("id")).toString() == projectId)
            return map.value(QStringLiteral("browserUrl")).toString();
    }
    return QString();
}
