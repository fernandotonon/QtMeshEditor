#include "CloudProjectsDialog.h"

#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

CloudProjectsDialog::CloudProjectsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("My Cloud Projects"));
    resize(560, 420);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Projects on QtMesh Cloud:"), this));

    m_list = new QListWidget(this);
    layout->addWidget(m_list, 1);

    auto* buttons = new QHBoxLayout();
    auto* closeButton = new QPushButton(tr("Close"), this);
    m_openButton = new QPushButton(tr("Open in Browser"), this);
    buttons->addStretch();
    buttons->addWidget(closeButton);
    buttons->addWidget(m_openButton);
    layout->addLayout(buttons);

    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_openButton, &QPushButton::clicked, this, [this]() {
        const QListWidgetItem* item = m_list->currentItem();
        if (!item)
            return;
        const QString url = item->data(Qt::UserRole + 1).toString();
        if (!url.isEmpty())
            QDesktopServices::openUrl(QUrl(url));
    });
    connect(m_list, &QListWidget::itemDoubleClicked, m_openButton, &QPushButton::click);
}

void CloudProjectsDialog::setProjects(const QList<QtMeshCloudClient::ProjectSummary>& projects)
{
    m_list->clear();
    for (const auto& project : projects) {
        const QString title = project.name.isEmpty() ? project.projectSlug : project.name;
        const QString subtitle = QStringLiteral("%1/%2").arg(project.ownerSlug, project.projectSlug);
        auto* item = new QListWidgetItem(
            subtitle == title ? title : QStringLiteral("%1\n%2").arg(title, subtitle),
            m_list);
        item->setData(Qt::UserRole, project.id);
        item->setData(Qt::UserRole + 1, project.projectUrl);
        item->setToolTip(project.projectUrl);
    }
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
}
