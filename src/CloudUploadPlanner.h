#ifndef CLOUD_UPLOAD_PLANNER_H
#define CLOUD_UPLOAD_PLANNER_H

#include "QtMeshCloudClient.h"

#include <QString>
#include <QStringList>

namespace CloudUploadPlanner {

QString makeProjectSlug(const QString& name, const QString& fallback = QStringLiteral("qtmesh-project"));
QString inferAssetRole(const QString& fileName);
QStringList selectedPathsForUpload(const QString& mainAssetPath,
                                   const QStringList& includeGlobs = {},
                                   const QStringList& excludeGlobs = {});
QList<QtMeshCloudClient::AssetFileDescriptor> buildAssetFileDescriptors(const QStringList& paths);

} // namespace CloudUploadPlanner

#endif // CLOUD_UPLOAD_PLANNER_H
