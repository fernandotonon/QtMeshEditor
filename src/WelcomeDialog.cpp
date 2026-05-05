#include "WelcomeDialog.h"
#include "SentryReporter.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QListWidget>
#include <QFileInfo>
#include <QSettings>
#include <QFileDialog>
#include <QApplication>
#include <QStyle>
#include <QFont>

bool WelcomeDialog::shouldShow()
{
    QSettings settings;
    return !settings.value("WelcomeScreen/dontShowAgain", false).toBool();
}

WelcomeDialog::WelcomeDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("QtMeshEditor");
    setFixedSize(480, 420);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(24, 20, 24, 16);

    // Title
    auto* titleLabel = new QLabel("QtMeshEditor");
    QFont titleFont = titleLabel->font();
    titleFont.setPointSize(20);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(titleLabel);

    auto* subtitleLabel = new QLabel("Automate your 3D asset pipeline");
    subtitleLabel->setAlignment(Qt::AlignCenter);
    subtitleLabel->setStyleSheet("color: gray;");
    mainLayout->addWidget(subtitleLabel);

    mainLayout->addSpacing(4);

    // Action buttons
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(10);

    auto* newSceneBtn = new QPushButton("New Scene");
    newSceneBtn->setMinimumHeight(36);
    newSceneBtn->setStyleSheet(
        "QPushButton { background-color: #3080d0; color: white; border-radius: 4px; padding: 0 16px; font-weight: bold; }"
        "QPushButton:hover { background-color: #4090e0; }");
    connect(newSceneBtn, &QPushButton::clicked, this, [this]() {
        m_action = NewScene;
        SentryReporter::addBreadcrumb("ui.action", "Welcome: New Scene");
        accept();
    });
    btnLayout->addWidget(newSceneBtn);

    auto* openFileBtn = new QPushButton("Open File...");
    openFileBtn->setMinimumHeight(36);
    connect(openFileBtn, &QPushButton::clicked, this, [this]() {
        QString file = QFileDialog::getOpenFileName(
            this, "Open 3D File", QString(),
            "3D Files (*.fbx *.gltf *.glb *.vrm *.obj *.dae *.stl *.mesh *.3ds *.x *.tmd *.TMD);;All Files (*)");
        if (!file.isEmpty()) {
            m_action = OpenFile;
            m_selectedFile = file;
            SentryReporter::addBreadcrumb("ui.action", "Welcome: Open File");
            accept();
        }
    });
    btnLayout->addWidget(openFileBtn);

    mainLayout->addLayout(btnLayout);

    // Recent files
    QSettings settings;
    QStringList recentFiles = settings.value("RecentFiles/files").toStringList();

    if (!recentFiles.isEmpty()) {
        auto* recentLabel = new QLabel("Recent Files");
        QFont recentFont = recentLabel->font();
        recentFont.setBold(true);
        recentLabel->setFont(recentFont);
        mainLayout->addWidget(recentLabel);

        auto* recentList = new QListWidget();
        recentList->setMaximumHeight(120);
        for (const QString& path : recentFiles) {
            QFileInfo fi(path);
            if (fi.exists()) {
                auto* item = new QListWidgetItem(fi.fileName());
                item->setToolTip(path);
                item->setData(Qt::UserRole, path);
                recentList->addItem(item);
            }
        }
        auto openRecent = [this](QListWidgetItem* item) {
            m_action = OpenRecent;
            m_selectedFile = item->data(Qt::UserRole).toString();
            SentryReporter::addBreadcrumb("ui.action", "Welcome: Open Recent");
            accept();
        };
        connect(recentList, &QListWidget::itemDoubleClicked, this, openRecent);
        connect(recentList, &QListWidget::itemActivated, this, openRecent);
        mainLayout->addWidget(recentList);
    }

    // Tips
    auto* tipsLabel = new QLabel(
        "<b>Tips:</b> Press <b>Ctrl+/</b> for keyboard shortcuts, "
        "<b>Ctrl+,</b> for preferences. "
        "Use the <b>AI Chat</b> panel to control the editor with natural language.");
    tipsLabel->setWordWrap(true);
    tipsLabel->setStyleSheet("color: gray; font-size: 11px;");
    mainLayout->addWidget(tipsLabel);

    mainLayout->addStretch();

    // Bottom row: don't show again + Get Started
    auto* bottomLayout = new QHBoxLayout();

    auto* dontShowCheck = new QCheckBox("Don't show again");
    bottomLayout->addWidget(dontShowCheck);

    // Persist "Don't show again" on ANY exit path (New Scene, Open File, Recent, Get Started)
    connect(this, &QDialog::accepted, this, [dontShowCheck]() {
        if (dontShowCheck->isChecked()) {
            QSettings settings;
            settings.setValue("WelcomeScreen/dontShowAgain", true);
        }
    });

    bottomLayout->addStretch();

    auto* getStartedBtn = new QPushButton("Get Started");
    getStartedBtn->setMinimumHeight(32);
    connect(getStartedBtn, &QPushButton::clicked, this, [this]() {
        m_action = Dismissed;
        SentryReporter::addBreadcrumb("ui.action", "Welcome: Dismissed");
        accept();
    });
    bottomLayout->addWidget(getStartedBtn);

    mainLayout->addLayout(bottomLayout);
}
