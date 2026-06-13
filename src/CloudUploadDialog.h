#ifndef CLOUD_UPLOAD_DIALOG_H
#define CLOUD_UPLOAD_DIALOG_H

#include "DependencyResolver.h"
#include "ProjectPackager.h"
#include "QtMeshCloudClient.h"

#include <QDialog>
#include <QStringList>

class QCheckBox;
class QComboBox;
class QListWidget;
class QPushButton;

class CloudUploadDialog : public QDialog {
    Q_OBJECT

public:
    explicit CloudUploadDialog(QWidget* parent = nullptr);

    void setAccountLabel(const QString& accountLabel);
    void setMainAssetPath(const QString& mainAssetPath);
    void setProjects(const QList<QtMeshCloudClient::ProjectSummary>& projects);
    void setScanSummary(const QJsonObject& scanSummary);

    bool hasSelectedProject() const;
    QtMeshCloudClient::ProjectSummary selectedProject() const;
    QString projectName() const;
    PackageMetadata manifest() const;
    bool runLocalScanBeforeUpload() const;

private:
    void rebuildDependencyList();
    QStringList selectedAbsolutePaths() const;

    QString m_mainAssetPath;
    QComboBox* m_projectCombo = nullptr;
    QListWidget* m_dependencyList = nullptr;
    QCheckBox* m_runScanCheck = nullptr;
    QPushButton* m_uploadButton = nullptr;
    QVector<DependencyEntry> m_dependencies;
    QJsonObject m_scanSummary;
};

#endif // CLOUD_UPLOAD_DIALOG_H
