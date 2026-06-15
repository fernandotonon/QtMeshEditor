#include "CloudUploadDialog.h"

#include "DependencyResolver.h"
#include "ProjectPackager.h"
#include "SentryReporter.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSet>
#include <QVBoxLayout>

CloudUploadDialog::CloudUploadDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Upload to QtMesh Cloud"));
    resize(640, 520);

    auto* layout = new QVBoxLayout(this);

    auto* accountLabel = new QLabel(this);
    accountLabel->setObjectName(QStringLiteral("cloudUploadAccountLabel"));
    accountLabel->setWordWrap(true);
    layout->addWidget(accountLabel);

    auto* projectLabel = new QLabel(tr("Project:"), this);
    layout->addWidget(projectLabel);

    m_projectCombo = new QComboBox(this);
    m_projectCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    layout->addWidget(m_projectCombo);

    auto* depsLabel = new QLabel(tr("Files to upload:"), this);
    layout->addWidget(depsLabel);

    m_dependencyList = new QListWidget(this);
    m_dependencyList->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(m_dependencyList, 1);

    m_runScanCheck = new QCheckBox(tr("Run local scan before upload"), this);
    m_runScanCheck->setChecked(true);
    layout->addWidget(m_runScanCheck);

    auto* buttons = new QHBoxLayout();
    auto* cancelButton = new QPushButton(tr("Cancel"), this);
    m_uploadButton = new QPushButton(tr("Upload"), this);
    m_uploadButton->setDefault(true);
    buttons->addStretch();
    buttons->addWidget(cancelButton);
    buttons->addWidget(m_uploadButton);
    layout->addLayout(buttons);

    connect(cancelButton, &QPushButton::clicked, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud upload dialog: Cancel"));
        reject();
    });
    connect(m_uploadButton, &QPushButton::clicked, this, [this]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("Cloud upload dialog: Upload"));
        accept();
    });
    connect(m_projectCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        m_uploadButton->setEnabled(hasSelectedProject());
    });

    m_uploadButton->setEnabled(false);
}

void CloudUploadDialog::setAccountLabel(const QString& accountLabel)
{
    if (auto* label = findChild<QLabel*>(QStringLiteral("cloudUploadAccountLabel")))
        label->setText(tr("Destination account: %1").arg(accountLabel));
}

void CloudUploadDialog::setMainAssetPath(const QString& mainAssetPath)
{
    m_mainAssetPath = mainAssetPath;
    rebuildDependencyList();
}

void CloudUploadDialog::setProjects(const QList<QtMeshCloudClient::ProjectSummary>& projects)
{
    m_projectCombo->clear();
    for (const QtMeshCloudClient::ProjectSummary& project : projects) {
        const QString title = project.name.isEmpty() ? project.projectSlug : project.name;
        const QString subtitle =
            QStringLiteral("%1/%2").arg(project.ownerSlug, project.projectSlug);
        const QString label = subtitle == title
                                  ? title
                                  : QStringLiteral("%1 (%2)").arg(title, subtitle);
        m_projectCombo->addItem(label);
        const int index = m_projectCombo->count() - 1;
        m_projectCombo->setItemData(index, project.id, Qt::UserRole);
        m_projectCombo->setItemData(index, project.ownerSlug, Qt::UserRole + 1);
        m_projectCombo->setItemData(index, project.projectSlug, Qt::UserRole + 2);
        m_projectCombo->setItemData(index, project.name, Qt::UserRole + 3);
        m_projectCombo->setItemData(index, project.projectUrl, Qt::UserRole + 4);
    }
    m_uploadButton->setEnabled(hasSelectedProject());
}

void CloudUploadDialog::setScanSummary(const QJsonObject& scanSummary)
{
    m_scanSummary = scanSummary;
}

void CloudUploadDialog::rebuildDependencyList()
{
    m_dependencyList->clear();
    m_dependencies = DependencyResolver::detect(m_mainAssetPath);
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
                                  QStringLiteral("Detected %1 cloud upload dependencies")
                                      .arg(m_dependencies.size()));
    for (const DependencyEntry& entry : m_dependencies) {
        const QFileInfo info(entry.absolutePath);
        QString label = info.fileName();
        if (!entry.exists)
            label += tr(" (missing)");
        auto* item = new QListWidgetItem(label, m_dependencyList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(entry.checkedByDefault ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, entry.absolutePath);
        item->setToolTip(entry.referencedBy);
        if (!entry.exists)
            item->setForeground(Qt::darkYellow);
    }
}

bool CloudUploadDialog::hasSelectedProject() const
{
    return m_projectCombo && m_projectCombo->currentIndex() >= 0;
}

QtMeshCloudClient::ProjectSummary CloudUploadDialog::selectedProject() const
{
    QtMeshCloudClient::ProjectSummary summary;
    if (!hasSelectedProject())
        return summary;

    const int index = m_projectCombo->currentIndex();
    summary.id = m_projectCombo->itemData(index, Qt::UserRole).toString();
    summary.ownerSlug = m_projectCombo->itemData(index, Qt::UserRole + 1).toString();
    summary.projectSlug = m_projectCombo->itemData(index, Qt::UserRole + 2).toString();
    summary.name = m_projectCombo->itemData(index, Qt::UserRole + 3).toString();
    summary.projectUrl = m_projectCombo->itemData(index, Qt::UserRole + 4).toString();
    return summary;
}

QString CloudUploadDialog::projectName() const
{
    const QtMeshCloudClient::ProjectSummary project = selectedProject();
    if (!project.name.isEmpty())
        return project.name;
    return project.projectSlug;
}

bool CloudUploadDialog::runLocalScanBeforeUpload() const
{
    return m_runScanCheck->isChecked();
}

QStringList CloudUploadDialog::selectedAbsolutePaths() const
{
    QStringList extras;
    for (int row = 0; row < m_dependencyList->count(); ++row) {
        QListWidgetItem* item = m_dependencyList->item(row);
        if (item && item->checkState() == Qt::Checked)
            extras.append(item->data(Qt::UserRole).toString());
    }
    return extras;
}

PackageMetadata CloudUploadDialog::manifest() const
{
    const QString mainCanonical = QFileInfo(m_mainAssetPath).absoluteFilePath();
    QSet<QString> selected;
    for (const QString& path : selectedAbsolutePaths())
        selected.insert(QFileInfo(path).absoluteFilePath());

    QStringList extras;
    for (const QString& path : selected) {
        if (path != mainCanonical)
            extras.append(path);
    }
    PackageMetadata metadata =
        ProjectPackager::buildManifest(m_mainAssetPath, extras, projectName());
    metadata.files.erase(
        std::remove_if(metadata.files.begin(), metadata.files.end(),
                       [&](const PackageEntry& entry) {
                           return !selected.contains(entry.absolutePath);
                       }),
        metadata.files.end());
    metadata.totalSize = 0;
    for (const PackageEntry& entry : metadata.files)
        metadata.totalSize += entry.size;
    if (!m_scanSummary.isEmpty())
        metadata.scanSummary = m_scanSummary;
    return metadata;
}
