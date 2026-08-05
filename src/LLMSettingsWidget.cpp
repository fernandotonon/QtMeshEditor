// LCOV_EXCL_START — QDialog subclass with complex UI widgets, requires display

#include "LLMSettingsWidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QDesktopServices>
#include <QFileInfo>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>
#include "SentryReporter.h"

LLMSettingsWidget::LLMSettingsWidget(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("AI Model Settings");
    setMinimumSize(550, 500);

    setupUI();
    loadCurrentSettings();
    updateModelList();
    updateRecommendedModelsList();
    updateStatus();
    updateAIModelCatalogList();
#ifdef ENABLE_STABLE_DIFFUSION
    updateSDModelList();
    updateSDRecommendedModelsList();
    updateSDStatus();
#endif

    // Connect to LLMManager signals
    LLMManager *manager = LLMManager::instance();
    connect(manager, &LLMManager::modelLoadCompleted, this, &LLMSettingsWidget::onModelLoadCompleted);
    connect(manager, &LLMManager::modelLoadError, this, &LLMSettingsWidget::onModelLoadError);
    connect(manager, &LLMManager::modelUnloaded, this, &LLMSettingsWidget::onModelUnloaded);
    connect(manager, &LLMManager::availableModelsChanged, this, &LLMSettingsWidget::updateModelList);

#ifdef ENABLE_STABLE_DIFFUSION
    SDManager *sdManager = SDManager::instance();
    connect(sdManager, &SDManager::modelLoadCompleted, this, &LLMSettingsWidget::onSDModelLoadCompleted);
    connect(sdManager, &SDManager::modelLoadError, this, &LLMSettingsWidget::onSDModelLoadError);
    connect(sdManager, &SDManager::modelUnloaded, this, &LLMSettingsWidget::onSDModelUnloaded);
    connect(sdManager, &SDManager::availableModelsChanged, this, &LLMSettingsWidget::updateSDModelList);
#endif

    AIModelCatalog *catalog = AIModelCatalog::instance();
    connect(catalog, &AIModelCatalog::modelsChanged, this, &LLMSettingsWidget::updateAIModelCatalogList);
    connect(catalog, &AIModelCatalog::busyChanged, this, &LLMSettingsWidget::updateAIModelCatalogButtons);
    connect(catalog, &AIModelCatalog::statusMessageChanged, this, [this]() {
        m_aiModelCatalogStatusLabel->setText(AIModelCatalog::instance()->statusMessage());
    });

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
    QWidget *aiModelsTab = new QWidget();

    setupModelsTab(modelsTab);
    setupSettingsTab(settingsTab);
    setupDownloadTab(downloadTab);
    setupAIModelCatalogTab(aiModelsTab);

    m_tabWidget->addTab(modelsTab, "LLM Models");
    m_tabWidget->addTab(settingsTab, "Settings");
    m_tabWidget->addTab(downloadTab, "LLM Download");
    m_tabWidget->addTab(aiModelsTab, "QtMeshEditor Models");

#ifdef ENABLE_STABLE_DIFFUSION
    QWidget *sdModelsTab = new QWidget();
    QWidget *sdSettingsTab = new QWidget();

    setupSDModelsTab(sdModelsTab);
    setupSDSettingsTab(sdSettingsTab);

    m_tabWidget->addTab(sdModelsTab, "SD Models");
    m_tabWidget->addTab(sdSettingsTab, "SD Settings");
#endif

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
    m_loadFromFileButton = new QPushButton("Load from file...", modelGroup);
    m_unloadButton = new QPushButton("Unload Model", modelGroup);
    m_unloadButton->setEnabled(false);
    buttonLayout->addWidget(m_loadButton);
    buttonLayout->addWidget(m_loadFromFileButton);
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
    connect(m_loadFromFileButton, &QPushButton::clicked, this, &LLMSettingsWidget::onLoadFromFileClicked);
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

    if (OnnxRuntimeSettings::instance()->onnxAvailable()) {
        QGroupBox *onnxGroup = new QGroupBox("ONNX Models", parent);
        QVBoxLayout *onnxLayout = new QVBoxLayout(onnxGroup);

        m_onnxPreferGpuCheckBox =
            new QCheckBox("Prefer GPU for ONNX models (when available)", onnxGroup);
        m_onnxPreferGpuCheckBox->setToolTip(
            "UniRig, SkinTokens, PBR synthesis, image-to-3D, and other ONNX "
            "features. Takes effect on the next run (no Apply needed).");
        onnxLayout->addWidget(m_onnxPreferGpuCheckBox);

        m_onnxGpuNoteLabel = new QLabel(onnxGroup);
        m_onnxGpuNoteLabel->setWordWrap(true);
        m_onnxGpuNoteLabel->setStyleSheet("color: gray; font-size: 11px;");
        onnxLayout->addWidget(m_onnxGpuNoteLabel);

        layout->addWidget(onnxGroup);
    }

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
    if (m_onnxPreferGpuCheckBox) {
        connect(m_onnxPreferGpuCheckBox, &QCheckBox::toggled,
                this, &LLMSettingsWidget::onOnnxPreferGpuToggled);
    }
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

void LLMSettingsWidget::setupAIModelCatalogTab(QWidget *parent)
{
    QVBoxLayout *layout = new QVBoxLayout(parent);

    QGroupBox *modelsGroup = new QGroupBox("QtMeshEditor Models", parent);
    QVBoxLayout *modelsLayout = new QVBoxLayout(modelsGroup);

    m_aiModelCatalogList = new QListWidget(modelsGroup);
    m_aiModelCatalogList->setSelectionMode(QAbstractItemView::SingleSelection);
    modelsLayout->addWidget(m_aiModelCatalogList);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    m_aiModelCatalogDownloadButton = new QPushButton("Download", modelsGroup);
    m_aiModelCatalogDownloadAllButton = new QPushButton("Download All", modelsGroup);
    m_aiModelCatalogDeleteButton = new QPushButton("Delete", modelsGroup);
    m_aiModelCatalogDeleteAllButton = new QPushButton("Remove All", modelsGroup);
    m_aiModelCatalogRefreshButton = new QPushButton("Refresh", modelsGroup);
    m_aiModelCatalogOpenFolderButton = new QPushButton("Open Folder", modelsGroup);
    buttonLayout->addWidget(m_aiModelCatalogDownloadButton);
    buttonLayout->addWidget(m_aiModelCatalogDownloadAllButton);
    buttonLayout->addWidget(m_aiModelCatalogDeleteButton);
    buttonLayout->addWidget(m_aiModelCatalogDeleteAllButton);
    buttonLayout->addWidget(m_aiModelCatalogRefreshButton);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_aiModelCatalogOpenFolderButton);
    modelsLayout->addLayout(buttonLayout);

    layout->addWidget(modelsGroup);

    QGroupBox *statusGroup = new QGroupBox("Status", parent);
    QVBoxLayout *statusLayout = new QVBoxLayout(statusGroup);
    m_aiModelCatalogStatusLabel = new QLabel("Ready", statusGroup);
    m_aiModelCatalogStatusLabel->setWordWrap(true);
    statusLayout->addWidget(m_aiModelCatalogStatusLabel);
    layout->addWidget(statusGroup);

    connect(m_aiModelCatalogList, &QListWidget::currentItemChanged,
            this, &LLMSettingsWidget::updateAIModelCatalogButtons);
    connect(m_aiModelCatalogDownloadButton, &QPushButton::clicked,
            this, &LLMSettingsWidget::onAIModelCatalogDownloadClicked);
    connect(m_aiModelCatalogDownloadAllButton, &QPushButton::clicked,
            this, &LLMSettingsWidget::onAIModelCatalogDownloadAllClicked);
    connect(m_aiModelCatalogDeleteButton, &QPushButton::clicked,
            this, &LLMSettingsWidget::onAIModelCatalogDeleteClicked);
    connect(m_aiModelCatalogDeleteAllButton, &QPushButton::clicked,
            this, &LLMSettingsWidget::onAIModelCatalogDeleteAllClicked);
    connect(m_aiModelCatalogRefreshButton, &QPushButton::clicked,
            this, &LLMSettingsWidget::onAIModelCatalogRefreshClicked);
    connect(m_aiModelCatalogOpenFolderButton, &QPushButton::clicked,
            this, &LLMSettingsWidget::onAIModelCatalogOpenFolderClicked);
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

    if (m_onnxPreferGpuCheckBox) {
        OnnxRuntimeSettings* ort = OnnxRuntimeSettings::instance();
        ort->refreshGpuProviderStatus();
        ort->loadSettings();
        m_onnxPreferGpuCheckBox->blockSignals(true);
        m_onnxPreferGpuCheckBox->setChecked(ort->preferGpu());
        m_onnxPreferGpuCheckBox->blockSignals(false);
        if (m_onnxGpuNoteLabel)
            m_onnxGpuNoteLabel->setText(ort->gpuProviderNote());
    }

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

void LLMSettingsWidget::onLoadFromFileClicked()
{
    QString file = QFileDialog::getOpenFileName(
        nullptr,
        "Load AI Model",
        LLMManager::instance()->modelsDirectory(),
        "GGUF models (*.gguf);;Binary models (*.bin);;All files (*)"
    );
    if (file.isEmpty())
        return;
    m_loadButton->setEnabled(false);
    m_statusLabel->setText("Loading model...");
    m_statusLabel->setStyleSheet("color: orange;");
    LLMManager::instance()->loadModelFromPath(file);
}

void LLMSettingsWidget::onUnloadModelClicked()
{
    LLMManager::instance()->unloadModel();
}

void LLMSettingsWidget::onBrowseDirectoryClicked()
{
    // Use nullptr parent so the dialog is not shown as a sheet attached to
    // this exec()-modal window — that combination fails silently on macOS.
    QString dir = QFileDialog::getExistingDirectory(nullptr, "Select Models Directory",
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
    const bool catalogDownload = AIModelCatalog::instance()->busy()
        && !AIModelCatalog::instance()->activeModelId().isEmpty();
    if (catalogDownload) {
        updateAIModelCatalogList();
        return;
    }

    m_downloadButton->setEnabled(true);
    m_cancelDownloadButton->setEnabled(false);
    m_downloadStatusLabel->setText(QString("Download completed: %1").arg(modelName));
    m_downloadProgressBar->setValue(100);
    m_downloadSpeedLabel->clear();

    LLMManager::instance()->scanForModels();
    updateModelList();
    updateRecommendedModelsList();

#ifdef ENABLE_STABLE_DIFFUSION
    SDManager::instance()->scanForModels();
    updateSDModelList();
    updateSDRecommendedModelsList();
    m_sdDownloadButton->setEnabled(true);
#endif
    updateAIModelCatalogList();

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
                             "LLM settings have been saved. They will take effect on the next model load.");
}

void LLMSettingsWidget::onOnnxPreferGpuToggled(bool checked)
{
    OnnxRuntimeSettings* ort = OnnxRuntimeSettings::instance();
    ort->setPreferGpu(checked);
    if (m_onnxGpuNoteLabel)
        m_onnxGpuNoteLabel->setText(ort->gpuProviderNote());
    SentryReporter::addBreadcrumb(
        "ui.action",
        QStringLiteral("AI settings: ONNX prefer GPU %1").arg(checked ? "on" : "off"));
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

    if (m_onnxPreferGpuCheckBox) {
        m_onnxPreferGpuCheckBox->blockSignals(true);
        m_onnxPreferGpuCheckBox->setChecked(OnnxRuntimeSettings::defaultPreferGpu());
        m_onnxPreferGpuCheckBox->blockSignals(false);
        onOnnxPreferGpuToggled(m_onnxPreferGpuCheckBox->isChecked());
    }

    m_applyButton->setEnabled(true);
}

void LLMSettingsWidget::updateAIModelCatalogList()
{
    const QString selectedId = m_aiModelCatalogList->currentItem()
        ? m_aiModelCatalogList->currentItem()->data(Qt::UserRole).toString()
        : QString();

    m_aiModelCatalogList->clear();
    const QVariantList models = AIModelCatalog::instance()->models();
    int selectedRow = -1;
    for (int i = 0; i < models.size(); ++i) {
        const QVariantMap model = models[i].toMap();
        const bool downloaded = model.value("downloaded").toBool();
        const bool partial = model.value("partial").toBool();
        const bool available = model.value("available").toBool();
        const qint64 installedBytes = model.value("installedBytes").toLongLong();

        QString status = downloaded ? "Downloaded" : (partial ? "Partial" : "Not downloaded");
        QString text = QString("%1 [%2]\n%3 - %4 - %5/%6 files%7\n%8")
            .arg(model.value("name").toString())
            .arg(status)
            .arg(model.value("feature").toString())
            .arg(model.value("size").toString())
            .arg(model.value("presentFiles").toInt())
            .arg(model.value("totalFiles").toInt())
            .arg(installedBytes > 0
                 ? QString(" - %1 installed").arg(formatFileSize(installedBytes))
                 : QString())
            .arg(model.value("description").toString());
        if (!available)
            text += QString("\n%1").arg(model.value("buildRequirement").toString());

        QListWidgetItem *item = new QListWidgetItem(text, m_aiModelCatalogList);
        item->setData(Qt::UserRole, model.value("id").toString());
        item->setData(Qt::UserRole + 1, downloaded);
        item->setData(Qt::UserRole + 2, partial);
        item->setData(Qt::UserRole + 3, available);
        item->setData(Qt::UserRole + 4, model.value("name").toString());

        if (downloaded)
            item->setForeground(QColor(0, 128, 0));
        else if (partial)
            item->setForeground(QColor(180, 100, 0));
        else if (!available)
            item->setForeground(QColor(160, 0, 0));

        if (item->data(Qt::UserRole).toString() == selectedId)
            selectedRow = i;
    }

    if (m_aiModelCatalogList->count() > 0)
        m_aiModelCatalogList->setCurrentRow(selectedRow >= 0 ? selectedRow : 0);
    updateAIModelCatalogButtons();
}

void LLMSettingsWidget::updateAIModelCatalogButtons()
{
    QListWidgetItem *item = m_aiModelCatalogList->currentItem();
    const bool busy = AIModelCatalog::instance()->busy();
    const bool hasItem = item != nullptr;
    const bool downloaded = hasItem && item->data(Qt::UserRole + 1).toBool();
    const bool partial = hasItem && item->data(Qt::UserRole + 2).toBool();
    const bool available = hasItem && item->data(Qt::UserRole + 3).toBool();

    m_aiModelCatalogDownloadButton->setEnabled(hasItem && available && !downloaded && !busy);
    m_aiModelCatalogDeleteButton->setEnabled(hasItem && (downloaded || partial) && !busy);
    m_aiModelCatalogDownloadAllButton->setEnabled(!busy);
    m_aiModelCatalogDeleteAllButton->setEnabled(!busy);
    m_aiModelCatalogRefreshButton->setEnabled(!busy);
    m_aiModelCatalogOpenFolderButton->setEnabled(true);
}

void LLMSettingsWidget::onAIModelCatalogDownloadClicked()
{
    QListWidgetItem *item = m_aiModelCatalogList->currentItem();
    if (!item)
        return;
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Download QtMeshEditor model"));
    AIModelCatalog::instance()->downloadModel(item->data(Qt::UserRole).toString());
    updateAIModelCatalogButtons();
}

void LLMSettingsWidget::onAIModelCatalogDownloadAllClicked()
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Download all QtMeshEditor models"));
    AIModelCatalog::instance()->downloadAllModels();
    updateAIModelCatalogButtons();
}

void LLMSettingsWidget::onAIModelCatalogDeleteClicked()
{
    QListWidgetItem *item = m_aiModelCatalogList->currentItem();
    if (!item)
        return;

    const QString modelName = item->data(Qt::UserRole + 4).toString();
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "Delete Model Files",
        QString("Delete downloaded files for %1?").arg(modelName),
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::No)
        return;

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Delete QtMeshEditor model files"));
    AIModelCatalog::instance()->deleteModel(item->data(Qt::UserRole).toString());
}

void LLMSettingsWidget::onAIModelCatalogDeleteAllClicked()
{
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "Remove All Model Files",
        "Remove all downloaded QtMeshEditor model files?",
        QMessageBox::Yes | QMessageBox::No);
    if (reply == QMessageBox::No)
        return;

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Delete all QtMeshEditor model files"));
    AIModelCatalog::instance()->deleteAllModels();
}

void LLMSettingsWidget::onAIModelCatalogRefreshClicked()
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Refresh QtMeshEditor model catalog"));
    AIModelCatalog::instance()->refresh();
}

void LLMSettingsWidget::onAIModelCatalogOpenFolderClicked()
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Open QtMeshEditor models folder"));
    const QString folderPath = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                   .filePath(QStringLiteral("ai_models"));
    QDir dir;
    if (!dir.mkpath(folderPath)) {
        QMessageBox::warning(this, "Open Folder Failed",
                             QString("Could not create %1.").arg(folderPath));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(folderPath))) {
        QMessageBox::warning(this, "Open Folder Failed",
                             QString("Could not open %1.").arg(folderPath));
    }
}

// ============ SD Models Tab ============

#ifdef ENABLE_STABLE_DIFFUSION
void LLMSettingsWidget::setupSDModelsTab(QWidget *parent)
{
    QVBoxLayout *layout = new QVBoxLayout(parent);

    // Status
    QGroupBox *statusGroup = new QGroupBox("SD Model Status", parent);
    QVBoxLayout *statusLayout = new QVBoxLayout(statusGroup);
    m_sdStatusLabel = new QLabel("No SD model loaded", statusGroup);
    m_sdStatusLabel->setWordWrap(true);
    statusLayout->addWidget(m_sdStatusLabel);
    layout->addWidget(statusGroup);

    // Model selection
    QGroupBox *modelGroup = new QGroupBox("Available SD Models", parent);
    QVBoxLayout *modelLayout = new QVBoxLayout(modelGroup);

    QHBoxLayout *comboLayout = new QHBoxLayout();
    m_sdModelCombo = new QComboBox(modelGroup);
    m_sdModelCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    comboLayout->addWidget(new QLabel("Select Model:", modelGroup));
    comboLayout->addWidget(m_sdModelCombo);
    modelLayout->addLayout(comboLayout);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    m_sdLoadButton = new QPushButton("Load Model", modelGroup);
    m_sdUnloadButton = new QPushButton("Unload", modelGroup);
    m_sdUnloadButton->setEnabled(false);
    m_sdRefreshButton = new QPushButton("Refresh", modelGroup);
    buttonLayout->addWidget(m_sdLoadButton);
    buttonLayout->addWidget(m_sdUnloadButton);
    buttonLayout->addWidget(m_sdRefreshButton);
    buttonLayout->addStretch();

    QPushButton *openFolderBtn = new QPushButton("Open Folder", modelGroup);
    connect(openFolderBtn, &QPushButton::clicked, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(SDManager::instance()->modelsDirectory()));
    });
    buttonLayout->addWidget(openFolderBtn);
    modelLayout->addLayout(buttonLayout);

    // Directory
    QHBoxLayout *dirLayout = new QHBoxLayout();
    dirLayout->addWidget(new QLabel("Models Directory:", modelGroup));
    m_sdDirectoryEdit = new QLineEdit(modelGroup);
    m_sdDirectoryEdit->setReadOnly(true);
    dirLayout->addWidget(m_sdDirectoryEdit);
    modelLayout->addLayout(dirLayout);

    layout->addWidget(modelGroup);

    // Recommended models
    QGroupBox *recommendedGroup = new QGroupBox("Recommended SD Models", parent);
    QVBoxLayout *recLayout = new QVBoxLayout(recommendedGroup);

    m_sdRecommendedModelsList = new QListWidget(recommendedGroup);
    m_sdRecommendedModelsList->setSelectionMode(QAbstractItemView::SingleSelection);
    recLayout->addWidget(m_sdRecommendedModelsList);

    QHBoxLayout *dlLayout = new QHBoxLayout();
    m_sdDownloadButton = new QPushButton("Download Selected", recommendedGroup);
    dlLayout->addWidget(m_sdDownloadButton);
    dlLayout->addStretch();
    recLayout->addLayout(dlLayout);

    layout->addWidget(recommendedGroup);

    layout->addStretch();

    // Connections
    connect(m_sdLoadButton, &QPushButton::clicked, this, &LLMSettingsWidget::onSDLoadModelClicked);
    connect(m_sdUnloadButton, &QPushButton::clicked, this, &LLMSettingsWidget::onSDUnloadModelClicked);
    connect(m_sdRefreshButton, &QPushButton::clicked, this, &LLMSettingsWidget::onSDRefreshModelsClicked);
    connect(m_sdDownloadButton, &QPushButton::clicked, this, &LLMSettingsWidget::onSDDownloadModelClicked);
}

void LLMSettingsWidget::setupSDSettingsTab(QWidget *parent)
{
    QVBoxLayout *layout = new QVBoxLayout(parent);

    QGroupBox *genGroup = new QGroupBox("Generation Settings", parent);
    QFormLayout *formLayout = new QFormLayout(genGroup);

    m_sdStepsSpinBox = new QSpinBox(genGroup);
    m_sdStepsSpinBox->setRange(1, 100);
    formLayout->addRow("Steps:", m_sdStepsSpinBox);

    m_sdCfgScaleSpinBox = new QDoubleSpinBox(genGroup);
    m_sdCfgScaleSpinBox->setRange(1.0, 20.0);
    m_sdCfgScaleSpinBox->setSingleStep(0.5);
    m_sdCfgScaleSpinBox->setDecimals(1);
    formLayout->addRow("CFG Scale:", m_sdCfgScaleSpinBox);

    m_sdWidthSpinBox = new QSpinBox(genGroup);
    m_sdWidthSpinBox->setRange(256, 1024);
    m_sdWidthSpinBox->setSingleStep(128);
    m_sdWidthSpinBox->setSuffix(" px");
    formLayout->addRow("Width:", m_sdWidthSpinBox);

    m_sdHeightSpinBox = new QSpinBox(genGroup);
    m_sdHeightSpinBox->setRange(256, 1024);
    m_sdHeightSpinBox->setSingleStep(128);
    m_sdHeightSpinBox->setSuffix(" px");
    formLayout->addRow("Height:", m_sdHeightSpinBox);

    m_sdNegativePromptEdit = new QLineEdit(genGroup);
    m_sdNegativePromptEdit->setPlaceholderText("blurry, low quality...");
    formLayout->addRow("Negative Prompt:", m_sdNegativePromptEdit);

    layout->addWidget(genGroup);

    // Apply button
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    m_sdApplyButton = new QPushButton("Apply", parent);
    m_sdApplyButton->setEnabled(false);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_sdApplyButton);
    layout->addLayout(buttonLayout);

    layout->addStretch();

    // Load current settings
    SDSettings sdSettings = SDManager::instance()->getSettings();
    m_sdStepsSpinBox->setValue(sdSettings.steps);
    m_sdCfgScaleSpinBox->setValue(static_cast<double>(sdSettings.cfgScale));
    m_sdWidthSpinBox->setValue(sdSettings.width);
    m_sdHeightSpinBox->setValue(sdSettings.height);
    m_sdNegativePromptEdit->setText(sdSettings.negativePrompt);

    // Track changes
    connect(m_sdStepsSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &LLMSettingsWidget::onSDSettingsChanged);
    connect(m_sdCfgScaleSpinBox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &LLMSettingsWidget::onSDSettingsChanged);
    connect(m_sdWidthSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &LLMSettingsWidget::onSDSettingsChanged);
    connect(m_sdHeightSpinBox, QOverload<int>::of(&QSpinBox::valueChanged), this, &LLMSettingsWidget::onSDSettingsChanged);
    connect(m_sdNegativePromptEdit, &QLineEdit::textChanged, this, &LLMSettingsWidget::onSDSettingsChanged);
    connect(m_sdApplyButton, &QPushButton::clicked, this, &LLMSettingsWidget::onSDApplySettings);
}

void LLMSettingsWidget::updateSDModelList()
{
    m_sdModelCombo->clear();
    m_sdDirectoryEdit->setText(SDManager::instance()->modelsDirectory());

    QStringList models = SDManager::instance()->availableModels();
    if (models.isEmpty()) {
        m_sdModelCombo->addItem("No SD models found - download one first");
        m_sdLoadButton->setEnabled(false);
    } else {
        m_sdModelCombo->addItems(models);
        m_sdLoadButton->setEnabled(true);
    }
}

void LLMSettingsWidget::updateSDRecommendedModelsList()
{
    m_sdRecommendedModelsList->clear();

    QVariantList models = SDManager::instance()->getRecommendedModelsInfo();
    for (const QVariant &v : models) {
        QVariantMap model = v.toMap();
        QString text = QString("%1\n%2 (%3)")
                           .arg(model["name"].toString())
                           .arg(model["description"].toString())
                           .arg(formatFileSize(model["size"].toLongLong()));

        QListWidgetItem *item = new QListWidgetItem(text, m_sdRecommendedModelsList);
        item->setData(Qt::UserRole, model["url"]);
        item->setData(Qt::UserRole + 1, model["fileName"]);
        item->setData(Qt::UserRole + 2, model["name"]);

        if (model["isDownloaded"].toBool()) {
            item->setText(text + " [Downloaded]");
            item->setForeground(QColor(0, 128, 0));
        }
    }
}

void LLMSettingsWidget::updateSDStatus()
{
    SDManager *manager = SDManager::instance();

    if (manager->isModelLoaded()) {
        m_sdStatusLabel->setText(QString("SD Model loaded: %1").arg(manager->currentModelName()));
        m_sdStatusLabel->setStyleSheet("color: green;");
        m_sdUnloadButton->setEnabled(true);
        m_sdLoadButton->setEnabled(true);
    } else {
        m_sdStatusLabel->setText("No SD model loaded");
        m_sdStatusLabel->setStyleSheet("color: gray;");
        m_sdUnloadButton->setEnabled(false);
    }
}

void LLMSettingsWidget::onSDLoadModelClicked()
{
    QString modelName = m_sdModelCombo->currentText();
    if (modelName.isEmpty() || modelName.startsWith("No SD")) return;

    m_sdLoadButton->setEnabled(false);
    m_sdStatusLabel->setText("Loading SD model...");
    m_sdStatusLabel->setStyleSheet("color: orange;");

    SDManager::instance()->loadModel(modelName);
}

void LLMSettingsWidget::onSDUnloadModelClicked()
{
    SDManager::instance()->unloadModel();
}

void LLMSettingsWidget::onSDRefreshModelsClicked()
{
    SDManager::instance()->scanForModels();
    updateSDRecommendedModelsList();
}

void LLMSettingsWidget::onSDDownloadModelClicked()
{
    QListWidgetItem *item = m_sdRecommendedModelsList->currentItem();
    if (!item) {
        QMessageBox::information(this, "No Model Selected", "Please select an SD model to download.");
        return;
    }

    QString url = item->data(Qt::UserRole).toString();
    QString fileName = item->data(Qt::UserRole + 1).toString();
    QString modelName = item->data(Qt::UserRole + 2).toString();

    QString destPath = QDir(SDManager::instance()->modelsDirectory()).filePath(fileName);

    if (QFileInfo::exists(destPath)) {
        QMessageBox::StandardButton reply = QMessageBox::question(
            this, "File Exists",
            QString("The SD model %1 already exists. Re-download?").arg(modelName),
            QMessageBox::Yes | QMessageBox::No);
        if (reply == QMessageBox::No) return;
        QFile::remove(destPath);
    }

    m_sdDownloadButton->setEnabled(false);
    m_downloadStatusLabel->setText(QString("Downloading %1...").arg(modelName));

    ModelDownloader::instance()->startDownload(url, destPath, modelName);
}

void LLMSettingsWidget::onSDModelLoadCompleted(const QString &modelName)
{
    m_sdStatusLabel->setText(QString("SD Model loaded: %1").arg(modelName));
    m_sdStatusLabel->setStyleSheet("color: green;");
    m_sdLoadButton->setEnabled(true);
    m_sdUnloadButton->setEnabled(true);
}

void LLMSettingsWidget::onSDModelLoadError(const QString &error)
{
    m_sdStatusLabel->setText(QString("Error: %1").arg(error));
    m_sdStatusLabel->setStyleSheet("color: red;");
    m_sdLoadButton->setEnabled(true);
    QMessageBox::warning(this, "SD Model Load Error", error);
}

void LLMSettingsWidget::onSDModelUnloaded()
{
    updateSDStatus();
}

void LLMSettingsWidget::onSDSettingsChanged()
{
    m_sdApplyButton->setEnabled(true);
}

void LLMSettingsWidget::onSDApplySettings()
{
    SDSettings settings = SDManager::instance()->getSettings();
    settings.steps = m_sdStepsSpinBox->value();
    settings.cfgScale = static_cast<float>(m_sdCfgScaleSpinBox->value());
    settings.width = m_sdWidthSpinBox->value();
    settings.height = m_sdHeightSpinBox->value();
    settings.negativePrompt = m_sdNegativePromptEdit->text();

    SDManager::instance()->setSettings(settings);
    m_sdApplyButton->setEnabled(false);

    QMessageBox::information(this, "Settings Applied", "SD settings have been saved.");
}
#endif

// LCOV_EXCL_STOP
