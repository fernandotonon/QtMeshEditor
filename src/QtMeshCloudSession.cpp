#include "QtMeshCloudSession.h"

#include "AssetScanController.h"
#include "CloudCredentialStore.h"
#include "CloudUploadDialog.h"
#include "CloudUploadPlanner.h"
#include "SentryReporter.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>
#include <QThread>

namespace {

void invokeUploadProgress(const QPointer<QtMeshCloudSession>& self,
                          int current,
                          int total,
                          const QString& label)
{
    if (!self)
        return;
    QMetaObject::invokeMethod(qApp,
                              [self, current, total, label]() {
                                  if (self)
                                      emit self->uploadProgress(current, total, label);
                              },
                              Qt::QueuedConnection);
}

void invokeUploadFinished(const QPointer<QtMeshCloudSession>& self,
                          bool ok,
                          const QString& error,
                          const QString& projectUrl,
                          const QString& scanStatus)
{
    if (!self)
        return;
    QMetaObject::invokeMethod(qApp,
                              [self, ok, error, projectUrl, scanStatus]() {
                                  if (self)
                                      emit self->uploadFinished(ok, error, projectUrl, scanStatus);
                              },
                              Qt::QueuedConnection);
}

void invokeUploadCanceled(const QPointer<QtMeshCloudSession>& self)
{
    if (!self)
        return;
    QMetaObject::invokeMethod(qApp,
                              [self]() {
                                  if (self)
                                      emit self->uploadCanceled();
                              },
                              Qt::QueuedConnection);
}

void invokePrepareWarning(const QPointer<QtMeshCloudSession>& self, const QString& warning)
{
    if (!self || warning.isEmpty())
        return;
    QMetaObject::invokeMethod(qApp,
                              [self, warning]() {
                                  if (self)
                                      emit self->uploadPrepareWarning(warning);
                              },
                              Qt::QueuedConnection);
}

} // namespace

QtMeshCloudSession::QtMeshCloudSession(const QString& bearerToken, QObject* parent)
    : QObject(parent)
    , m_bearerToken(bearerToken)
{
}

void QtMeshCloudSession::cancel()
{
    m_canceled.store(true);
    if (m_uploadCancelFlag)
        m_uploadCancelFlag->store(true);
}

void QtMeshCloudSession::listProjects(const QString& cursor)
{
    const QString token = m_bearerToken;
    QPointer<QtMeshCloudSession> self(this);
    QThread* worker = QThread::create([self, token, cursor]() {
        auto fetch = [](const QString& bearer, const QString& pageCursor) {
            return QtMeshCloudClient::fetchProjects(bearer, pageCursor);
        };

        QString activeToken = token;
        auto result = fetch(activeToken, cursor);
        if (!result.ok && result.httpStatus == 401) {
            const QString reloaded = CloudCredentialStore::loadSession().token;
            if (!reloaded.isEmpty() && reloaded != activeToken) {
                activeToken = reloaded;
                result = fetch(activeToken, cursor);
            }
        }

        const auto listed = result;
        QMetaObject::invokeMethod(qApp, [self, listed, activeToken, token]() {
            if (!self)
                return;
            if (!listed.ok) {
                QString err = listed.errorString;
                if (listed.httpStatus == 401)
                    err = QStringLiteral("unauthorized");
                emit self->projectsListed({}, err, {}, false);
                return;
            }
            if (activeToken != token)
                self->setBearerToken(activeToken);
            emit self->projectsListed(listed.projects, {}, listed.nextCursor, listed.hasMore);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void QtMeshCloudSession::deleteProject(const QString& projectId)
{
    const QString token = m_bearerToken;
    const QString id = projectId.trimmed();
    QPointer<QtMeshCloudSession> self(this);
    QThread* worker = QThread::create([self, token, id]() {
        QString activeToken = token;
        auto result = QtMeshCloudClient::deleteProject(activeToken, id);
        if (!result.ok && result.httpStatus == 401) {
            const QString reloaded = CloudCredentialStore::loadSession().token;
            if (!reloaded.isEmpty() && reloaded != activeToken) {
                activeToken = reloaded;
                result = QtMeshCloudClient::deleteProject(activeToken, id);
            }
        }
        QMetaObject::invokeMethod(qApp, [self, id, result, activeToken, token]() {
            if (!self)
                return;
            if (activeToken != token)
                self->setBearerToken(activeToken);
            emit self->projectDeleted(id, result.ok ? QString() : result.errorString);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void QtMeshCloudSession::downloadProject(const QString& projectId, const QString& destDir)
{
    Q_UNUSED(destDir);
    QMetaObject::invokeMethod(this, [this, projectId]() {
        emit downloadComplete(false,
                              QStringLiteral("Download by project id is not supported. Use owner/slug."),
                              QStringLiteral("not-implemented"));
    }, Qt::QueuedConnection);
}

void QtMeshCloudSession::downloadProjectBySlug(const QString& ownerSlug,
                                               const QString& projectSlug,
                                               const QString& destDir)
{
    m_canceled.store(false);
    const QString token = m_bearerToken;
    const QString owner = ownerSlug.trimmed();
    const QString slug = projectSlug.trimmed();
    QString destination = destDir.trimmed();
    if (destination.isEmpty()) {
        destination = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
            + QStringLiteral("/cloud/") + owner + QLatin1Char('/') + slug;
    }

    QPointer<QtMeshCloudSession> self(this);
    QThread* worker = QThread::create([self, token, owner, slug, destination, canceled = &m_canceled]() {
        auto runDownload = [&](const QString& bearer) {
            return QtMeshCloudClient::downloadProjectBySlug(
                bearer,
                owner,
                slug,
                destination,
                [&](int current, int total, const QString& fileName) {
                    QMetaObject::invokeMethod(qApp,
                                            [self, current, total, fileName]() {
                                                if (self)
                                                    emit self->downloadProgress(current, total, fileName);
                                            },
                                            Qt::QueuedConnection);
                },
                canceled);
        };

        QString activeToken = token;
        auto result = runDownload(activeToken);
        if (!result.ok) {
            const QString reloaded = CloudCredentialStore::loadSession().token;
            if (!reloaded.isEmpty() && reloaded != activeToken) {
                activeToken = reloaded;
                result = runDownload(activeToken);
            }
        }

        QMetaObject::invokeMethod(qApp, [self, result, activeToken, token]() {
            if (!self)
                return;
            if (activeToken != token)
                self->setBearerToken(activeToken);
            if (result.ok)
                emit self->downloadComplete(true, result.localMainFile, {});
            else
                emit self->downloadComplete(false, result.errorString, QStringLiteral("download-failed"));
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

namespace {

QVariantList projectFilesToVariantList(const QList<QtMeshCloudClient::ProjectFileEntry>& files)
{
    QVariantList mapped;
    mapped.reserve(files.size());
    for (const QtMeshCloudClient::ProjectFileEntry& file : files) {
        QVariantMap map;
        map.insert(QStringLiteral("id"), file.id);
        map.insert(QStringLiteral("originalName"), file.originalName);
        map.insert(QStringLiteral("name"), file.name);
        map.insert(QStringLiteral("role"), file.role);
        map.insert(QStringLiteral("extension"), file.extension);
        map.insert(QStringLiteral("sizeBytes"), static_cast<double>(file.sizeBytes));
        map.insert(QStringLiteral("canOpen"),
                   QtMeshCloudClient::isImportableCloudAssetPath(file.originalName));
        mapped.append(map);
    }
    return mapped;
}

QString defaultCloudProjectCacheDir(const QString& owner, const QString& slug)
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
        + QStringLiteral("/cloud/") + owner + QLatin1Char('/') + slug;
}

} // namespace

void QtMeshCloudSession::fetchProjectFiles(const QString& ownerSlug, const QString& projectSlug)
{
    const QString token = m_bearerToken;
    const QString owner = ownerSlug.trimmed();
    const QString slug = projectSlug.trimmed();
    QPointer<QtMeshCloudSession> self(this);
    QThread* worker = QThread::create([self, token, owner, slug]() {
        auto runFetch = [owner, slug](const QString& bearer) {
            return QtMeshCloudClient::fetchProjectManifest(bearer, owner, slug);
        };

        QString activeToken = token;
        auto manifest = runFetch(activeToken);
        if (!manifest.ok) {
            const QString reloaded = CloudCredentialStore::loadSession().token;
            if (!reloaded.isEmpty() && reloaded != activeToken) {
                activeToken = reloaded;
                manifest = runFetch(activeToken);
            }
        }

        const auto fetched = manifest;
        const QVariantList files = fetched.ok
            ? projectFilesToVariantList(QtMeshCloudClient::projectFilesFromManifest(fetched.manifest))
            : QVariantList{};
        const QString error = fetched.ok ? QString() : fetched.errorString;

        QMetaObject::invokeMethod(qApp, [self, files, error, activeToken, token]() {
            if (!self)
                return;
            if (activeToken != token)
                self->setBearerToken(activeToken);
            emit self->projectFilesFetched(files, error);
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void QtMeshCloudSession::downloadProjectFile(const QString& ownerSlug,
                                           const QString& projectSlug,
                                           const QString& fileId,
                                           const QString& destDir)
{
    m_canceled.store(false);
    const QString token = m_bearerToken;
    const QString owner = ownerSlug.trimmed();
    const QString slug = projectSlug.trimmed();
    const QString id = fileId.trimmed();
    QString destination = destDir.trimmed();
    if (destination.isEmpty())
        destination = defaultCloudProjectCacheDir(owner, slug);

    QPointer<QtMeshCloudSession> self(this);
    QThread* worker = QThread::create([self, token, owner, slug, id, destination, canceled = &m_canceled]() {
        auto runDownload = [&](const QString& bearer) {
            return QtMeshCloudClient::downloadProjectFileBySlug(
                bearer,
                owner,
                slug,
                id,
                destination,
                [&](int current, int total, const QString& fileName) {
                    QMetaObject::invokeMethod(qApp,
                                            [self, current, total, fileName]() {
                                                if (self)
                                                    emit self->downloadProgress(current, total, fileName);
                                            },
                                            Qt::QueuedConnection);
                },
                canceled);
        };

        QString activeToken = token;
        auto result = runDownload(activeToken);
        if (!result.ok) {
            const QString reloaded = CloudCredentialStore::loadSession().token;
            if (!reloaded.isEmpty() && reloaded != activeToken) {
                activeToken = reloaded;
                result = runDownload(activeToken);
            }
        }

        QMetaObject::invokeMethod(qApp, [self, result, activeToken, token]() {
            if (!self)
                return;
            if (activeToken != token)
                self->setBearerToken(activeToken);
            if (result.ok)
                emit self->downloadComplete(true, result.localMainFile, {});
            else
                emit self->downloadComplete(false, result.errorString, QStringLiteral("download-failed"));
        }, Qt::QueuedConnection);
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void QtMeshCloudSession::uploadPackage(const PackageMetadata& metadata,
                                       const QString& ownerSlug,
                                       const QString& projectSlug,
                                       bool createNewProject)
{
    m_uploadCancelFlag = std::make_shared<std::atomic_bool>(false);
    m_canceled.store(false);
    startUploadWorker(metadata, ownerSlug, projectSlug, createNewProject);
}

void QtMeshCloudSession::uploadPackageFromAssets(const CloudPackageUploadRequest& request)
{
    m_uploadCancelFlag = std::make_shared<std::atomic_bool>(false);
    m_canceled.store(false);
    const QString token = m_bearerToken;
    const CloudPackageUploadRequest req = request;
    QPointer<QtMeshCloudSession> self(this);
    const std::shared_ptr<std::atomic_bool> canceled = m_uploadCancelFlag;

    QThread* worker = QThread::create([self, token, req, canceled]() {
        if (canceled->load()) {
            invokeUploadCanceled(self);
            return;
        }

        invokeUploadProgress(self, 0, 1, QStringLiteral("Preparing package…"));

        QJsonObject scanSummary;
        if (req.runLocalScan) {
            invokeUploadProgress(self, 0, 1, QStringLiteral("Scanning assets…"));
            const QFileInfo mainAssetInfo(req.mainAssetPath);
            QString scanError;
            const QByteArray scanJson = AssetScanController::runIsolatedScanJsonSync(
                mainAssetInfo.absolutePath(), mainAssetInfo.fileName(), &scanError);
            if (scanJson.isEmpty()) {
                invokePrepareWarning(
                    self,
                    scanError.isEmpty()
                        ? QStringLiteral("Local scan failed; continuing without scan summary.")
                        : QStringLiteral("Local scan failed; continuing without scan summary.\n\n%1")
                              .arg(scanError));
            } else {
                QJsonParseError parseError;
                const QJsonDocument doc = QJsonDocument::fromJson(scanJson, &parseError);
                if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                    invokePrepareWarning(
                        self,
                        QStringLiteral("Local scan returned invalid JSON; continuing without scan summary.\n\n%1")
                            .arg(parseError.errorString()));
                } else {
                    scanSummary = doc.object();
                }
            }
        }

        if (canceled->load()) {
            invokeUploadCanceled(self);
            return;
        }

        const PackageMetadata package = CloudUploadDialog::buildManifestForUpload(
            req.mainAssetPath, req.selectedAbsolutePaths, req.projectName, scanSummary);
        if (package.files.isEmpty()) {
            invokeUploadFinished(self, false,
                                 QStringLiteral("Select at least one file to upload."), {}, {});
            return;
        }

        for (const PackageEntry& entry : package.files) {
            if (!QFileInfo::exists(entry.absolutePath)) {
                invokeUploadFinished(self, false,
                                     QStringLiteral("Missing file: %1")
                                         .arg(QFileInfo(entry.absolutePath).fileName()),
                                     {}, {});
                return;
            }
        }

        if (!self)
            return;

        // Continue upload on this worker thread (scan/manifest prep must not block the GUI).
        const bool createNewProject = req.createNewProject;
        const QString ownerSlug = req.ownerSlug;
        const QString projectSlug = req.projectSlug;

        QtMeshCloudClient::ProjectResult project;
        if (createNewProject) {
            QString slug = projectSlug.isEmpty()
                ? CloudUploadPlanner::makeProjectSlug(package.projectName)
                : projectSlug;
            project = QtMeshCloudClient::createProject(token, package.projectName, slug);
            if (!project.ok && project.httpStatus == 409) {
                slug = CloudUploadPlanner::makeProjectSlug(
                    QStringLiteral("%1-%2").arg(package.projectName, slug));
                project = QtMeshCloudClient::createProject(token, package.projectName, slug);
            }
        } else {
            project.ok = true;
            project.ownerSlug = ownerSlug;
            project.projectSlug = projectSlug;
            project.projectUrl = QtMeshCloudClient::projectDashboardUrl(
                project.ownerSlug, project.projectSlug);
        }

        if (canceled->load()) {
            invokeUploadCanceled(self);
            return;
        }
        if (!project.ok) {
            invokeUploadFinished(self, false, project.errorString, {}, {});
            return;
        }

        QList<QtMeshCloudClient::AssetFileDescriptor> descriptors;
        for (const PackageEntry& entry : package.files) {
            QtMeshCloudClient::AssetFileDescriptor descriptor;
            descriptor.path = entry.absolutePath.isEmpty() ? entry.relativePath : entry.absolutePath;
            descriptor.uploadName = entry.relativePath;
            descriptor.role = entry.role;
            descriptor.sizeBytes = entry.size;
            descriptors.append(descriptor);
        }

        const auto uploadUrls = QtMeshCloudClient::requestUploadUrls(
            token, project.ownerSlug, project.projectSlug, descriptors);
        if (canceled->load()) {
            invokeUploadCanceled(self);
            return;
        }
        if (!uploadUrls.ok) {
            invokeUploadFinished(self, false, uploadUrls.errorString, {}, {});
            return;
        }

        QStringList uploadedFileIds;
        QString mainFileId;
        QString fallbackMainFileId;
        const int total = uploadUrls.uploads.size();
        for (int i = 0; i < total; ++i) {
            if (canceled->load()) {
                invokeUploadCanceled(self);
                return;
            }

            invokeUploadProgress(self, i + 1, total + 1, descriptors.at(i).uploadName);

            const auto result = QtMeshCloudClient::uploadFileContent(
                token, uploadUrls.uploads.at(i), descriptors.at(i).path, canceled.get());
            if (result.canceled) {
                invokeUploadCanceled(self);
                return;
            }
            if (!result.ok) {
                invokeUploadFinished(self, false, result.errorString, {}, {});
                return;
            }

            uploadedFileIds.append(uploadUrls.uploads.at(i).fileId);
            if (fallbackMainFileId.isEmpty())
                fallbackMainFileId = uploadUrls.uploads.at(i).fileId;
            if (mainFileId.isEmpty() && descriptors.at(i).role == QLatin1String("main"))
                mainFileId = uploadUrls.uploads.at(i).fileId;
        }
        if (mainFileId.isEmpty())
            mainFileId = fallbackMainFileId;

        if (canceled->load()) {
            invokeUploadCanceled(self);
            return;
        }

        invokeUploadProgress(self, total + 1, total + 1, QString());

        const auto completed = QtMeshCloudClient::completeUpload(
            token, project.ownerSlug, project.projectSlug, uploadedFileIds, mainFileId);
        if (!completed.ok) {
            invokeUploadFinished(self, false, completed.errorString, {}, {});
            return;
        }

        QString reportWarning;
        if (!package.scanSummary.isEmpty() && !mainFileId.isEmpty()) {
            const auto reportResult = QtMeshCloudClient::uploadFileReport(
                token, project.ownerSlug, project.projectSlug, mainFileId, package.scanSummary);
            if (!reportResult.ok) {
                reportWarning = QStringLiteral("File uploaded, but analysis report upload failed.");
                if (!reportResult.errorString.isEmpty())
                    reportWarning += QStringLiteral("\n\n") + reportResult.errorString;
                SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                                              reportWarning,
                                              QStringLiteral("warning"));
            }
        }

        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                                      QStringLiteral("QtMesh Cloud package upload completed"));
        invokeUploadFinished(self, true, reportWarning, project.projectUrl, completed.scanStatus);
    });

    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void QtMeshCloudSession::startUploadWorker(const PackageMetadata& package,
                                           const QString& ownerSlug,
                                           const QString& projectSlug,
                                           bool createNewProject)
{
    const QString token = m_bearerToken;
    QPointer<QtMeshCloudSession> self(this);
    const std::shared_ptr<std::atomic_bool> canceled = m_uploadCancelFlag;

    QThread* worker = QThread::create([self, token, package, ownerSlug, projectSlug, createNewProject,
                                       canceled]() {
        if (canceled->load()) {
            invokeUploadCanceled(self);
            return;
        }

        QtMeshCloudClient::ProjectResult project;
        if (createNewProject) {
            QString slug = projectSlug.isEmpty()
                ? CloudUploadPlanner::makeProjectSlug(package.projectName)
                : projectSlug;
            project = QtMeshCloudClient::createProject(token, package.projectName, slug);
            if (!project.ok && project.httpStatus == 409) {
                slug = CloudUploadPlanner::makeProjectSlug(
                    QStringLiteral("%1-%2").arg(package.projectName, slug));
                project = QtMeshCloudClient::createProject(token, package.projectName, slug);
            }
        } else {
            project.ok = true;
            project.ownerSlug = ownerSlug;
            project.projectSlug = projectSlug;
            project.projectUrl = QStringLiteral("https://qtmesh.dev/%1/%2")
                                     .arg(project.ownerSlug, project.projectSlug);
        }

        if (canceled->load()) {
            invokeUploadCanceled(self);
            return;
        }
        if (!project.ok) {
            invokeUploadFinished(self, false, project.errorString, {}, {});
            return;
        }

        QList<QtMeshCloudClient::AssetFileDescriptor> descriptors;
        for (const PackageEntry& entry : package.files) {
            QtMeshCloudClient::AssetFileDescriptor descriptor;
            descriptor.path = entry.absolutePath.isEmpty() ? entry.relativePath : entry.absolutePath;
            descriptor.uploadName = entry.relativePath;
            descriptor.role = entry.role;
            descriptor.sizeBytes = entry.size;
            descriptors.append(descriptor);
        }

        const auto uploadUrls = QtMeshCloudClient::requestUploadUrls(
            token, project.ownerSlug, project.projectSlug, descriptors);
        if (canceled->load()) {
            invokeUploadCanceled(self);
            return;
        }
        if (!uploadUrls.ok) {
            invokeUploadFinished(self, false, uploadUrls.errorString, {}, {});
            return;
        }

        QStringList uploadedFileIds;
        QString mainFileId;
        QString fallbackMainFileId;
        const int total = uploadUrls.uploads.size();
        for (int i = 0; i < total; ++i) {
            if (canceled->load()) {
                invokeUploadCanceled(self);
                return;
            }

            invokeUploadProgress(self, i + 1, total + 1, descriptors.at(i).uploadName);

            const auto result = QtMeshCloudClient::uploadFileContent(
                token, uploadUrls.uploads.at(i), descriptors.at(i).path, canceled.get());
            if (result.canceled) {
                invokeUploadCanceled(self);
                return;
            }
            if (!result.ok) {
                invokeUploadFinished(self, false, result.errorString, {}, {});
                return;
            }

            uploadedFileIds.append(uploadUrls.uploads.at(i).fileId);
            if (fallbackMainFileId.isEmpty())
                fallbackMainFileId = uploadUrls.uploads.at(i).fileId;
            if (mainFileId.isEmpty() && descriptors.at(i).role == QLatin1String("main"))
                mainFileId = uploadUrls.uploads.at(i).fileId;
        }
        if (mainFileId.isEmpty())
            mainFileId = fallbackMainFileId;

        if (canceled->load()) {
            invokeUploadCanceled(self);
            return;
        }

        invokeUploadProgress(self, total + 1, total + 1, QString());

        const auto completed = QtMeshCloudClient::completeUpload(
            token, project.ownerSlug, project.projectSlug, uploadedFileIds, mainFileId);
        if (!completed.ok) {
            invokeUploadFinished(self, false, completed.errorString, {}, {});
            return;
        }

        QString reportWarning;
        if (!package.scanSummary.isEmpty() && !mainFileId.isEmpty()) {
            const auto reportResult = QtMeshCloudClient::uploadFileReport(
                token, project.ownerSlug, project.projectSlug, mainFileId, package.scanSummary);
            if (!reportResult.ok) {
                reportWarning = QStringLiteral("File uploaded, but analysis report upload failed.");
                if (!reportResult.errorString.isEmpty())
                    reportWarning += QStringLiteral("\n\n") + reportResult.errorString;
                SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                                              reportWarning,
                                              QStringLiteral("warning"));
            }
        }

        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                                      QStringLiteral("QtMesh Cloud package upload completed"));
        invokeUploadFinished(self, true, reportWarning, project.projectUrl, completed.scanStatus);
    });

    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}
