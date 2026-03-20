// LCOV_EXCL_START — SD feature is not enabled in CI; requires GPU + model files
#include "SDWorker.h"
#include <QDebug>
#include <QThread>
#include <QImage>

SDWorker::SDWorker(QObject *parent)
    : QObject(parent)
{
}

SDWorker::~SDWorker()
{
    unloadModel();
}

bool SDWorker::loadModel(const QString &modelPath)
{
#ifdef ENABLE_STABLE_DIFFUSION
    // LCOV_EXCL_START — requires a real SD model file to exercise success paths
    // Stop any ongoing generation first
    if (m_isGenerating.load()) {
        m_stopRequested.store(true);
        for (int i = 0; i < 100 && m_isGenerating.load(); ++i) {
            QThread::msleep(10);
        }
    }

    bool success = false;
    bool wasModelLoaded = false;
    QString error;

    {
        QMutexLocker locker(&m_mutex);

        // Unload previous model
        if (m_ctx) {
            wasModelLoaded = true;
            free_sd_ctx(m_ctx);
            m_ctx = nullptr;
            m_modelPath.clear();
            m_isModelLoaded.store(false);
            qDebug() << "SDWorker: Previous model unloaded";
        }

        qDebug() << "SDWorker: Loading model from" << modelPath;

        try {
            sd_ctx_params_t params;
            sd_ctx_params_init(&params);
            QByteArray modelPathUtf8 = modelPath.toUtf8();
            params.model_path = modelPathUtf8.constData();
            params.n_threads = m_settings.threads > 0 ? m_settings.threads : QThread::idealThreadCount();
            if (params.n_threads > 16) {
                params.n_threads = 16;
            }
            params.vae_decode_only = true;

            m_ctx = new_sd_ctx(&params);
        } catch (const std::exception &e) {
            error = QString("Exception loading SD model: %1").arg(e.what());
            qWarning() << "SDWorker:" << error;
            m_ctx = nullptr;
        } catch (...) {
            error = QString("Unknown exception loading SD model from: %1").arg(modelPath);
            qWarning() << "SDWorker:" << error;
            m_ctx = nullptr;
        }

        if (!m_ctx) {
            if (error.isEmpty()) {
                error = QString("Failed to load SD model from: %1").arg(modelPath);
            }
            qWarning() << "SDWorker:" << error;
        } else {
            m_modelPath = modelPath;
            m_isModelLoaded.store(true);
            success = true;
            qDebug() << "SDWorker: Model loaded successfully";
        }
    }
    // Mutex released - safe to emit signals

    if (wasModelLoaded) {
        emit modelUnloaded();
    }

    if (success) {
        emit modelLoaded(modelPath);
    } else if (!error.isEmpty()) {
        emit modelLoadError(error);
    }

    return success;
    // LCOV_EXCL_STOP
#else
    Q_UNUSED(modelPath);
    emit modelLoadError("Stable Diffusion support is not enabled. Rebuild with ENABLE_STABLE_DIFFUSION=ON");
    return false;
#endif
}

// LCOV_EXCL_START — requires a previously loaded SD model
void SDWorker::unloadModel()
{
#ifdef ENABLE_STABLE_DIFFUSION
    if (m_isGenerating.load()) {
        m_stopRequested.store(true);
        for (int i = 0; i < 100 && m_isGenerating.load(); ++i) {
            QThread::msleep(10);
        }
    }

    bool wasLoaded = false;

    {
        QMutexLocker locker(&m_mutex);
        wasLoaded = (m_ctx != nullptr);
        unloadModelInternal();
    }

    if (wasLoaded) {
        emit modelUnloaded();
    }
#endif
}

void SDWorker::unloadModelInternal()
{
#ifdef ENABLE_STABLE_DIFFUSION
    if (m_ctx) {
        free_sd_ctx(m_ctx);
        m_ctx = nullptr;
        qDebug() << "SDWorker: Model unloaded";
    }

    m_modelPath.clear();
    m_isModelLoaded.store(false);
#endif
}

bool SDWorker::isModelLoaded() const
{
    return m_isModelLoaded.load();
}

void SDWorker::setSettings(const SDSettings &settings)
{
    QMutexLocker locker(&m_mutex);
    m_settings = settings;
}

void SDWorker::requestStop()
{
    m_stopRequested.store(true);
}

// LCOV_EXCL_STOP

#ifdef ENABLE_STABLE_DIFFUSION
void SDWorker::recreateContext()
{
    if (m_ctx) {
        free_sd_ctx(m_ctx);
        m_ctx = nullptr;
    }
    if (m_modelPath.isEmpty()) return;

    sd_ctx_params_t params;
    sd_ctx_params_init(&params);
    QByteArray pathUtf8 = m_modelPath.toUtf8();
    params.model_path = pathUtf8.constData();
    params.n_threads = m_settings.threads > 0 ? m_settings.threads : QThread::idealThreadCount();
    if (params.n_threads > 16) params.n_threads = 16;
    params.vae_decode_only = true;

    m_ctx = new_sd_ctx(&params);
    if (!m_ctx) {
        m_isModelLoaded.store(false);
        emit modelLoadError("Failed to recreate SD context");
    }
}
#endif

void SDWorker::generateTexture(const QString &prompt, const QString &outputPath)
{
#ifdef ENABLE_STABLE_DIFFUSION
    if (!isModelLoaded()) {
        emit generationError("No SD model loaded");
        return;
    }

    // Prevent concurrent generation
    if (m_isGenerating.load()) {
        emit generationError("Generation already in progress");
        return;
    }

    // LCOV_EXCL_START — requires a loaded SD model for generation
    m_stopRequested.store(false);
    m_isGenerating.store(true);

    QMutexLocker locker(&m_mutex);

    // sd.cpp crashes on second generate_image() call with the same context.
    // Recreate the context before each generation to ensure clean state.
    recreateContext();

    if (!m_ctx) {
        m_isGenerating.store(false);
        emit generationError("SD context creation failed");
        return;
    }

    emit generationStarted();

    qDebug() << "SDWorker: Generating texture with prompt:" << prompt;
    qDebug() << "SDWorker: Size:" << m_settings.width << "x" << m_settings.height
             << "Steps:" << m_settings.steps << "CFG:" << m_settings.cfgScale;

    // Set up progress callback
    sd_set_progress_callback(progressCallback, this);

    sd_image_t *result = nullptr;
    try {
        sd_img_gen_params_t img_params;
        sd_img_gen_params_init(&img_params);

        // Keep QByteArrays alive for the duration of generate_image
        QByteArray promptUtf8 = prompt.toUtf8();
        QByteArray negPromptUtf8 = m_settings.negativePrompt.toUtf8();

        img_params.prompt = promptUtf8.constData();
        img_params.negative_prompt = negPromptUtf8.isEmpty() ? "" : negPromptUtf8.constData();
        img_params.width = m_settings.width;
        img_params.height = m_settings.height;
        img_params.sample_params.sample_steps = m_settings.steps;
        img_params.sample_params.guidance.txt_cfg = m_settings.cfgScale;
        img_params.seed = m_settings.seed;
        img_params.sample_params.sample_method = static_cast<enum sample_method_t>(m_settings.sampleMethod);

        result = generate_image(m_ctx, &img_params);
    } catch (const std::exception &e) {
        // Clear callback before returning
        sd_set_progress_callback(nullptr, nullptr);
        m_isGenerating.store(false);
        emit generationError(QString("Exception during generation: %1").arg(e.what()));
        return;
    } catch (...) {
        sd_set_progress_callback(nullptr, nullptr);
        m_isGenerating.store(false);
        emit generationError("Unknown exception during texture generation");
        return;
    }

    // Clear callback now that generation is done
    sd_set_progress_callback(nullptr, nullptr);

    if (m_stopRequested.load()) {
        if (result) free(result);
        m_isGenerating.store(false);
        emit generationStopped();
        return;
    }

    if (!result || !result->data) {
        if (result) free(result);
        m_isGenerating.store(false);
        emit generationError("Failed to generate image - no output produced");
        return;
    }

    // Deep copy the image data — sd.cpp may reuse the internal buffer on next call
    QImage::Format format = (result->channel == 4) ? QImage::Format_RGBA8888 : QImage::Format_RGB888;
    int bytesPerLine = static_cast<int>(result->width * result->channel);
    size_t dataSize = static_cast<size_t>(bytesPerLine) * result->height;
    QByteArray pixelCopy(reinterpret_cast<const char*>(result->data), dataSize);
    QImage imgCopy(reinterpret_cast<const uchar*>(pixelCopy.constData()),
                   result->width, result->height, bytesPerLine, format);
    imgCopy = imgCopy.copy(); // Detach from QByteArray backing

    // Only free the struct, not the data — sd.cpp manages the pixel buffer internally
    free(result);

    bool saved = imgCopy.save(outputPath, "PNG");

    m_isGenerating.store(false);

    if (saved) {
        qDebug() << "SDWorker: Texture saved to" << outputPath;
        emit generationCompleted(outputPath);
    } else {
        emit generationError(QString("Failed to save texture to: %1").arg(outputPath));
    }
    // LCOV_EXCL_STOP
#else
    Q_UNUSED(prompt);
    Q_UNUSED(outputPath);
    emit generationError("Stable Diffusion support is not enabled");
#endif
}


#ifdef ENABLE_STABLE_DIFFUSION
void SDWorker::progressCallback(int step, int steps, float time, void *data)
{
    Q_UNUSED(time);
    SDWorker *worker = static_cast<SDWorker*>(data);
    if (worker) {
        // Emit directly — Qt's signal-slot connection between SDWorker (worker thread)
        // and SDManager (main thread) uses QueuedConnection automatically, so the slot
        // is safely delivered to the main thread. Using QMetaObject::invokeMethod with
        // QueuedConnection would post to the worker's event loop which is blocked
        // inside generate_image(), causing the progress to never update.
        emit worker->generationProgress(step, steps);
    }
}
#endif
// LCOV_EXCL_STOP
