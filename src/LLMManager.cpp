#include "LLMManager.h"
#include <QCoreApplication>
#include <QStandardPaths>
#include <QDebug>
#include <QFileInfo>
#include <QTimer>
#include <QRegularExpression>

LLMManager* LLMManager::s_instance = nullptr;

LLMManager* LLMManager::instance()
{
    if (!s_instance) {
        s_instance = new LLMManager();
    }
    return s_instance;
}

LLMManager* LLMManager::qmlInstance(QQmlEngine *engine, QJSEngine *scriptEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(scriptEngine)
    return instance();
}

LLMManager::LLMManager(QObject *parent)
    : QObject(parent)
{
    m_modelsDirectory = getDefaultModelsDirectory();
    populateRecommendedModels();
    loadSettings();
    initializeWorkerThread();
    scanForModels();

    // Auto-load model after event loop starts (delayed to allow UI initialization)
    QTimer::singleShot(500, this, &LLMManager::tryAutoLoadModel);
}

LLMManager::~LLMManager()
{
    shutdownWorkerThread();
}

void LLMManager::initializeWorkerThread()
{
    m_workerThread = new QThread(this);
    m_worker = new LLMWorker();
    m_worker->moveToThread(m_workerThread);

    // Connect worker signals
    connect(m_worker, &LLMWorker::modelLoaded, this, &LLMManager::onWorkerModelLoaded);
    connect(m_worker, &LLMWorker::modelLoadError, this, &LLMManager::onWorkerModelLoadError);
    connect(m_worker, &LLMWorker::modelUnloaded, this, &LLMManager::onWorkerModelUnloaded);
    connect(m_worker, &LLMWorker::generationStarted, this, &LLMManager::onWorkerGenerationStarted);
    connect(m_worker, &LLMWorker::generationProgress, this, &LLMManager::onWorkerGenerationProgress);
    connect(m_worker, &LLMWorker::generationCompleted, this, &LLMManager::onWorkerGenerationCompleted);
    connect(m_worker, &LLMWorker::generationError, this, &LLMManager::onWorkerGenerationError);
    connect(m_worker, &LLMWorker::generationStopped, this, &LLMManager::onWorkerGenerationStopped);

    // Initialize backend on the worker thread after it starts
    connect(m_workerThread, &QThread::started, m_worker, &LLMWorker::initBackend);

    // Cleanup on thread finished
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);

    m_workerThread->start();
    qDebug() << "LLMManager: Worker thread started";
}

void LLMManager::shutdownWorkerThread()
{
    if (m_worker) {
        m_worker->requestStop();
        // Unload model on worker thread before stopping to free Metal resources
        if (m_workerThread && m_workerThread->isRunning())
            QMetaObject::invokeMethod(m_worker, &LLMWorker::unloadModel, Qt::BlockingQueuedConnection);
    }

    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait();
        qDebug() << "LLMManager: Worker thread stopped";
    }
}

QString LLMManager::getDefaultModelsDirectory() const
{
    QString dataPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath("models");
}

void LLMManager::populateRecommendedModels()
{
    m_recommendedModels.clear();

    // Recommended GGUF models from Hugging Face - ordered by size (smallest first)

    // Gemma 3 models (Google's latest)
    m_recommendedModels.append({
        "Gemma 3 1B Q4_K_M",
        "gemma-3-1b-it-Q4_K_M.gguf",
        "https://huggingface.co/bartowski/google_gemma-3-1b-it-GGUF/resolve/main/google_gemma-3-1b-it-Q4_K_M.gguf",
        "Google's Gemma 3 1B. Ultra-fast, great for quick tasks.",
        900000000, // ~0.9GB
        false
    });

    m_recommendedModels.append({
        "Gemma 2 2B Q4_K_M",
        "gemma-2-2b-it-Q4_K_M.gguf",
        "https://huggingface.co/bartowski/gemma-2-2b-it-GGUF/resolve/main/gemma-2-2b-it-Q4_K_M.gguf",
        "Google's Gemma 2 2B. Fast and efficient.",
        1800000000, // ~1.8GB
        false
    });

    m_recommendedModels.append({
        "Llama 3.2 3B Q4_K_M",
        "Llama-3.2-3B-Instruct-Q4_K_M.gguf",
        "https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/Llama-3.2-3B-Instruct-Q4_K_M.gguf",
        "Meta's Llama 3.2 3B. Well-rounded performance.",
        2000000000, // ~2.0GB
        false
    });

    m_recommendedModels.append({
        "Qwen 2.5 3B Q4_K_M",
        "qwen2.5-3b-instruct-q4_k_m.gguf",
        "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf",
        "Alibaba's Qwen 2.5 3B. Great for code generation.",
        2100000000, // ~2.1GB
        false
    });

    m_recommendedModels.append({
        "Qwen 2.5 Coder 3B Q4_K_M",
        "qwen2.5-coder-3b-instruct-q4_k_m.gguf",
        "https://huggingface.co/Qwen/Qwen2.5-Coder-3B-Instruct-GGUF/resolve/main/qwen2.5-coder-3b-instruct-q4_k_m.gguf",
        "Qwen 2.5 Coder 3B. Specialized for code tasks.",
        2100000000, // ~2.1GB
        false
    });

    m_recommendedModels.append({
        "Phi-3.5 Mini Q4_K_M",
        "Phi-3.5-mini-instruct-Q4_K_M.gguf",
        "https://huggingface.co/bartowski/Phi-3.5-mini-instruct-GGUF/resolve/main/Phi-3.5-mini-instruct-Q4_K_M.gguf",
        "Microsoft's Phi-3.5 Mini. Compact and capable.",
        2400000000, // ~2.4GB
        false
    });

    m_recommendedModels.append({
        "Gemma 3 4B Q4_K_M",
        "gemma-3-4b-it-Q4_K_M.gguf",
        "https://huggingface.co/bartowski/google_gemma-3-4b-it-GGUF/resolve/main/google_gemma-3-4b-it-Q4_K_M.gguf",
        "Google's Gemma 3 4B. Excellent balance of speed and quality.",
        3100000000, // ~3.1GB
        false
    });

    m_recommendedModels.append({
        "Qwen 2.5 7B Q4_K_M",
        "qwen2.5-7b-instruct-q4_k_m.gguf",
        "https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF/resolve/main/qwen2.5-7b-instruct-q4_k_m.gguf",
        "Qwen 2.5 7B. Higher quality, requires more VRAM.",
        4700000000, // ~4.7GB
        false
    });

    m_recommendedModels.append({
        "Qwen 2.5 Coder 7B Q4_K_M",
        "qwen2.5-coder-7b-instruct-q4_k_m.gguf",
        "https://huggingface.co/Qwen/Qwen2.5-Coder-7B-Instruct-GGUF/resolve/main/qwen2.5-coder-7b-instruct-q4_k_m.gguf",
        "Qwen 2.5 Coder 7B. Best for code generation.",
        4700000000, // ~4.7GB
        false
    });

    m_recommendedModels.append({
        "Gemma 3 12B Q4_K_M",
        "gemma-3-12b-it-Q4_K_M.gguf",
        "https://huggingface.co/bartowski/google_gemma-3-12b-it-GGUF/resolve/main/google_gemma-3-12b-it-Q4_K_M.gguf",
        "Google's Gemma 3 12B. High quality, needs 8GB+ VRAM.",
        8100000000, // ~8.1GB
        false
    });
}

bool LLMManager::isModelLoaded() const
{
    return m_worker && m_worker->isModelLoaded();
}

QStringList LLMManager::availableModels() const
{
    return m_availableModels;
}

QList<ModelInfo> LLMManager::getRecommendedModels() const
{
    return m_recommendedModels;
}

void LLMManager::setModelsDirectory(const QString &dir)
{
    if (m_modelsDirectory != dir) {
        m_modelsDirectory = dir;
        QDir().mkpath(dir);
        scanForModels();
        saveSettings();
        emit modelsDirectoryChanged();
    }
}

LLMSettings LLMManager::getSettings() const
{
    return m_settings;
}

void LLMManager::setSettings(const LLMSettings &settings)
{
    m_settings = settings;
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [this, settings]() {
            m_worker->setSettings(settings);
        }, Qt::QueuedConnection);
    }
    saveSettings();
}

bool LLMManager::isGenerating() const
{
    return m_worker && m_worker->isGenerating();
}

QString LLMManager::getOgre3DSystemPrompt()
{
    return R"(You generate Ogre3D material scripts. Output ONLY the material script, nothing else.

RULES:
1. Output ONLY the material script - no markdown, no explanations
2. Keep the same material name if modifying an existing material
3. All color values must be NUMBERS between 0.0 and 1.0
4. Do NOT add texture_unit unless specifically asked for textures
5. For simple color changes, only modify ambient/diffuse/specular/emissive values

BASIC STRUCTURE:
material NAME
{
    technique
    {
        pass
        {
            ambient R G B
            diffuse R G B
            specular R G B SHININESS
            emissive R G B
        }
    }
}

COMMON COLORS (R G B values):
- red: 1.0 0.0 0.0
- green: 0.0 1.0 0.0
- blue: 0.0 0.0 1.0
- yellow: 1.0 1.0 0.0
- white: 1.0 1.0 1.0
- black: 0.0 0.0 0.0
- orange: 1.0 0.5 0.0
- purple: 0.5 0.0 1.0
- cyan: 0.0 1.0 1.0

EXAMPLE - Green shiny material:
material MyMaterial
{
    technique
    {
        pass
        {
            ambient 0.0 0.2 0.0
            diffuse 0.0 0.8 0.0
            specular 0.5 1.0 0.5 64
            emissive 0.0 0.0 0.0
        }
    }
}

ADVANCED (only use when requested):
- scene_blend add|alpha_blend : For transparency/glow
- depth_write off : For transparent materials
- lighting off : Disable dynamic lighting
- texture_unit { texture <file> } : Only if textures requested)";
}

void LLMManager::loadModel(const QString &modelName)
{
    QString modelPath = getModelFilePath(modelName);
    if (modelPath.isEmpty() || !QFileInfo::exists(modelPath)) {
        emit modelLoadError(QString("Model file not found: %1").arg(modelName));
        return;
    }

    // LCOV_EXCL_START — requires a real GGUF model file
    m_isLoading = true;
    emit isLoadingChanged();
    emit modelLoadStarted(modelName);
    m_currentModelName = modelName;

    QMetaObject::invokeMethod(m_worker, [this, modelPath]() {
        m_worker->setSettings(m_settings);
        m_worker->loadModel(modelPath);
    }, Qt::QueuedConnection);
    // LCOV_EXCL_STOP
}

// Settings setters for QML
void LLMManager::setContextSize(int value)
{
    if (m_settings.contextSize != value) {
        m_settings.contextSize = value;
        saveSettings();
        emit settingsChanged();
    }
}

void LLMManager::setMaxTokens(int value)
{
    if (m_settings.maxTokens != value) {
        m_settings.maxTokens = value;
        saveSettings();
        emit settingsChanged();
    }
}

void LLMManager::setTemperature(float value)
{
    if (m_settings.temperature != value) {
        m_settings.temperature = value;
        saveSettings();
        emit settingsChanged();
    }
}

void LLMManager::setGpuLayers(int value)
{
    if (m_settings.gpuLayers != value) {
        m_settings.gpuLayers = value;
        saveSettings();
        emit settingsChanged();
    }
}

void LLMManager::setAutoLoadModel(bool value)
{
    if (m_autoLoadModel != value) {
        m_autoLoadModel = value;
        saveSettings();
        emit autoLoadModelChanged();
    }
}

void LLMManager::tryAutoLoadModel()
{
    if (!m_autoLoadModel || m_lastModelName.isEmpty()) {
        return;
    }

    // Check if the model file exists
    if (!modelFileExists(m_lastModelName)) {
        qDebug() << "LLMManager: Auto-load model not found:" << m_lastModelName;
        return;
    }

    // LCOV_EXCL_START — requires a downloaded model file
    qDebug() << "LLMManager: Auto-loading model:" << m_lastModelName;
    loadModel(m_lastModelName);
    // LCOV_EXCL_STOP
}

void LLMManager::unloadModel()
{
    if (m_worker) {
        QMetaObject::invokeMethod(m_worker, [this]() {
            m_worker->unloadModel();
        }, Qt::QueuedConnection);
    }
}

void LLMManager::scanForModels()
{
    m_availableModels.clear();

    QDir modelsDir(m_modelsDirectory);
    if (!modelsDir.exists()) {
        modelsDir.mkpath(".");
    }

    QStringList filters;
    filters << "*.gguf" << "*.bin";
    QFileInfoList files = modelsDir.entryInfoList(filters, QDir::Files);

    for (const QFileInfo &file : files) {
        m_availableModels.append(file.baseName());
    }

    // Update recommended models download status
    for (int i = 0; i < m_recommendedModels.size(); ++i) {
        QString filePath = modelsDir.filePath(m_recommendedModels[i].fileName);
        m_recommendedModels[i].isDownloaded = QFileInfo::exists(filePath);
    }

    qDebug() << "LLMManager: Found" << m_availableModels.size() << "models in" << m_modelsDirectory;
    emit availableModelsChanged();
}

void LLMManager::generateMaterial(const QString &prompt, const QString &currentMaterial, const QStringList &availableTextures)
{
    if (!isModelLoaded()) {
        emit generationError("No model loaded. Please load a model first.");
        return;
    }

    // LCOV_EXCL_START — requires a loaded LLM model
    // Store for potential retry
    m_pendingPrompt = prompt;
    m_pendingCurrentMaterial = currentMaterial;
    m_pendingAvailableTextures = availableTextures;
    m_retryCount = 0;

    QString userPrompt = buildUserPrompt(prompt, currentMaterial, availableTextures);

    QMetaObject::invokeMethod(m_worker, [this, userPrompt]() {
        m_worker->generate(getOgre3DSystemPrompt(), userPrompt);
    }, Qt::QueuedConnection);
    // LCOV_EXCL_STOP
}

QString LLMManager::buildUserPrompt(const QString &prompt, const QString &currentMaterial, const QStringList &availableTextures) const
{
    QString userPrompt;
    QString lowerPrompt = prompt.toLower();

    // Only include textures if the user's prompt mentions texture-related keywords
    bool needsTextures = lowerPrompt.contains("texture") ||
                         lowerPrompt.contains("image") ||
                         lowerPrompt.contains("picture") ||
                         lowerPrompt.contains("map") ||
                         lowerPrompt.contains("bitmap") ||
                         lowerPrompt.contains(".png") ||
                         lowerPrompt.contains(".jpg") ||
                         lowerPrompt.contains(".dds");

    QString texturesSection;
    if (needsTextures && !availableTextures.isEmpty()) {
        // Filter to only include image-like textures (exclude internal Ogre textures)
        QStringList filteredTextures;
        for (const QString &tex : availableTextures) {
            // Skip internal Ogre textures and non-image resources
            if (!tex.startsWith("Ogre/") &&
                !tex.startsWith("RTT") &&
                !tex.startsWith("__") &&
                !tex.contains("RenderTarget") &&
                (tex.endsWith(".png", Qt::CaseInsensitive) ||
                 tex.endsWith(".jpg", Qt::CaseInsensitive) ||
                 tex.endsWith(".jpeg", Qt::CaseInsensitive) ||
                 tex.endsWith(".dds", Qt::CaseInsensitive) ||
                 tex.endsWith(".tga", Qt::CaseInsensitive) ||
                 tex.endsWith(".bmp", Qt::CaseInsensitive))) {
                filteredTextures.append(tex);
            }
        }

        if (!filteredTextures.isEmpty()) {
            // Limit to reasonable number to avoid prompt bloat
            int maxTextures = qMin(filteredTextures.size(), 30);
            QStringList limitedTextures = filteredTextures.mid(0, maxTextures);

            texturesSection = QString("\nAVAILABLE TEXTURES (use ONLY from this list):\n%1\n")
                                  .arg(limitedTextures.join(", "));
        }
    }

    if (!currentMaterial.isEmpty()) {
        userPrompt = QString("Current material (keep the same name):\n%1\n%2\nRequest: %3")
                         .arg(currentMaterial)
                         .arg(texturesSection)
                         .arg(prompt);
    } else {
        userPrompt = QString("%1Create: %2")
                         .arg(texturesSection)
                         .arg(prompt);
    }

    return userPrompt;
}

void LLMManager::stopGeneration()
{
    if (m_worker) {
        m_worker->requestStop();
    }
}

void LLMManager::saveSettings()
{
    QSettings settings;
    settings.beginGroup("LLM");
    settings.setValue("modelsDirectory", m_modelsDirectory);
    settings.setValue("contextSize", m_settings.contextSize);
    settings.setValue("maxTokens", m_settings.maxTokens);
    settings.setValue("temperature", static_cast<double>(m_settings.temperature));
    settings.setValue("gpuLayers", m_settings.gpuLayers);
    settings.setValue("threads", m_settings.threads);
    settings.setValue("topP", static_cast<double>(m_settings.topP));
    settings.setValue("topK", m_settings.topK);
    settings.setValue("repeatPenalty", static_cast<double>(m_settings.repeatPenalty));
    settings.setValue("autoLoadModel", m_autoLoadModel);
    // Save last model name when a model is loaded
    if (!m_currentModelName.isEmpty()) {
        settings.setValue("lastModel", m_currentModelName);
    }
    settings.endGroup();
}

void LLMManager::loadSettings()
{
    QSettings settings;
    settings.beginGroup("LLM");
    m_modelsDirectory = settings.value("modelsDirectory", getDefaultModelsDirectory()).toString();
    m_settings.contextSize = settings.value("contextSize", 4096).toInt();
    m_settings.maxTokens = settings.value("maxTokens", 2048).toInt();
    m_settings.temperature = settings.value("temperature", 0.7).toFloat();
    m_settings.gpuLayers = settings.value("gpuLayers", 99).toInt();
    m_settings.threads = settings.value("threads", 0).toInt();
    m_settings.topP = settings.value("topP", 0.9).toFloat();
    m_settings.topK = settings.value("topK", 40).toInt();
    m_settings.repeatPenalty = settings.value("repeatPenalty", 1.1).toFloat();
    m_autoLoadModel = settings.value("autoLoadModel", false).toBool();
    m_lastModelName = settings.value("lastModel", "").toString();
    settings.endGroup();
}

QString LLMManager::getModelFilePath(const QString &modelName) const
{
    QDir modelsDir(m_modelsDirectory);

    // First check if it's a full filename with extension
    QString directPath = modelsDir.filePath(modelName);
    if (QFileInfo::exists(directPath)) {
        return directPath;
    }

    // Try with .gguf extension
    QString ggufPath = modelsDir.filePath(modelName + ".gguf");
    if (QFileInfo::exists(ggufPath)) {
        return ggufPath;
    }

    // Try with .bin extension
    QString binPath = modelsDir.filePath(modelName + ".bin");
    if (QFileInfo::exists(binPath)) {
        return binPath;
    }

    // Check in recommended models
    for (const ModelInfo &info : m_recommendedModels) {
        if (info.name == modelName) {
            QString path = modelsDir.filePath(info.fileName);
            if (QFileInfo::exists(path)) {
                return path;
            }
        }
    }

    return QString();
}

bool LLMManager::modelFileExists(const QString &modelName) const
{
    return !getModelFilePath(modelName).isEmpty();
}

QVariantList LLMManager::getAvailableModelsInfo() const
{
    QVariantList result;
    QDir modelsDir(m_modelsDirectory);

    for (const QString &modelName : m_availableModels) {
        QFileInfo fileInfo(modelsDir.filePath(modelName + ".gguf"));
        if (!fileInfo.exists()) {
            fileInfo = QFileInfo(modelsDir.filePath(modelName + ".bin"));
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

QVariantList LLMManager::getRecommendedModelsInfo() const
{
    QVariantList result;
    for (const ModelInfo &info : m_recommendedModels) {
        result.append(info.toVariantMap());
    }
    return result;
}

// LCOV_EXCL_START — worker signal handlers require a loaded/running LLM model
void LLMManager::onWorkerModelLoaded(const QString &modelPath)
{
    qDebug() << "LLMManager: Model loaded:" << modelPath;
    m_isLoading = false;
    emit isLoadingChanged();
    emit modelLoadCompleted(m_currentModelName);
    emit modelLoadedChanged();
    emit currentModelNameChanged();
    saveSettings(); // Save last loaded model
}

void LLMManager::onWorkerModelLoadError(const QString &error)
{
    qWarning() << "LLMManager: Model load error:" << error;
    m_isLoading = false;
    emit isLoadingChanged();
    m_currentModelName.clear();
    emit modelLoadError(error);
    emit modelLoadedChanged();
    emit currentModelNameChanged();
}

void LLMManager::onWorkerModelUnloaded()
{
    qDebug() << "LLMManager: Model unloaded";
    // Don't clear the model name if we're loading a new model (switching models)
    if (!m_isLoading) {
        m_currentModelName.clear();
        emit currentModelNameChanged();
    }
    emit modelUnloaded();
    emit modelLoadedChanged();
}

void LLMManager::onWorkerGenerationStarted()
{
    emit generationStarted();
    emit isGeneratingChanged();
}

void LLMManager::onWorkerGenerationProgress(const QString &partialText, float progress)
{
    emit generationProgress(partialText, progress);
}

void LLMManager::onWorkerGenerationCompleted(const QString &fullText)
{
    // Clean up the generated script
    QString cleanedScript = cleanupGeneratedScript(fullText);

    // Validate the script
    QString errorMessage;
    if (validateMaterialScript(cleanedScript, errorMessage)) {
        // Valid script - emit and reset
        m_retryCount = 0;
        emit generationCompleted(cleanedScript);
        emit isGeneratingChanged();
    } else {
        // Invalid script - retry if we haven't exceeded max retries
        m_retryCount++;
        qDebug() << "LLMManager: Invalid script generated (" << errorMessage << "), retry" << m_retryCount << "of" << MAX_RETRIES;

        if (m_retryCount <= MAX_RETRIES) {
            // Build a correction prompt with the error information
            QString basePrompt = buildUserPrompt(m_pendingPrompt, m_pendingCurrentMaterial, m_pendingAvailableTextures);
            QString correctionPrompt = QString(
                "The previous output was invalid: %1\n\n"
                "IMPORTANT: Output ONLY a valid Ogre3D material script with:\n"
                "- Proper numeric values (0.0 to 1.0 for colors)\n"
                "- Balanced braces (every { must have a matching })\n"
                "- No markdown or explanations\n\n%2")
                .arg(errorMessage)
                .arg(basePrompt);

            QMetaObject::invokeMethod(m_worker, [this, correctionPrompt]() {
                m_worker->generate(getOgre3DSystemPrompt(), correctionPrompt);
            }, Qt::QueuedConnection);
        } else {
            // Max retries exceeded - emit what we have with a warning
            m_retryCount = 0;
            qWarning() << "LLMManager: Max retries exceeded, returning potentially invalid script";
            emit generationCompleted(cleanedScript);
            emit isGeneratingChanged();
        }
    }
}

void LLMManager::onWorkerGenerationError(const QString &error)
{
    emit generationError(error);
    emit isGeneratingChanged();
}

void LLMManager::onWorkerGenerationStopped()
{
    m_retryCount = 0;
    emit generationStopped();
    emit isGeneratingChanged();
}
// LCOV_EXCL_STOP

QString LLMManager::cleanupGeneratedScript(const QString &script) const
{
    QString cleaned = script.trimmed();

    // Remove markdown code blocks if present
    if (cleaned.startsWith("```")) {
        int firstNewline = cleaned.indexOf('\n');
        if (firstNewline != -1) {
            cleaned = cleaned.mid(firstNewline + 1);
        }
    }
    if (cleaned.endsWith("```")) {
        cleaned = cleaned.left(cleaned.length() - 3).trimmed();
    }

    // Find the material block - look for "material " at start of a line
    QRegularExpression materialStart("^material\\s+", QRegularExpression::MultilineOption);
    QRegularExpressionMatch match = materialStart.match(cleaned);
    if (match.hasMatch()) {
        cleaned = cleaned.mid(match.capturedStart());
    }

    // Remove any text after the last closing brace that ends the material
    int braceCount = 0;
    int materialEnd = -1;
    bool inMaterial = false;

    for (int i = 0; i < cleaned.length(); ++i) {
        QChar c = cleaned[i];
        if (c == '{') {
            braceCount++;
            inMaterial = true;
        } else if (c == '}') {
            braceCount--;
            if (inMaterial && braceCount == 0) {
                materialEnd = i;
                break;
            }
        }
    }

    if (materialEnd != -1) {
        cleaned = cleaned.left(materialEnd + 1);
    }

    return cleaned.trimmed();
}

bool LLMManager::validateMaterialScript(const QString &script, QString &errorMessage) const
{
    QString cleaned = script.trimmed();

    // Check basic structure
    if (!cleaned.startsWith("material ")) {
        errorMessage = "Script must start with 'material'";
        return false;
    }

    // Check for balanced braces
    int braceCount = 0;
    for (QChar c : cleaned) {
        if (c == '{') braceCount++;
        if (c == '}') braceCount--;
        if (braceCount < 0) {
            errorMessage = "Unbalanced braces (extra closing brace)";
            return false;
        }
    }
    if (braceCount != 0) {
        errorMessage = "Unbalanced braces (missing closing brace)";
        return false;
    }

    // Check for required sections
    if (!cleaned.contains("technique")) {
        errorMessage = "Missing 'technique' block";
        return false;
    }
    if (!cleaned.contains("pass")) {
        errorMessage = "Missing 'pass' block";
        return false;
    }

    // Check for common errors - invalid values in property lines
    QStringList lines = cleaned.split('\n');
    QRegularExpression propertyPattern("^\\s*(ambient|diffuse|specular|emissive)\\s+(.+)$");

    for (const QString &line : lines) {
        QRegularExpressionMatch match = propertyPattern.match(line);
        if (match.hasMatch()) {
            QString values = match.captured(2).trimmed();
            // Check if values contain non-numeric words (except for valid keywords)
            QStringList parts = values.split(QRegularExpression("\\s+"));
            for (const QString &part : parts) {
                // Skip if it's a number
                bool isNumber = false;
                part.toFloat(&isNumber);
                if (isNumber) continue;

                // Invalid word in property value
                errorMessage = QString("Invalid value '%1' in %2 property").arg(part).arg(match.captured(1));
                return false;
            }
        }
    }

    return true;
}
