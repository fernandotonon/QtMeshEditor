#include "LLMSettingsWidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>

LLMSettingsWidget::LLMSettingsWidget(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("AI Model Settings");
    setMinimumSize(500, 450);

    setupUI();
    loadCurrentSettings();
    updateModelList();
    updateRecommendedModelsList();
    updateStatus();

    // Connect to LLMManager signals
    LLMManager *manager = LLMManager::instance();
    connect(manager, &LLMManager::modelLoadCompleted, this, &LLMSettingsWidget::onModelLoadCompleted);
    connect(manager, &LLMManager::modelLoadError, this, &LLMSettingsWidget::onModelLoadError);
    connect(manager, &LLMManager::modelUnloaded, this, &LLMSettingsWidget::onModelUnloaded);
    connect(manager, &LLMManager::availableModelsChanged, this, &LLMSettingsWidget::updateModelList);

    // Connect to ModelDownloader signals
    ModelDownloader *downloader = ModelDownloader::instance();
    connect(downloader, &ModelDownloader::downloadProgressUpdated, this, &LLMSettingsWidget::onDownloadProgress);
    connect(downloader, &ModelDownloader::downloadCompleted, this, &LLMSettingsWidget::onDownloadCompleted);
    connect(downloader, &ModelDownloader::downloadError, this, &LLMSettingsWidget::onDownloadError);
}

void LLMSettingsWidget::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    m_tabWidget = new QTabWidget(this);

    QWidget *modelsTab = new QWidget();
    QWidget *settingsTab = new QWidget();
    QWidget *downloadTab = new QWidget();

    setupModelsTab(modelsTab);
    setupSettingsTab(settingsTab);
    setupDownloadTab(downloadTab);

    m_tabWidget->addTab(modelsTab, "Models");
    m_tabWidget->addTab(settingsTab, "Settings");
    m_tabWidget->addTab(downloadTab, "Download");

    mainLayout->addWidget(m_tabWidget);

    // Close button
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addStretch();
    QPushButton *closeButton = new QPushButton("Close", this);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    buttonLayout->addWidget(closeButton);
    mainLayout->addLayout(buttonLayout);
}

void LLMSettingsWidget::setupModelsTab(QWidget *parent)
{
    QVBoxLayout *layout = new QVBoxLayout(parent);

    // Directory selection
    QGroupBox *directoryGroup = new QGroupBox("Models Directory", parent);
    QHBoxLayout *dirLayout = new QHBoxLayout(directoryGroup);
    m_directoryEdit = new QLineEdit(directoryGroup);
    m_directoryEdit->setReadOnly(true);
    m_browseButton = new QPushButton("Browse...", directoryGroup);
    m_refreshButton = new QPushButton("Refresh", directoryGroup);
    dirLayout->addWidget(m_directoryEdit);
    dirLayout->addWidget(m_browseButton);
    dirLayout->addWidget(m_refreshButton);
    layout->addWidget(directoryGroup);

    // Model selection
    QGroupBox *modelGroup = new QGroupBox("Model Selection", parent);
    QVBoxLayout *modelLayout = new QVBoxLayout(modelGroup);

    QHBoxLayout *comboLayout = new QHBoxLayout();
    m_modelCombo = new QComboBox(modelGroup);
    m_modelCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    comboLayout->addWidget(new QLabel("Select Model:", modelGroup));
    comboLayout->addWidget(m_modelCombo);
    modelLayout->addLayout(comboLayout);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    m_loadButton = new QPushButton("Load Model", modelGroup);
    m_unloadButton = new QPushButton("Unload Model", modelGroup);
    m_unloadButton->setEnabled(false);
    buttonLayout->addWidget(m_loadButton);
    buttonLayout->addWidget(m_unloadButton);
    buttonLayout->addStretch();
    modelLayout->addLayout(buttonLayout);

    layout->addWidget(modelGroup);

    // Status
    QGroupBox *statusGroup = new QGroupBox("Status", parent);
    QVBoxLayout *statusLayout = new QVBoxLayout(statusGroup);
    m_statusLabel = new QLabel("No model loaded", statusGroup);
    m_statusLabel->setWordWrap(true);
    statusLayout->addWidget(m_statusLabel);
    layout->addWidget(statusGroup);

    layout->addStretch();

    // Connections
    connect(m_browseButton, &QPushButton::clicked, this, &LLMSettingsWidget::onBrowseDirectoryClicked);
    connect(m_refreshButton, &QPushButton::clicked, this, &LLMSettingsWidget::onRefreshModelsClicked);
    connect(m_loadButton, &QPushButton::clicked, this, &LLMSettingsWidget::onLoadModelClicked);
    connect(m_unloadButton, &QPushButton::clicked, this, &LLMSettingsWidget::onUnloadModelClicked);
}

void LLMSettingsWidget::setupSettingsTab(QWidget *parent)
{
    QVBoxLayout *layout = new QVBoxLayout(parent);

    QGroupBox *inferenceGroup = new QGroupBox("Inference Settings", parent);
    QFormLayout *formLayout = new QFormLayout(inferenceGroup);

    // Context size
    m_contextSizeSpinBox = new QSpinBox(inferenceGroup);
    m_contextSizeSpinBox->setRange(512, 32768);
    m_contextSizeSpinBox->setSingleStep(512);
    m_contextSizeSpinBox->setSuffix(" tokens");
    formLayout->addRow("Context Size:", m_contextSizeSpinBox);

    // Max tokens
    m_maxTokensSpinBox = new QSpinBox(inferenceGroup);
    m_maxTokensSpinBox->setRange(64, 8192);
    m_maxTokensSpinBox->setSingleStep(64);
    m_maxTokensSpinBox->setSuffix(" tokens");
    formLayout->addRow("Max Output Tokens:", m_maxTokensSpinBox);

    // Temperature
    QHBoxLayout *tempLayout = new QHBoxLayout();
    m_temperatureSpinBox = new QDoubleSpinBox(inferenceGroup);
    m_temperatureSpinBox->setRange(0.0, 2.0);
    m_temperatureSpinBox->setSingleStep(0.1);
    m_temperatureSpinBox->setDecimals(2);
    m_temperatureSlider = new QSlider(Qt::Horizontal, inferenceGroup);
    m_temperatureSlider->setRange(0, 200);
    tempLayout->addWidget(m_temperatureSpinBox);
    tempLayout->addWidget(m_temperatureSlider);
    formLayout->addRow("Temperature:", tempLayout);

    connect(m_temperatureSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            [this](double value) { m_temperatureSlider->setValue(static_cast<int>(value * 100)); });
    connect(m_temperatureSlider, &QSlider::valueChanged,
            [this](int value) { m_temperatureSpinBox->setValue(value / 100.0); });

    // Top P
    m_topPSpinBox = new QDoubleSpinBox(inferenceGroup);
    m_topPSpinBox->setRange(0.0, 1.0);
    m_topPSpinBox->setSingleStep(0.05);
    m_topPSpinBox->setDecimals(2);
    formLayout->addRow("Top P:", m_topPSpinBox);

    // Top K
    m_topKSpinBox = new QSpinBox(inferenceGroup);
    m_topKSpinBox->setRange(1, 100);
    formLayout->addRow("Top K:", m_topKSpinBox);

    // Repeat penalty
    m_repeatPenaltySpinBox = new QDoubleSpinBox(inferenceGroup);
    m_repeatPenaltySpinBox->setRange(1.0, 2.0);
    m_repeatPenaltySpinBox->setSingleStep(0.05);
    m_repeatPenaltySpinBox->setDecimals(2);
    formLayout->addRow("Repeat Penalty:", m_repeatPenaltySpinBox);

    layout->addWidget(inferenceGroup);

    // Hardware settings
    QGroupBox *hardwareGroup = new QGroupBox("Hardware Settings", parent);
    QFormLayout *hwFormLayout = new QFormLayout(hardwareGroup);

    // GPU layers
    m_gpuLayersSpinBox = new QSpinBox(hardwareGroup);
    m_gpuLayersSpinBox->setRange(0, 999);
    m_gpuLayersSpinBox->setSpecialValueText("CPU only");
    m_gpuLayersSpinBox->setToolTip("Number of layers to offload to GPU (99 = all layers)");
    hwFormLayout->addRow("GPU Layers:", m_gpuLayersSpinBox);

    // Threads
    m_threadsSpinBox = new QSpinBox(hardwareGroup);
    m_threadsSpinBox->setRange(0, 64);
    m_threadsSpinBox->setSpecialValueText("Auto");
    m_threadsSpinBox->setToolTip("Number of CPU threads (0 = auto-detect)");
    hwFormLayout->addRow("CPU Threads:", m_threadsSpinBox);

    layout->addWidget(hardwareGroup);

    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    m_applyButton = new QPushButton("Apply", parent);
    m_resetButton = new QPushButton("Reset to Defaults", parent);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_applyButton);
    buttonLayout->addWidget(m_resetButton);
    layout->addLayout(buttonLayout);

    layout->addStretch();

    // Connections
    connect(m_applyButton, &QPushButton::clicked, this, &LLMSettingsWidget::onApplySettings);
    connect(m_resetButton, &QPushButton::clicked, this, &LLMSettingsWidget::onResetDefaults);

    // Track changes
    connect(m_contextSizeSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &LLMSettingsWidget::onSettingsChanged);
    connect(m_maxTokensSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &LLMSettingsWidget::onSettingsChanged);
    connect(m_temperatureSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &LLMSettingsWidget::onSettingsChanged);
    connect(m_gpuLayersSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &LLMSettingsWidget::onSettingsChanged);
    connect(m_threadsSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &LLMSettingsWidget::onSettingsChanged);
    connect(m_topPSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &LLMSettingsWidget::onSettingsChanged);
    connect(m_topKSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &LLMSettingsWidget::onSettingsChanged);
    connect(m_repeatPenaltySpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &LLMSettingsWidget::onSettingsChanged);
}

void LLMSettingsWidget::setupDownloadTab(QWidget *parent)
{
    QVBoxLayout *layout = new QVBoxLayout(parent);

    // Recommended models list
    QGroupBox *modelsGroup = new QGroupBox("Recommended Models", parent);
    QVBoxLayout *modelsLayout = new QVBoxLayout(modelsGroup);

    m_recommendedModelsList = new QListWidget(modelsGroup);
    m_recommendedModelsList->setSelectionMode(QAbstractItemView::SingleSelection);
    modelsLayout->addWidget(m_recommendedModelsList);

    QHBoxLayout *downloadButtonLayout = new QHBoxLayout();
    m_downloadButton = new QPushButton("Download Selected", modelsGroup);
    m_cancelDownloadButton = new QPushButton("Cancel", modelsGroup);
    m_cancelDownloadButton->setEnabled(false);
    downloadButtonLayout->addWidget(m_downloadButton);
    downloadButtonLayout->addWidget(m_cancelDownloadButton);
    downloadButtonLayout->addStretch();
    modelsLayout->addLayout(downloadButtonLayout);

    layout->addWidget(modelsGroup);

    // Download progress
    QGroupBox *progressGroup = new QGroupBox("Download Progress", parent);
    QVBoxLayout *progressLayout = new QVBoxLayout(progressGroup);

    m_downloadProgressBar = new QProgressBar(progressGroup);
    m_downloadProgressBar->setRange(0, 100);
    m_downloadProgressBar->setValue(0);
    progressLayout->addWidget(m_downloadProgressBar);

    QHBoxLayout *statusLayout = new QHBoxLayout();
    m_downloadStatusLabel = new QLabel("Ready", progressGroup);
    m_downloadSpeedLabel = new QLabel("", progressGroup);
    statusLayout->addWidget(m_downloadStatusLabel);
    statusLayout->addStretch();
    statusLayout->addWidget(m_downloadSpeedLabel);
    progressLayout->addLayout(statusLayout);

    layout->addWidget(progressGroup);

    layout->addStretch();

    // Connections
    connect(m_downloadButton, &QPushButton::clicked, this, &LLMSettingsWidget::onDownloadModelClicked);
    connect(m_cancelDownloadButton, &QPushButton::clicked, this, &LLMSettingsWidget::onCancelDownloadClicked);
}

void LLMSettingsWidget::updateModelList()
{
    m_modelCombo->clear();
    m_directoryEdit->setText(LLMManager::instance()->modelsDirectory());

    QStringList models = LLMManager::instance()->availableModels();
    if (models.isEmpty()) {
        m_modelCombo->addItem("No models found - download one first");
        m_loadButton->setEnabled(false);
    } else {
        m_modelCombo->addItems(models);
        m_loadButton->setEnabled(true);
    }
}

void LLMSettingsWidget::updateRecommendedModelsList()
{
    m_recommendedModelsList->clear();

    QList<ModelInfo> models = LLMManager::instance()->getRecommendedModels();
    for (const ModelInfo &model : models) {
        QString text = QString("%1\n%2 (%3)")
                           .arg(model.name)
                           .arg(model.description)
                           .arg(formatFileSize(model.size));

        QListWidgetItem *item = new QListWidgetItem(text, m_recommendedModelsList);
        item->setData(Qt::UserRole, model.url);
        item->setData(Qt::UserRole + 1, model.fileName);
        item->setData(Qt::UserRole + 2, model.name);

        if (model.isDownloaded) {
            item->setText(text + " [Downloaded]");
            item->setForeground(QColor(0, 128, 0));
        }
    }
}

void LLMSettingsWidget::updateStatus()
{
    LLMManager *manager = LLMManager::instance();

    if (manager->isModelLoaded()) {
        m_statusLabel->setText(QString("Model loaded: %1").arg(manager->currentModelName()));
        m_statusLabel->setStyleSheet("color: green;");
        m_unloadButton->setEnabled(true);
        m_loadButton->setEnabled(true);
    } else {
        m_statusLabel->setText("No model loaded");
        m_statusLabel->setStyleSheet("color: gray;");
        m_unloadButton->setEnabled(false);
    }
}

void LLMSettingsWidget::loadCurrentSettings()
{
    LLMSettings settings = LLMManager::instance()->getSettings();

    m_contextSizeSpinBox->setValue(settings.contextSize);
    m_maxTokensSpinBox->setValue(settings.maxTokens);
    m_temperatureSpinBox->setValue(static_cast<double>(settings.temperature));
    m_gpuLayersSpinBox->setValue(settings.gpuLayers);
    m_threadsSpinBox->setValue(settings.threads);
    m_topPSpinBox->setValue(static_cast<double>(settings.topP));
    m_topKSpinBox->setValue(settings.topK);
    m_repeatPenaltySpinBox->setValue(static_cast<double>(settings.repeatPenalty));

    m_applyButton->setEnabled(false);
}

QString LLMSettingsWidget::formatFileSize(qint64 bytes) const
{
    if (bytes < 1024) {
        return QString("%1 B").arg(bytes);
    } else if (bytes < 1024 * 1024) {
        return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    } else if (bytes < 1024 * 1024 * 1024) {
        return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    } else {
        return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    }
}

void LLMSettingsWidget::onLoadModelClicked()
{
    QString modelName = m_modelCombo->currentText();
    if (modelName.isEmpty() || modelName.startsWith("No models")) {
        return;
    }

    m_loadButton->setEnabled(false);
    m_statusLabel->setText("Loading model...");
    m_statusLabel->setStyleSheet("color: orange;");

    LLMManager::instance()->loadModel(modelName);
}

void LLMSettingsWidget::onUnloadModelClicked()
{
    LLMManager::instance()->unloadModel();
}

void LLMSettingsWidget::onBrowseDirectoryClicked()
{
    QString dir = QFileDialog::getExistingDirectory(this, "Select Models Directory",
                                                     m_directoryEdit->text());
    if (!dir.isEmpty()) {
        LLMManager::instance()->setModelsDirectory(dir);
        m_directoryEdit->setText(dir);
        updateModelList();
        updateRecommendedModelsList();
    }
}

void LLMSettingsWidget::onRefreshModelsClicked()
{
    LLMManager::instance()->scanForModels();
    updateRecommendedModelsList();
}

void LLMSettingsWidget::onDownloadModelClicked()
{
    QListWidgetItem *item = m_recommendedModelsList->currentItem();
    if (!item) {
        QMessageBox::information(this, "No Model Selected", "Please select a model to download.");
        return;
    }

    QString url = item->data(Qt::UserRole).toString();
    QString fileName = item->data(Qt::UserRole + 1).toString();
    QString modelName = item->data(Qt::UserRole + 2).toString();

    QString destPath = QDir(LLMManager::instance()->modelsDirectory()).filePath(fileName);

    if (QFileInfo::exists(destPath)) {
        QMessageBox::StandardButton reply = QMessageBox::question(
            this, "File Exists",
            QString("The model %1 already exists. Do you want to re-download it?").arg(modelName),
            QMessageBox::Yes | QMessageBox::No);

        if (reply == QMessageBox::No) {
            return;
        }
        QFile::remove(destPath);
    }

    m_downloadButton->setEnabled(false);
    m_cancelDownloadButton->setEnabled(true);
    m_downloadStatusLabel->setText(QString("Downloading %1...").arg(modelName));

    ModelDownloader::instance()->startDownload(url, destPath, modelName);
}

void LLMSettingsWidget::onCancelDownloadClicked()
{
    ModelDownloader::instance()->cancelDownload();
    m_downloadButton->setEnabled(true);
    m_cancelDownloadButton->setEnabled(false);
    m_downloadStatusLabel->setText("Download canceled");
    m_downloadProgressBar->setValue(0);
    m_downloadSpeedLabel->clear();
}

void LLMSettingsWidget::onModelLoadCompleted(const QString &modelName)
{
    m_statusLabel->setText(QString("Model loaded: %1").arg(modelName));
    m_statusLabel->setStyleSheet("color: green;");
    m_loadButton->setEnabled(true);
    m_unloadButton->setEnabled(true);
}

void LLMSettingsWidget::onModelLoadError(const QString &error)
{
    m_statusLabel->setText(QString("Error: %1").arg(error));
    m_statusLabel->setStyleSheet("color: red;");
    m_loadButton->setEnabled(true);
    QMessageBox::warning(this, "Model Load Error", error);
}

void LLMSettingsWidget::onModelUnloaded()
{
    updateStatus();
}

void LLMSettingsWidget::onDownloadProgress(const QString &modelName, qint64 bytesReceived, qint64 bytesTotal)
{
    Q_UNUSED(modelName);
    if (bytesTotal > 0) {
        int progress = static_cast<int>((bytesReceived * 100) / bytesTotal);
        m_downloadProgressBar->setValue(progress);
        m_downloadStatusLabel->setText(QString("Downloaded: %1 / %2")
                                            .arg(formatFileSize(bytesReceived))
                                            .arg(formatFileSize(bytesTotal)));
    }

    float speed = ModelDownloader::instance()->downloadSpeed();
    if (speed > 0) {
        m_downloadSpeedLabel->setText(QString("%1/s").arg(formatFileSize(static_cast<qint64>(speed))));
    }
}

void LLMSettingsWidget::onDownloadCompleted(const QString &modelName, const QString &filePath)
{
    Q_UNUSED(filePath);
    m_downloadButton->setEnabled(true);
    m_cancelDownloadButton->setEnabled(false);
    m_downloadStatusLabel->setText(QString("Download completed: %1").arg(modelName));
    m_downloadProgressBar->setValue(100);
    m_downloadSpeedLabel->clear();

    LLMManager::instance()->scanForModels();
    updateModelList();
    updateRecommendedModelsList();

    QMessageBox::information(this, "Download Complete",
                             QString("Model %1 has been downloaded successfully.").arg(modelName));
}

void LLMSettingsWidget::onDownloadError(const QString &modelName, const QString &error)
{
    m_downloadButton->setEnabled(true);
    m_cancelDownloadButton->setEnabled(false);
    m_downloadStatusLabel->setText(QString("Error: %1").arg(error));
    m_downloadSpeedLabel->clear();

    QMessageBox::warning(this, "Download Error",
                         QString("Failed to download %1: %2").arg(modelName).arg(error));
}

void LLMSettingsWidget::onSettingsChanged()
{
    m_applyButton->setEnabled(true);
}

void LLMSettingsWidget::onApplySettings()
{
    LLMSettings settings;
    settings.contextSize = m_contextSizeSpinBox->value();
    settings.maxTokens = m_maxTokensSpinBox->value();
    settings.temperature = static_cast<float>(m_temperatureSpinBox->value());
    settings.gpuLayers = m_gpuLayersSpinBox->value();
    settings.threads = m_threadsSpinBox->value();
    settings.topP = static_cast<float>(m_topPSpinBox->value());
    settings.topK = m_topKSpinBox->value();
    settings.repeatPenalty = static_cast<float>(m_repeatPenaltySpinBox->value());

    LLMManager::instance()->setSettings(settings);
    m_applyButton->setEnabled(false);

    QMessageBox::information(this, "Settings Applied",
                             "Settings have been saved. They will take effect on the next model load.");
}

void LLMSettingsWidget::onResetDefaults()
{
    LLMSettings defaults;
    m_contextSizeSpinBox->setValue(defaults.contextSize);
    m_maxTokensSpinBox->setValue(defaults.maxTokens);
    m_temperatureSpinBox->setValue(static_cast<double>(defaults.temperature));
    m_gpuLayersSpinBox->setValue(defaults.gpuLayers);
    m_threadsSpinBox->setValue(defaults.threads);
    m_topPSpinBox->setValue(static_cast<double>(defaults.topP));
    m_topKSpinBox->setValue(defaults.topK);
    m_repeatPenaltySpinBox->setValue(static_cast<double>(defaults.repeatPenalty));

    m_applyButton->setEnabled(true);
}
