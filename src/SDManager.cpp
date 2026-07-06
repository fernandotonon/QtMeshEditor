#include "SDManager.h"
#include "GamificationManager.h"
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDebug>
#include <QFileInfo>
#include <QTimer>
#include <QDateTime>
#include <QUuid>
#include <QRegularExpression>

SDManager* SDManager::s_instance = nullptr;

SDManager* SDManager::instance()
{
    if (!s_instance) {
        s_instance = new SDManager();
    }
    return s_instance;
}

SDManager* SDManager::qmlInstance(QQmlEngine *engine, QJSEngine *scriptEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(scriptEngine)
    return instance();
}

// LCOV_EXCL_START — constructor/destructor require SDWorker which needs sd.cpp GPU backend
SDManager::SDManager(QObject *parent)
    : QObject(parent)
{
    m_modelsDirectory = getDefaultModelsDirectory();
    populateRecommendedModels();
    loadSettings();
    initializeWorkerThread();
    scanForModels();

    // Auto-load model after event loop starts
    QTimer::singleShot(1000, this, &SDManager::tryAutoLoadModel);
}

SDManager::~SDManager()
{
    shutdownWorkerThread();
}

void SDManager::initializeWorkerThread()
{
    m_workerThread = new QThread(this);
    m_worker = new SDWorker();
    m_worker->moveToThread(m_workerThread);

    // Connect worker signals
    connect(m_worker, &SDWorker::modelLoaded, this, &SDManager::onWorkerModelLoaded);
    connect(m_worker, &SDWorker::modelLoadError, this, &SDManager::onWorkerModelLoadError);
    connect(m_worker, &SDWorker::modelUnloaded, this, &SDManager::onWorkerModelUnloaded);
    connect(m_worker, &SDWorker::generationStarted, this, &SDManager::onWorkerGenerationStarted);
    connect(m_worker, &SDWorker::generationProgress, this, &SDManager::onWorkerGenerationProgress);
    connect(m_worker, &SDWorker::generationCompleted, this, &SDManager::onWorkerGenerationCompleted);
    connect(m_worker, &SDWorker::generationError, this, &SDManager::onWorkerGenerationError);
    connect(m_worker, &SDWorker::generationStopped, this, &SDManager::onWorkerGenerationStopped);

    // Cleanup on thread finished
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_workerThread->start();
    qDebug() << "SDManager: Worker thread started";
}
// LCOV_EXCL_STOP

// LCOV_EXCL_START — worker thread requires sd.cpp GPU backend
void SDManager::shutdownWorkerThread()
{
    if (m_worker) {
        m_worker->requestStop();
    }

    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait();
        qDebug() << "SDManager: Worker thread stopped";
    }
}
// LCOV_EXCL_STOP

QString SDManager::getDefaultModelsDirectory() const
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath("sd_models");
}

void SDManager::populateRecommendedModels()
{
    m_recommendedModels.clear();

    m_recommendedModels.append({
        "SD 1.5 (Safetensors)",
        "v1-5-pruned-emaonly.safetensors",
        "https://huggingface.co/stable-diffusion-v1-5/stable-diffusion-v1-5/resolve/main/v1-5-pruned-emaonly.safetensors",
        "Stable Diffusion 1.5 official. Good all-round quality. ~4.3GB",
        4265380512,
        false
    });

    m_recommendedModels.append({
        "SDXL Turbo (FP16)",
        "sd_xl_turbo_1.0_fp16.safetensors",
        "https://huggingface.co/stabilityai/sdxl-turbo/resolve/main/sd_xl_turbo_1.0_fp16.safetensors",
        "SDXL Turbo - fast generation, 4-12 steps. ~6.5GB",
        6938081905,
        false
    });

    // Issue #403: ControlNet depth model for mesh-aware texture
    // generation. NOT a base model — it pairs WITH SD 1.5 as the
    // conditioning network. Selected via the "Generate Texture from
    // Mesh…" dialog's ControlNet field, not the base-model dropdown.
    // lllyasviel's converted fp16 safetensors (the sd.cpp-compatible
    // single-file format).
    {
        SDModelInfo cn;
        cn.name        = "ControlNet Depth (SD 1.5)";
        cn.fileName    = "control_v11f1p_sd15_depth_fp16.safetensors";
        cn.url         = "https://huggingface.co/comfyanonymous/ControlNet-v1-1_fp16_safetensors/resolve/main/control_v11f1p_sd15_depth_fp16.safetensors";
        cn.description = "Depth ControlNet for mesh-aware texture generation. Pairs with SD 1.5 (not SDXL). ~723MB";
        cn.size        = 722601104;
        cn.isDownloaded = false;
        cn.kind        = QStringLiteral("controlnet");
        m_recommendedModels.append(cn);
    }
}

bool SDManager::isModelLoaded() const
{
    return m_worker && m_worker->isModelLoaded();
}

QStringList SDManager::availableModels() const
{
    return m_availableModels;
}

void SDManager::setModelsDirectory(const QString &dir)
{
    if (m_modelsDirectory != dir) {
        m_modelsDirectory = dir;
        QDir().mkpath(dir);
        scanForModels();
        saveSettings();
        emit modelsDirectoryChanged();
    }
}

SDSettings SDManager::getSettings() const
{
    return m_settings;
}

void SDManager::setSettings(const SDSettings &settings)
{
    m_settings = settings;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [this, settings]() {
            m_worker->setSettings(settings);
        }, Qt::QueuedConnection);
    }
    saveSettings();
}

bool SDManager::isGenerating() const
{
    return m_worker && m_worker->isGenerating();
}

// Settings setters for QML
void SDManager::setImageWidth(int value)
{
    if (m_settings.width != value) {
        m_settings.width = value;
        saveSettings();
        emit settingsChanged();
    }
}

void SDManager::setImageHeight(int value)
{
    if (m_settings.height != value) {
        m_settings.height = value;
        saveSettings();
        emit settingsChanged();
    }
}

void SDManager::setSteps(int value)
{
    if (m_settings.steps != value) {
        m_settings.steps = value;
        saveSettings();
        emit settingsChanged();
    }
}

void SDManager::setCfgScale(float value)
{
    if (m_settings.cfgScale != value) {
        m_settings.cfgScale = value;
        saveSettings();
        emit settingsChanged();
    }
}

void SDManager::setNegativePrompt(const QString &value)
{
    if (m_settings.negativePrompt != value) {
        m_settings.negativePrompt = value;
        saveSettings();
        emit settingsChanged();
    }
}

void SDManager::setAutoLoadModel(bool value)
{
    if (m_autoLoadModel != value) {
        m_autoLoadModel = value;
        saveSettings();
        emit autoLoadModelChanged();
    }
}

void SDManager::tryAutoLoadModel()
{
    if (!m_autoLoadModel || m_lastModelName.isEmpty()) {
        return;
    }

    if (!modelFileExists(m_lastModelName)) {
        qDebug() << "SDManager: Auto-load model not found:" << m_lastModelName;
        return;
    }

    // LCOV_EXCL_START
    qDebug() << "SDManager: Auto-loading model:" << m_lastModelName;
    loadModel(m_lastModelName);
    // LCOV_EXCL_STOP
}

QString SDManager::enhanceTexturePrompt(const QString &prompt)
{
    // Add 3D texture-specific keywords to improve generation quality
    QString enhanced = prompt.trimmed();
    QString lower = enhanced.toLower();

    // Add texture-specific suffix if not already present
    QStringList textureKeywords = {"seamless", "tileable", "texture", "material", "surface"};
    bool hasTextureKeyword = false;
    for (const auto &kw : textureKeywords) {
        if (lower.contains(kw)) {
            hasTextureKeyword = true;
            break;
        }
    }

    if (!hasTextureKeyword) {
        enhanced += ", seamless tileable texture";
    }

    // Add quality keywords
    if (!lower.contains("photorealistic") && !lower.contains("realistic")) {
        enhanced += ", photorealistic";
    }
    if (!lower.contains("detail")) {
        enhanced += ", highly detailed";
    }

    // Add 3D-specific keywords
    enhanced += ", flat surface, top-down view, even lighting, normal map compatible";

    return enhanced;
}

QString SDManager::getTextureNegativePrompt()
{
    return "3d render, perspective, objects, people, faces, animals, text, watermark, "
           "logo, border, frame, vignette, uneven lighting, shadow, depth of field, "
           "blurry, low quality, distorted, cartoon, anime";
}

void SDManager::loadModel(const QString &modelName)
{
    QString modelPath = getModelFilePath(modelName);
    if (modelPath.isEmpty() || !QFileInfo::exists(modelPath)) {
        emit modelLoadError(QString("SD model file not found: %1").arg(modelName));
        return;
    }

    // LCOV_EXCL_START — requires a real SD model file
    m_isLoading = true;
    emit isLoadingChanged();
    emit modelLoadStarted(modelName);
    m_currentModelName = modelName;

    // Auto-detect model type and adjust default settings
    QString lowerName = modelName.toLower() + modelPath.toLower();
    if (lowerName.contains("turbo")) {
        m_settings.steps = 12;
        m_settings.cfgScale = 2.0f;
        m_settings.negativePrompt = "blurry, low quality, distorted, simple, cartoon";
        emit settingsChanged();
        qDebug() << "SDManager: Detected Turbo model, set 12 steps, CFG 2.0";
    } else if (lowerName.contains("lightning")) {
        m_settings.steps = 8;
        m_settings.cfgScale = 2.0f;
        emit settingsChanged();
        qDebug() << "SDManager: Detected Lightning model, set 8 steps, CFG 2.0";
    } else if (lowerName.contains("sdxl") || lowerName.contains("sd_xl")) {
        m_settings.steps = 30;
        m_settings.cfgScale = 7.0f;
        emit settingsChanged();
        qDebug() << "SDManager: Detected SDXL model, set 30 steps, CFG 7.0";
    }

    QMetaObject::invokeMethod(m_worker, [this, modelPath]() {
        m_worker->setSettings(m_settings);
        m_worker->loadModel(modelPath);
    }, Qt::QueuedConnection);
    // LCOV_EXCL_STOP
}

// LCOV_EXCL_START — requires sd.cpp worker thread with GPU backend
void SDManager::unloadModel()
{
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [this]() {
            m_worker->unloadModel();
        }, Qt::QueuedConnection);
    }
}
// LCOV_EXCL_STOP

void SDManager::scanForModels()
{
    m_availableModels.clear();

    QDir modelsDir(m_modelsDirectory);
    if (!modelsDir.exists()) {
        modelsDir.mkpath(".");
    }

    QStringList filters;
    filters << "*.safetensors" << "*.ckpt" << "*.gguf";
    QFileInfoList files = modelsDir.entryInfoList(filters, QDir::Files);

    // Collect ControlNet filenames so they don't show up as base
    // models (they can't be loaded as a generation context). Use
    // both the known recommended-list filenames and a name heuristic
    // ("controlnet" / "control_v" — the lllyasviel naming).
    QStringList controlNetFiles;
    for (const SDModelInfo &info : m_recommendedModels)
        if (info.kind == QStringLiteral("controlnet"))
            controlNetFiles << info.fileName;

    for (const QFileInfo &file : files) {
        const QString lower = file.fileName().toLower();
        const bool isControlNet =
            controlNetFiles.contains(file.fileName())
            || lower.contains("controlnet")
            || lower.startsWith("control_v");
        if (isControlNet) continue;  // not a base model
        m_availableModels.append(file.completeBaseName());
    }

    // Update recommended models download status
    for (int i = 0; i < m_recommendedModels.size(); ++i) {
        QString filePath = modelsDir.filePath(m_recommendedModels[i].fileName);
        m_recommendedModels[i].isDownloaded = QFileInfo::exists(filePath);
    }

    qDebug() << "SDManager: Found" << m_availableModels.size() << "SD models in" << m_modelsDirectory;
    emit availableModelsChanged();
}

QString SDManager::generateOutputPath() const
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir outputDir(QDir(dataPath).filePath("generated_textures"));
    if (!outputDir.exists()) {
        outputDir.mkpath(".");
    }

    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString uuid = QUuid::createUuid().toString(QUuid::Id128).left(8);
    return outputDir.filePath(QString("texture_%1_%2.png").arg(timestamp, uuid));
}

void SDManager::generateTexture(const QString &prompt, int width, int height, const QString &outputFileName)
{
    if (!isModelLoaded()) {
        emit generationError("No SD model loaded. Please load a model first.");
        return;
    }

    GamificationManager::noteFeature(QStringLiteral("stable_diffusion"));

    // LCOV_EXCL_START — requires a loaded SD model
    // Override settings if width/height provided
    if (width > 0) {
        m_settings.width = width;
    }
    if (height > 0) {
        m_settings.height = height;
    }

    QString outputPath;
    if (!outputFileName.isEmpty()) {
        // Use the specified filename in the generated textures directory
        QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir outputDir(QDir(dataPath).filePath("generated_textures"));
        if (!outputDir.exists()) {
            outputDir.mkpath(".");
        }
        QString fileName = outputFileName.trimmed();
        fileName = QFileInfo(fileName).fileName(); // Strip path separators to prevent path traversal
        // If it already ends with .png, keep it; otherwise replace/add .png
        if (!fileName.endsWith(".png", Qt::CaseInsensitive) &&
            !fileName.endsWith(".jpg", Qt::CaseInsensitive)) {
            fileName.replace(QRegularExpression(R"(\.\w+$)"), "");
            fileName += ".png";
        }
        outputPath = outputDir.filePath(fileName);
    } else {
        outputPath = generateOutputPath();
    }

    // Enhance prompt for 3D texture generation
    QString enhancedPrompt = enhanceTexturePrompt(prompt);

    // Use texture-specific negative prompt if user hasn't set a custom one
    SDSettings genSettings = m_settings;
    if (genSettings.negativePrompt.isEmpty() ||
        genSettings.negativePrompt == "blurry, low quality, distorted, simple, cartoon") {
        genSettings.negativePrompt = getTextureNegativePrompt();
    }

    QMetaObject::invokeMethod(m_worker, [this, enhancedPrompt, outputPath, genSettings]() {
        m_worker->setSettings(genSettings);
        m_worker->generateTexture(enhancedPrompt, outputPath);
    }, Qt::QueuedConnection);
    // LCOV_EXCL_STOP
}

void SDManager::generateMeshTexture(const QString &prompt,
                                    const QImage &controlImage,
                                    const QString &controlNetPath,
                                    float controlStrength,
                                    const QString &outputFileName,
                                    int width,
                                    int height,
                                    int64_t seed)
{
    if (!isModelLoaded()) {
        emit generationError("No SD model loaded. Please load a model first.");
        return;
    }

    // LCOV_EXCL_START — requires a loaded SD model + worker
    QString outputPath;
    if (!outputFileName.isEmpty()) {
        QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir outputDir(QDir(dataPath).filePath("generated_textures"));
        if (!outputDir.exists()) outputDir.mkpath(".");
        QString fileName = QFileInfo(outputFileName.trimmed()).fileName();
        // The worker always encodes PNG, so force a .png suffix — a .jpg
        // name would produce a PNG payload with a misleading extension.
        if (!fileName.endsWith(".png", Qt::CaseInsensitive)) {
            fileName.replace(QRegularExpression(R"(\.\w+$)"), "");
            fileName += ".png";
        }
        outputPath = outputDir.filePath(fileName);
    } else {
        outputPath = generateOutputPath();
    }

    QString enhancedPrompt = enhanceTexturePrompt(prompt);

    SDSettings genSettings = m_settings;
    if (genSettings.negativePrompt.isEmpty() ||
        genSettings.negativePrompt == "blurry, low quality, distorted, simple, cartoon") {
        genSettings.negativePrompt = getTextureNegativePrompt();
    }
    genSettings.controlNetPath  = controlNetPath;
    genSettings.controlStrength = controlStrength;
    // A non-negative seed locks generation determinism for this call (used by
    // the multi-view bake to share one seed across views); -1 keeps the stored
    // seed (which is itself -1 = random by default).
    if (seed >= 0) genSettings.seed = seed;
    // Honor the caller's requested generation size (the depth map is
    // captured at this resolution too); fall back to the stored SD
    // settings when a non-positive value is passed.
    if (width  > 0) genSettings.width  = width;
    if (height > 0) genSettings.height = height;

    QMetaObject::invokeMethod(m_worker,
        [this, enhancedPrompt, controlImage, outputPath, genSettings]() {
            m_worker->setSettings(genSettings);
            m_worker->generateTextureControlled(enhancedPrompt, controlImage, outputPath);
        }, Qt::QueuedConnection);
    // LCOV_EXCL_STOP
}

// LCOV_EXCL_START — requires sd.cpp worker
void SDManager::stopGeneration()
{
    if (m_worker) {
        m_worker->requestStop();
    }
}
// LCOV_EXCL_STOP

void SDManager::saveSettings()
{
    QSettings settings;
    settings.beginGroup("StableDiffusion");
    settings.setValue("modelsDirectory", m_modelsDirectory);
    settings.setValue("width", m_settings.width);
    settings.setValue("height", m_settings.height);
    settings.setValue("steps", m_settings.steps);
    settings.setValue("cfgScale", static_cast<double>(m_settings.cfgScale));
    settings.setValue("seed", QVariant::fromValue(m_settings.seed));
    settings.setValue("negativePrompt", m_settings.negativePrompt);
    settings.setValue("sampleMethod", m_settings.sampleMethod);
    settings.setValue("threads", m_settings.threads);
    settings.setValue("gpuLayers", m_settings.gpuLayers);
    settings.setValue("autoLoadModel", m_autoLoadModel);
    if (!m_currentModelName.isEmpty()) {
        settings.setValue("lastModel", m_currentModelName);
    }
    settings.endGroup();
}

void SDManager::loadSettings()
{
    QSettings settings;
    settings.beginGroup("StableDiffusion");
    m_modelsDirectory = settings.value("modelsDirectory", getDefaultModelsDirectory()).toString();
    m_settings.width = settings.value("width", 512).toInt();
    m_settings.height = settings.value("height", 512).toInt();
    m_settings.steps = settings.value("steps", 20).toInt();
    m_settings.cfgScale = settings.value("cfgScale", 7.0).toFloat();
    m_settings.seed = settings.value("seed", -1).toLongLong();
    m_settings.negativePrompt = settings.value("negativePrompt", "").toString();
    m_settings.sampleMethod = settings.value("sampleMethod", 0).toInt();
    m_settings.threads = settings.value("threads", 0).toInt();
    m_settings.gpuLayers = settings.value("gpuLayers", 99).toInt();
    m_autoLoadModel = settings.value("autoLoadModel", true).toBool();
    m_lastModelName = settings.value("lastModel", "").toString();
    settings.endGroup();
}

QString SDManager::getModelFilePath(const QString &modelName) const
{
    QDir modelsDir(m_modelsDirectory);

    // Check if it's a full filename with extension
    QString directPath = modelsDir.filePath(modelName);
    if (QFileInfo::exists(directPath)) {
        return directPath;
    }

    // Try common extensions
    QStringList extensions = {".safetensors", ".ckpt", ".gguf"};
    for (const QString &ext : extensions) {
        QString path = modelsDir.filePath(modelName + ext);
        if (QFileInfo::exists(path)) {
            return path;
        }
    }

    // Search by completeBaseName match (handles filenames with dots like "model.1.0.safetensors")
    QFileInfoList allFiles = modelsDir.entryInfoList(QDir::Files);
    for (const QFileInfo &f : allFiles) {
        if (f.completeBaseName() == modelName) {
            return f.absoluteFilePath();
        }
    }

    // Check in recommended models
    for (const SDModelInfo &info : m_recommendedModels) {
        if (info.name == modelName) {
            QString path = modelsDir.filePath(info.fileName);
            if (QFileInfo::exists(path)) {
                return path;
            }
        }
    }

    return QString();
}

bool SDManager::modelFileExists(const QString &modelName) const
{
    return !getModelFilePath(modelName).isEmpty();
}

QVariantList SDManager::getAvailableModelsInfo() const
{
    QVariantList result;
    QDir modelsDir(m_modelsDirectory);

    for (const QString &modelName : m_availableModels) {
        // Model names use completeBaseName(), so find matching file
        QStringList extensions = {".safetensors", ".ckpt", ".gguf"};
        QFileInfo fileInfo;
        for (const QString &ext : extensions) {
            fileInfo = QFileInfo(modelsDir.filePath(modelName + ext));
            if (fileInfo.exists()) break;
        }
        if (!fileInfo.exists()) {
            // Try matching by iterating directory files
            QFileInfoList allFiles = modelsDir.entryInfoList(QDir::Files);
            for (const QFileInfo &f : allFiles) {
                if (f.completeBaseName() == modelName) {
                    fileInfo = f;
                    break;
                }
            }
        }

        QVariantMap info;
        info["name"] = modelName;
        info["fileName"] = fileInfo.fileName();
        info["size"] = fileInfo.size();
        info["isDownloaded"] = true;
        result.append(info);
    }

    return result;
}

QVariantList SDManager::getRecommendedModelsInfo() const
{
    QVariantList result;
    for (const SDModelInfo &info : m_recommendedModels) {
        result.append(info.toVariantMap());
    }
    return result;
}

// LCOV_EXCL_START — worker signal handlers require a loaded/running SD model
void SDManager::onWorkerModelLoaded(const QString &modelPath)
{
    Q_UNUSED(modelPath);
    qDebug() << "SDManager: Model loaded:" << m_currentModelName;
    m_isLoading = false;
    emit isLoadingChanged();
    emit modelLoadCompleted(m_currentModelName);
    emit modelLoadedChanged();
    emit currentModelNameChanged();
    saveSettings();
}

void SDManager::onWorkerModelLoadError(const QString &error)
{
    qWarning() << "SDManager: Model load error:" << error;
    m_isLoading = false;
    emit isLoadingChanged();
    m_currentModelName.clear();
    emit modelLoadError(error);
    emit modelLoadedChanged();
    emit currentModelNameChanged();
}

void SDManager::onWorkerModelUnloaded()
{
    qDebug() << "SDManager: Model unloaded";
    if (!m_isLoading) {
        m_currentModelName.clear();
        emit currentModelNameChanged();
    }
    emit modelUnloaded();
    emit modelLoadedChanged();
}

void SDManager::onWorkerGenerationStarted()
{
    m_generationStep = 0;
    m_generationTotalSteps = m_settings.steps;
    emit generationStarted();
    emit isGeneratingChanged();
    emit generationProgressChanged();
}

void SDManager::onWorkerGenerationProgress(int step, int totalSteps)
{
    m_generationStep = step;
    m_generationTotalSteps = totalSteps;
    emit generationProgressChanged();
}

void SDManager::onWorkerGenerationCompleted(const QString &outputPath)
{
    m_generationStep = m_generationTotalSteps;
    emit generationProgressChanged();
    emit generationCompleted(outputPath);
    emit isGeneratingChanged();
}

void SDManager::onWorkerGenerationError(const QString &error)
{
    m_generationStep = 0;
    emit generationProgressChanged();
    emit generationError(error);
    emit isGeneratingChanged();
}

void SDManager::onWorkerGenerationStopped()
{
    m_generationStep = 0;
    emit generationProgressChanged();
    emit generationStopped();
    emit isGeneratingChanged();
}
// LCOV_EXCL_STOP
