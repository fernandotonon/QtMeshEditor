#include "QtMeshCloudSession.h"

#include "AssetScanController.h"
#include "CloudUploadDialog.h"
#include "CloudUploadPlanner.h"
#include "SentryReporter.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QThread>

namespace {

void invokeUploadProgress(QtMeshCloudSession* self, int current, int total, const QString& label)
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

void invokeUploadFinished(QtMeshCloudSession* self,
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

void invokeUploadCanceled(QtMeshCloudSession* self)
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

void invokePrepareWarning(QtMeshCloudSession* self, const QString& warning)
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
}

void QtMeshCloudSession::listProjects()
{
    const QString token = m_bearerToken;
    QPointer<QtMeshCloudSession> self(this);
    QThread* worker = QThread::create([self, token]() {
        const auto result = QtMeshCloudClient::fetchProjects(token);
        QMetaObject::invokeMethod(qApp, [self, result]() {
            if (!self)
                return;
            if (!result.ok) {
                emit self->projectsListed({}, result.errorString);
                return;
            }
            emit self->projectsListed(result.projects, {});
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
    m_canceled.store(false);
    startUploadWorker(metadata, ownerSlug, projectSlug, createNewProject);
}

void QtMeshCloudSession::uploadPackageFromAssets(const CloudPackageUploadRequest& request)
{
    m_canceled.store(false);
    const QString token = m_bearerToken;
    const CloudPackageUploadRequest req = request;
    QPointer<QtMeshCloudSession> self(this);

    QThread* worker = QThread::create([self, token, req, canceled = &m_canceled]() {
        if (canceled->load()) {
            invokeUploadCanceled(self.data());
            return;
        }

        invokeUploadProgress(self.data(), 0, 1, QStringLiteral("Preparing package…"));

        QJsonObject scanSummary;
        if (req.runLocalScan) {
            invokeUploadProgress(self.data(), 0, 1, QStringLiteral("Scanning assets…"));
            const QFileInfo mainAssetInfo(req.mainAssetPath);
            QString scanError;
            const QByteArray scanJson = AssetScanController::runIsolatedScanJsonSync(
                mainAssetInfo.absolutePath(), mainAssetInfo.fileName(), &scanError);
            if (scanJson.isEmpty()) {
                invokePrepareWarning(
                    self.data(),
                    scanError.isEmpty()
                        ? QStringLiteral("Local scan failed; continuing without scan summary.")
                        : QStringLiteral("Local scan failed; continuing without scan summary.\n\n%1")
                              .arg(scanError));
            } else {
                QJsonParseError parseError;
                const QJsonDocument doc = QJsonDocument::fromJson(scanJson, &parseError);
                if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
                    invokePrepareWarning(
                        self.data(),
                        QStringLiteral("Local scan returned invalid JSON; continuing without scan summary.\n\n%1")
                            .arg(parseError.errorString()));
                } else {
                    scanSummary = doc.object();
                }
            }
        }

        if (canceled->load()) {
            invokeUploadCanceled(self.data());
            return;
        }

        const PackageMetadata package = CloudUploadDialog::buildManifestForUpload(
            req.mainAssetPath, req.selectedAbsolutePaths, req.projectName, scanSummary);
        if (package.files.isEmpty()) {
            invokeUploadFinished(self.data(), false,
                                 QStringLiteral("Select at least one file to upload."), {}, {});
            return;
        }

        for (const PackageEntry& entry : package.files) {
            if (!QFileInfo::exists(entry.absolutePath)) {
                invokeUploadFinished(self.data(), false,
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
            project.projectUrl = QStringLiteral("https://qtmesh.dev/%1/%2")
                                     .arg(project.ownerSlug, project.projectSlug);
        }

        if (canceled->load()) {
            invokeUploadCanceled(self.data());
            return;
        }
        if (!project.ok) {
            invokeUploadFinished(self.data(), false, project.errorString, {}, {});
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
            invokeUploadCanceled(self.data());
            return;
        }
        if (!uploadUrls.ok) {
            invokeUploadFinished(self.data(), false, uploadUrls.errorString, {}, {});
            return;
        }

        QStringList uploadedFileIds;
        QString mainFileId;
        QString fallbackMainFileId;
        const int total = uploadUrls.uploads.size();
        for (int i = 0; i < total; ++i) {
            if (canceled->load()) {
                invokeUploadCanceled(self.data());
                return;
            }

            invokeUploadProgress(self.data(), i + 1, total + 1, descriptors.at(i).uploadName);

            const auto result = QtMeshCloudClient::uploadFileContent(
                token, uploadUrls.uploads.at(i), descriptors.at(i).path, canceled);
            if (result.canceled) {
                invokeUploadCanceled(self.data());
                return;
            }
            if (!result.ok) {
                invokeUploadFinished(self.data(), false, result.errorString, {}, {});
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
            invokeUploadCanceled(self.data());
            return;
        }

        invokeUploadProgress(self.data(), total + 1, total + 1, QString());

        const auto completed = QtMeshCloudClient::completeUpload(
            token, project.ownerSlug, project.projectSlug, uploadedFileIds, mainFileId);
        if (!completed.ok) {
            invokeUploadFinished(self.data(), false, completed.errorString, {}, {});
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
                SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
                                              reportWarning,
                                              QStringLiteral("warning"));
            }
        }

        SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
                                      QStringLiteral("QtMesh Cloud package upload completed"));
        invokeUploadFinished(self.data(), true, reportWarning, project.projectUrl, completed.scanStatus);
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

    QThread* worker = QThread::create([self, token, package, ownerSlug, projectSlug, createNewProject,
                                       canceled = &m_canceled]() {
        if (canceled->load()) {
            invokeUploadCanceled(self.data());
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
            invokeUploadCanceled(self.data());
            return;
        }
        if (!project.ok) {
            invokeUploadFinished(self.data(), false, project.errorString, {}, {});
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
            invokeUploadCanceled(self.data());
            return;
        }
        if (!uploadUrls.ok) {
            invokeUploadFinished(self.data(), false, uploadUrls.errorString, {}, {});
            return;
        }

        QStringList uploadedFileIds;
        QString mainFileId;
        QString fallbackMainFileId;
        const int total = uploadUrls.uploads.size();
        for (int i = 0; i < total; ++i) {
            if (canceled->load()) {
                invokeUploadCanceled(self.data());
                return;
            }

            invokeUploadProgress(self.data(), i + 1, total + 1, descriptors.at(i).uploadName);

            const auto result = QtMeshCloudClient::uploadFileContent(
                token, uploadUrls.uploads.at(i), descriptors.at(i).path, canceled);
            if (result.canceled) {
                invokeUploadCanceled(self.data());
                return;
            }
            if (!result.ok) {
                invokeUploadFinished(self.data(), false, result.errorString, {}, {});
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
            invokeUploadCanceled(self.data());
            return;
        }

        invokeUploadProgress(self.data(), total + 1, total + 1, QString());

        const auto completed = QtMeshCloudClient::completeUpload(
            token, project.ownerSlug, project.projectSlug, uploadedFileIds, mainFileId);
        if (!completed.ok) {
            invokeUploadFinished(self.data(), false, completed.errorString, {}, {});
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
                SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
                                              reportWarning,
                                              QStringLiteral("warning"));
            }
        }

        SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
                                      QStringLiteral("QtMesh Cloud package upload completed"));
        invokeUploadFinished(self.data(), true, reportWarning, project.projectUrl, completed.scanStatus);
    });

    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}
