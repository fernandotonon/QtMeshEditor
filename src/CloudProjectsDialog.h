#ifndef CLOUD_PROJECTS_DIALOG_H
#define CLOUD_PROJECTS_DIALOG_H

#include "QtMeshCloudClient.h"

#include <QDialog>

class QListWidget;
class QPushButton;

class CloudProjectsDialog : public QDialog {
    Q_OBJECT

public:
    explicit CloudProjectsDialog(QWidget* parent = nullptr);

    void setProjects(const QList<QtMeshCloudClient::ProjectSummary>& projects);

private:
    QListWidget* m_list = nullptr;
    QPushButton* m_openButton = nullptr;
};

#endif // CLOUD_PROJECTS_DIALOG_H
