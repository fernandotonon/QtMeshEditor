#include "CloudProjectsController.h"

#include "CloudCredentialStore.h"
#include "QtMeshCloudSession.h"
#include "SentryReporter.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QLocale>
#include <QTimeZone>
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

                    if (m_activeProjectId == projectId)
                        closeProjectFiles();
                });

        connect(m_session, &QtMeshCloudSession::projectFilesFetched, this,
                [this](const QVariantList& files, const QString& error) {
                    m_loadingProjectFiles = false;
                    emit loadingProjectFilesChanged();

                    if (!error.isEmpty()) {
                        m_projectFiles.clear();
                        emit projectFilesChanged();
                        emit cloudOpenFailed(error.isEmpty()
                                                 ? QStringLiteral("Could not load project files.")
                                                 : error);
                        return;
                    }

                    m_projectFiles = files;
                    emit projectFilesChanged();
                    SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.files"),
                                                  QStringLiteral("count=%1").arg(files.size()));
                });

        connect(m_session, &QtMeshCloudSession::downloadProgress, this,
                [this](int current, int total, const QString& fileName) {
                    emit cloudDownloadProgress(current, total, fileName);
                });
        connect(m_session, &QtMeshCloudSession::downloadComplete, this,
                [this](bool ok, const QString& message, const QString& detail) {
                    m_downloading = false;
                    if (ok) {
                        SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.open_in_editor"),
                                                        QStringLiteral("import %1").arg(message));
                        emit cloudProjectReady(message);
                        return;
                    }
                    const QString error = message.isEmpty() ? detail : message;
                    emit cloudOpenFailed(error.isEmpty()
                                             ? QStringLiteral("Could not download project from QtMesh Cloud.")
                                             : error);
                });
    }
}

void CloudProjectsController::refresh()
{
    if (!signedIn()) {
        emit signInRequired();
        return;
    }

    closeProjectFiles();
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

void CloudProjectsController::browseProjectFiles(const QString& projectId)
{
    QString ownerSlug;
    QString projectSlug;
    QString projectName;
    if (!lookupProjectSlugs(projectId, &ownerSlug, &projectSlug, &projectName))
        return;
    if (!signedIn()) {
        emit signInRequired();
        return;
    }

    beginProjectFilesView(projectId, ownerSlug, projectSlug, projectName);
    emit browseProjectRequested();
}

void CloudProjectsController::browseProjectBySlug(const QString& ownerSlug, const QString& projectSlug)
{
    if (ownerSlug.trimmed().isEmpty() || projectSlug.trimmed().isEmpty())
        return;
    if (!signedIn()) {
        emit signInRequired();
        return;
    }

    const QString owner = ownerSlug.trimmed();
    const QString slug = projectSlug.trimmed();
    beginProjectFilesView(QString(), owner, slug, QStringLiteral("%1/%2").arg(owner, slug));
    emit browseProjectRequested();
}

void CloudProjectsController::closeProjectFiles()
{
    const bool hadView = m_viewingProjectFiles || !m_projectFiles.isEmpty() || m_loadingProjectFiles;
    m_viewingProjectFiles = false;
    m_loadingProjectFiles = false;
    m_activeProjectId.clear();
    m_activeProjectName.clear();
    m_activeOwnerSlug.clear();
    m_activeProjectSlug.clear();
    m_projectFiles.clear();
    if (hadView) {
        emit loadingProjectFilesChanged();
        emit projectFilesChanged();
        emit activeProjectChanged();
    }
}

void CloudProjectsController::openProjectFile(const QString& fileId)
{
    if (fileId.trimmed().isEmpty())
        return;
    if (!signedIn()) {
        emit signInRequired();
        return;
    }
    if (m_activeOwnerSlug.isEmpty() || m_activeProjectSlug.isEmpty())
        return;
    if (m_downloading)
        return;

    for (const QVariant& value : std::as_const(m_projectFiles)) {
        const QVariantMap map = value.toMap();
        if (map.value(QStringLiteral("id")).toString() != fileId)
            continue;
        if (!canOpenFile(map)) {
            emit cloudOpenFailed(QStringLiteral("This file type cannot be opened in the editor."));
            return;
        }
        break;
    }

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.open_file"),
                                  QStringLiteral("%1/%2 file=%3")
                                      .arg(m_activeOwnerSlug, m_activeProjectSlug, fileId));
    startFileDownload(fileId.trimmed());
}

void CloudProjectsController::openInEditor(const QString& projectId)
{
    browseProjectFiles(projectId);
}

void CloudProjectsController::openProjectBySlug(const QString& ownerSlug, const QString& projectSlug)
{
    browseProjectBySlug(ownerSlug, projectSlug);
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
    QDateTime parsed = QDateTime::fromString(isoTimestamp, Qt::ISODate);
    if (!parsed.isValid())
        parsed = QDateTime::fromString(isoTimestamp, Qt::ISODateWithMs);
    if (!parsed.isValid()) {
        parsed = QDateTime::fromString(isoTimestamp, QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        if (parsed.isValid())
            parsed.setTimeZone(QTimeZone::UTC);
    }
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

QString CloudProjectsController::formatProjectSubtitle(const QVariant& project) const
{
    const QVariantMap map = project.toMap();
    const QString format = map.value(QStringLiteral("sourceFormat")).toString();
    const qint64 sizeBytes = static_cast<qint64>(map.value(QStringLiteral("sizeBytes")).toDouble());
    const QString updatedAt = map.value(QStringLiteral("updatedAt")).toString();
    if (!format.isEmpty() || sizeBytes > 0 || !updatedAt.isEmpty()) {
        return QStringLiteral("%1 · %2 · %3")
            .arg(format.isEmpty() ? QStringLiteral("asset") : format.toUpper(),
                 formatFileSize(sizeBytes),
                 formatUpdatedAt(updatedAt));
    }

    const QString owner = map.value(QStringLiteral("ownerSlug")).toString();
    const QString slug = map.value(QStringLiteral("projectSlug")).toString();
    if (!owner.isEmpty() && !slug.isEmpty())
        return QStringLiteral("%1/%2").arg(owner, slug);
    return QString();
}

QString CloudProjectsController::formatFileRole(const QString& role) const
{
    const QString normalized = role.trimmed().toLower();
    if (normalized == QStringLiteral("model") || normalized == QStringLiteral("main"))
        return QStringLiteral("Model");
    if (normalized == QStringLiteral("texture"))
        return QStringLiteral("Texture");
    if (normalized == QStringLiteral("material"))
        return QStringLiteral("Material");
    if (normalized == QStringLiteral("animation"))
        return QStringLiteral("Animation");
    if (normalized == QStringLiteral("skeleton"))
        return QStringLiteral("Skeleton");
    if (normalized == QStringLiteral("metadata"))
        return QStringLiteral("Metadata");
    if (normalized == QStringLiteral("sidecar"))
        return QStringLiteral("Sidecar");
    if (normalized.isEmpty())
        return QStringLiteral("File");
    return normalized.at(0).toUpper() + normalized.mid(1);
}

QString CloudProjectsController::formatFileSubtitle(const QVariant& file) const
{
    const QVariantMap map = file.toMap();
    const QString role = formatFileRole(map.value(QStringLiteral("role")).toString());
    const qint64 sizeBytes = static_cast<qint64>(map.value(QStringLiteral("sizeBytes")).toDouble());
    const QString extension = map.value(QStringLiteral("extension")).toString();
    QStringList parts;
    parts << role;
    if (!extension.isEmpty())
        parts << extension.toUpper();
    parts << formatFileSize(sizeBytes);
    return parts.join(QStringLiteral(" · "));
}

bool CloudProjectsController::canOpenFile(const QVariant& file) const
{
    const QVariantMap map = file.toMap();
    if (map.contains(QStringLiteral("canOpen")))
        return map.value(QStringLiteral("canOpen")).toBool();
    const QString name = map.value(QStringLiteral("originalName")).toString();
    return QtMeshCloudClient::isImportableCloudAssetPath(name);
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
               !project.browserUrl.isEmpty()
                   ? project.browserUrl
                   : QtMeshCloudClient::projectDashboardUrl(project.ownerSlug,
                                                            project.projectSlug));
    map.insert(QStringLiteral("sourceFormat"), project.sourceFormat);
    map.insert(QStringLiteral("sizeBytes"), static_cast<double>(project.sizeBytes));
    map.insert(QStringLiteral("updatedAt"), project.updatedAt);
    map.insert(QStringLiteral("mainFile"), project.mainFile);
    return map;
}

QString CloudProjectsController::browserUrlForProject(const QString& projectId) const
{
    for (const QVariant& value : m_projects) {
        const QVariantMap map = value.toMap();
        if (map.value(QStringLiteral("id")).toString() == projectId) {
            const QString browser = map.value(QStringLiteral("browserUrl")).toString();
            if (!browser.isEmpty())
                return browser;
            const QString owner = map.value(QStringLiteral("ownerSlug")).toString();
            const QString slug = map.value(QStringLiteral("projectSlug")).toString();
            if (owner.isEmpty() || slug.isEmpty())
                return QString();
            return QtMeshCloudClient::projectDashboardUrl(owner, slug);
        }
    }
    return QString();
}

bool CloudProjectsController::lookupProjectSlugs(const QString& projectId,
                                                 QString* ownerSlug,
                                                 QString* projectSlug,
                                                 QString* projectName) const
{
    if (!ownerSlug || !projectSlug)
        return false;
    for (const QVariant& value : m_projects) {
        const QVariantMap map = value.toMap();
        if (!projectId.isEmpty() && map.value(QStringLiteral("id")).toString() != projectId)
            continue;
        *ownerSlug = map.value(QStringLiteral("ownerSlug")).toString();
        *projectSlug = map.value(QStringLiteral("projectSlug")).toString();
        if (projectName) {
            *projectName = map.value(QStringLiteral("name")).toString();
            if (projectName->isEmpty())
                *projectName = *projectSlug;
        }
        return !ownerSlug->isEmpty() && !projectSlug->isEmpty();
    }
    return false;
}

void CloudProjectsController::beginProjectFilesView(const QString& projectId,
                                                    const QString& ownerSlug,
                                                    const QString& projectSlug,
                                                    const QString& projectName)
{
    ensureSession();
    m_viewingProjectFiles = true;
    m_activeProjectId = projectId;
    m_activeOwnerSlug = ownerSlug;
    m_activeProjectSlug = projectSlug;
    m_activeProjectName = projectName.isEmpty() ? projectSlug : projectName;
    m_projectFiles.clear();
    m_loadingProjectFiles = true;
    emit activeProjectChanged();
    emit projectFilesChanged();
    emit loadingProjectFilesChanged();
    m_session->fetchProjectFiles(ownerSlug, projectSlug);
}

void CloudProjectsController::startDownloadBySlug(const QString& ownerSlug, const QString& projectSlug)
{
    ensureSession();
    m_downloading = true;
    m_session->downloadProjectBySlug(ownerSlug, projectSlug);
}

void CloudProjectsController::startFileDownload(const QString& fileId)
{
    ensureSession();
    m_downloading = true;
    m_session->downloadProjectFile(m_activeOwnerSlug, m_activeProjectSlug, fileId);
}
