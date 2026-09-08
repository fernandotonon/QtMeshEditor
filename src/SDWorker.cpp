// LCOV_EXCL_START — SD feature is not enabled in CI; requires GPU + model files
#include "SDWorker.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QImage>

SDWorker::SDWorker(QObject *parent)
    : QObject(parent)
{
}

// LCOV_EXCL_STOP — pure file-set detection, unit-tested in every build
SDWorker::Flux2Set SDWorker::detectFlux2Set(const QString &dir)
{
    Flux2Set set;
    QDir d(dir);
    if (dir.isEmpty() || !d.exists()) return set;
    const QFileInfoList files = d.entryInfoList(
        {QStringLiteral("*.gguf"), QStringLiteral("*.safetensors")},
        QDir::Files);
    for (const QFileInfo &fi : files) {
        const QString lower = fi.fileName().toLower();
        const bool isVae = lower.contains(QLatin1String("vae"))
                           || lower.startsWith(QLatin1String("ae."));
        if (isVae) {
            if (set.vae.isEmpty()) set.vae = fi.absoluteFilePath();
            continue;
        }
        if (lower.contains(QLatin1String("qwen"))
            || lower.contains(QLatin1String("mistral"))) {
            if (set.llm.isEmpty()) set.llm = fi.absoluteFilePath();
            continue;
        }
        if (lower.contains(QLatin1String("flux"))) {
            if (set.diffusion.isEmpty()) set.diffusion = fi.absoluteFilePath();
        }
    }
    return set;
}
// LCOV_EXCL_START

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

        // A DIRECTORY means a FLUX.2 component set (diffusion GGUF + VAE +
        // Qwen3 text encoder) — sd.cpp loads those through separate params,
        // not model_path.
        m_isFlux2 = false;
        m_flux2 = Flux2Set{};
        if (QFileInfo(modelPath).isDir()) {
            m_flux2 = detectFlux2Set(modelPath);
            if (!m_flux2.valid()) {
                error = QString("Directory does not contain a FLUX.2 set "
                                "(diffusion + vae + text encoder): %1")
                            .arg(modelPath);
                qWarning() << "SDWorker:" << error;
                // The previous context was already freed above — consumers
                // must see the unload transition before the error.
                if (wasModelLoaded)
                    emit modelUnloaded();
                emit modelLoadError(error);
                return false;
            }
            m_isFlux2 = true;
        }

        try {
            sd_ctx_params_t params;
            sd_ctx_params_init(&params);
            QByteArray modelPathUtf8 = modelPath.toUtf8();
            QByteArray diffUtf8, vaeUtf8, llmUtf8;
            if (m_isFlux2) {
                diffUtf8 = m_flux2.diffusion.toUtf8();
                vaeUtf8  = m_flux2.vae.toUtf8();
                llmUtf8  = m_flux2.llm.toUtf8();
                params.diffusion_model_path = diffUtf8.constData();
                params.vae_path             = vaeUtf8.constData();
                params.llm_path             = llmUtf8.constData();
            } else {
                params.model_path = modelPathUtf8.constData();
            }
            params.n_threads = m_settings.threads > 0 ? m_settings.threads : QThread::idealThreadCount();
            if (params.n_threads > 16) {
                params.n_threads = 16;
            }
            // FLUX.2 keeps the full VAE: reference-image EDITING encodes the
            // ref through it, and a decode-only graph hard-asserts inside
            // sd.cpp (auto_encoder_kl GGML_ASSERT). The flux2 VAE is ~336MB —
            // negligible next to the 4B diffusion model.
            params.vae_decode_only = !m_isFlux2;

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

void SDWorker::setRefImage(const QImage &image)
{
    QMutexLocker locker(&m_mutex);
    m_refImage = image;
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
    QByteArray diffUtf8, vaeUtf8, llmUtf8;
    if (m_isFlux2 && m_flux2.valid()) {
        diffUtf8 = m_flux2.diffusion.toUtf8();
        vaeUtf8  = m_flux2.vae.toUtf8();
        llmUtf8  = m_flux2.llm.toUtf8();
        params.diffusion_model_path = diffUtf8.constData();
        params.vae_path             = vaeUtf8.constData();
        params.llm_path             = llmUtf8.constData();
    } else {
        params.model_path = pathUtf8.constData();
    }
    params.n_threads = m_settings.threads > 0 ? m_settings.threads : QThread::idealThreadCount();
    if (params.n_threads > 16) params.n_threads = 16;

    // Issue #403: load the ControlNet model into the context when a
    // path is configured. `controlNetUtf8` must outlive new_sd_ctx.
    // ControlNet needs the full VAE (encode + decode), so
    // vae_decode_only must be false in that case; otherwise keep the
    // decode-only optimization that halves VAE memory for plain
    // txt2img. NEVER for FLUX.2 — the persisted ControlNet is an SD 1.5
    // depth model and poisons the flux context.
    QByteArray controlNetUtf8;
    if (!m_isFlux2 && !m_settings.controlNetPath.isEmpty()) {
        controlNetUtf8 = m_settings.controlNetPath.toUtf8();
        params.control_net_path = controlNetUtf8.constData();
        params.vae_decode_only = false;
    } else {
        params.vae_decode_only = true;
    }
    // FLUX.2: full VAE always (reference-image editing encodes through it).
    if (m_isFlux2)
        params.vae_decode_only = false;

    m_ctx = new_sd_ctx(&params);
    if (!m_ctx) {
        m_isModelLoaded.store(false);
        emit modelLoadError("Failed to recreate SD context");
    }
}
#endif

void SDWorker::generateTexture(const QString &prompt, const QString &outputPath)
{
    // Plain txt2img — no control image.
    generateTextureControlled(prompt, QImage(), outputPath);
}

void SDWorker::generateTextureControlled(const QString &prompt,
                                         const QImage &controlImage,
                                         const QString &outputPath)
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

    // Consume the one-shot reference FIRST: every early return below
    // (context failure, etc.) must also clear it, or a rejected edit's ref
    // would silently attach to the next unrelated generation.
    const QImage pendingRef = m_refImage;
    m_refImage = QImage();

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
        if (m_isFlux2) {
            // FLUX.2-klein is a 4-step guidance-distilled flow model: cfg
            // must stay at 1.0 (no negative prompt), euler (NOT ancestral),
            // few steps. The user's SD-oriented settings (30 steps, cfg 7)
            // produce garbage on it, so klein pins its own sampling.
            img_params.negative_prompt = "";
            img_params.sample_params.guidance.txt_cfg = 1.0f;
            // Unconditionally 4: the settings dialog's step count is an
            // SD-model knob (turbo/SDXL auto-detect writes 12/30 into it),
            // and klein is a 4-step distilled model — passing SD-oriented
            // values through just degrades it.
            img_params.sample_params.sample_steps = 4;
            img_params.sample_params.sample_method = EULER_SAMPLE_METHOD;
        }

        // FLUX.2 image editing: attach the one-shot reference image so the
        // prompt EDITS it (kontext-style). Buffers must outlive
        // generate_image, so they live in this scope; the ref is consumed
        // (cleared) whether or not this generation succeeds.
        QImage refRgb;
        QByteArray refBytes;
        sd_image_t refSdImage{};
        if (m_isFlux2 && !pendingRef.isNull()) {
            refRgb = pendingRef.convertToFormat(QImage::Format_RGB888);
            const int rowBytes = refRgb.width() * 3;
            refBytes.reserve(rowBytes * refRgb.height());
            for (int y = 0; y < refRgb.height(); ++y)
                refBytes.append(
                    reinterpret_cast<const char*>(refRgb.constScanLine(y)),
                    rowBytes);
            refSdImage.data    = reinterpret_cast<uint8_t*>(refBytes.data());
            refSdImage.width   = refRgb.width();
            refSdImage.height  = refRgb.height();
            refSdImage.channel = 3;
            img_params.ref_images            = &refSdImage;
            img_params.ref_images_count      = 1;
            img_params.auto_resize_ref_image = true;
            qDebug() << "SDWorker: FLUX.2 edit mode — 1 reference image";
        }

        // Issue #403: ControlNet depth conditioning. When a control
        // image was supplied and the context was built with a
        // ControlNet model, attach the (resized to generation res,
        // RGB8) depth map as the control image. `controlBytes` must
        // outlive generate_image, so it's declared in this scope.
        QImage controlRgb;
        QByteArray controlBytes;
        if (!controlImage.isNull() && !m_settings.controlNetPath.isEmpty()) {
            controlRgb = controlImage
                .scaled(m_settings.width, m_settings.height,
                        Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_RGB888);
            // sd.cpp expects a tightly-packed width*height*3 buffer.
            // QImage scanlines are padded to a 32-bit boundary, so we
            // must copy row-by-row (bytesPerLine may exceed width*3 for
            // widths that aren't a multiple of 4) rather than blit the
            // whole sizeInBytes() span.
            const int rowBytes = controlRgb.width() * 3;
            controlBytes.reserve(rowBytes * controlRgb.height());
            for (int y = 0; y < controlRgb.height(); ++y) {
                controlBytes.append(
                    reinterpret_cast<const char*>(controlRgb.constScanLine(y)),
                    rowBytes);
            }
            img_params.control_image.data =
                reinterpret_cast<uint8_t*>(controlBytes.data());
            img_params.control_image.width   = controlRgb.width();
            img_params.control_image.height  = controlRgb.height();
            img_params.control_image.channel = 3;
            img_params.control_strength = m_settings.controlStrength;
            qDebug() << "SDWorker: depth ControlNet active, strength"
                     << m_settings.controlStrength;
        }

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
    Q_UNUSED(controlImage);
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
