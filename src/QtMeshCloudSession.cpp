#include "QtMeshCloudSession.h"

#include "CloudUploadPlanner.h"
#include "SentryReporter.h"

#include <QThread>

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
    QThread* worker = QThread::create([this, token]() {
        const auto result = QtMeshCloudClient::fetchProjects(token);
        QMetaObject::invokeMethod(this, [this, result]() {
            if (!result.ok) {
                emit projectsListed({}, result.errorString);
                return;
            }
            emit projectsListed(result.projects, {});
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
    const QString token = m_bearerToken;
    const PackageMetadata package = metadata;

    QThread* worker = QThread::create([this, token, package, ownerSlug, projectSlug, createNewProject]() {
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
        }

        if (m_canceled.load()) {
            QMetaObject::invokeMethod(this, [this]() { emit uploadCanceled(); }, Qt::QueuedConnection);
            return;
        }
        if (!project.ok) {
            QMetaObject::invokeMethod(this, [this, project]() {
                emit uploadFinished(false, project.errorString, {}, {});
            }, Qt::QueuedConnection);
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
        if (m_canceled.load()) {
            QMetaObject::invokeMethod(this, [this]() { emit uploadCanceled(); }, Qt::QueuedConnection);
            return;
        }
        if (!uploadUrls.ok) {
            QMetaObject::invokeMethod(this, [this, uploadUrls]() {
                emit uploadFinished(false, uploadUrls.errorString, {}, {});
            }, Qt::QueuedConnection);
            return;
        }

        QStringList uploadedFileIds;
        QString mainFileId;
        QString fallbackMainFileId;
        const int total = uploadUrls.uploads.size();
        for (int i = 0; i < total; ++i) {
            if (m_canceled.load()) {
                QMetaObject::invokeMethod(this, [this]() { emit uploadCanceled(); }, Qt::QueuedConnection);
                return;
            }

            const QString label = descriptors.at(i).uploadName;
            QMetaObject::invokeMethod(this, [this, i, total, label]() {
                emit uploadProgress(i + 1, total + 1, label);
            }, Qt::QueuedConnection);

            const auto result = QtMeshCloudClient::uploadFileContent(
                token, uploadUrls.uploads.at(i), descriptors.at(i).path, &m_canceled);
            if (result.canceled) {
                QMetaObject::invokeMethod(this, [this]() { emit uploadCanceled(); }, Qt::QueuedConnection);
                return;
            }
            if (!result.ok) {
                QMetaObject::invokeMethod(this, [this, result]() {
                    emit uploadFinished(false, result.errorString, {}, {});
                }, Qt::QueuedConnection);
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

        const auto completed = QtMeshCloudClient::completeUpload(
            token, project.ownerSlug, project.projectSlug, uploadedFileIds, mainFileId);
        if (!completed.ok) {
            QMetaObject::invokeMethod(this, [this, completed]() {
                emit uploadFinished(false, completed.errorString, {}, {});
            }, Qt::QueuedConnection);
            return;
        }

        SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
                                      QStringLiteral("QtMesh Cloud package upload completed"));
        QMetaObject::invokeMethod(this, [this, project, completed]() {
            emit uploadFinished(true, {}, project.projectUrl, completed.scanStatus);
        }, Qt::QueuedConnection);
    });

    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}
