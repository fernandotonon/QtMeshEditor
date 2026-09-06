#include "MeshGenController.h"

#include "GamificationManager.h"

#include "MeshGenPredictor.h"
#include "TripoSGPredictor.h"
#include "Trellis2Predictor.h"
#include "MeshGenBuilder.h"
#include "BackgroundRemover.h"
#include "MeshImporterExporter.h"
#include "SentryReporter.h"
#include "AIAssistManager.h"    // ensureUpscaleModel (main-thread model fetch)
#include "TextureUpscaler.h"    // worker-side Real-ESRGAN 2x on the baked diffuse
#include "ImageCaptioner.h"     // background SmolVLM caption of the picked image

#include <OgreSceneNode.h>

#include <QBuffer>
#include <QByteArray>
#include <QFileDialog>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QImage>
#include <QCoreApplication>   // organizationName() — test-harness guard
#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <thread>
#include "AppStorage.h"

// Holds the worker's output for the main-thread build step.
struct MeshGenController::Pending {
    MeshGenPredictor::Result result;
    QString imageName;
    std::thread worker;
    ~Pending() { if (worker.joinable()) worker.join(); }
};

MeshGenController* MeshGenController::s_instance = nullptr;

MeshGenController::MeshGenController(QObject* parent) : QObject(parent) {}

MeshGenController* MeshGenController::instance()
{
    // Unparented process-wide singleton — matches every other PropertiesPanel
    // controller (IsometricSpritesController, UvUnwrapController, …). Its lifetime
    // is bounded by kill(), called from the MainWindow teardown, so each
    // MainWindow/MCPServer test (which builds a fresh MainWindow + QQmlEngine
    // loading PropertiesPanel.qml, then tears it down) starts and ends clean. An
    // earlier attempt parented this to qApp and never killed it — the surviving
    // singleton was then referenced by the NEXT test's engine after the previous
    // engine had been destroyed, crashing MainWindowTest/MCPServerTest (signal 11).
    if (!s_instance) s_instance = new MeshGenController();
    return s_instance;
}

MeshGenController* MeshGenController::create(QQmlEngine* engine, QJSEngine*)
{
    // Match the IsometricSpritesController/other-controller pattern: only pin
    // CppOwnership when an engine is actually provided (guards the singleton from a
    // stale/second engine trying to take ownership).
    if (engine)
        QQmlEngine::setObjectOwnership(instance(), QQmlEngine::CppOwnership);
    return instance();
}

void MeshGenController::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

bool MeshGenController::available() const
{
    // In the unit-test harness, report unavailable so PropertiesPanel.qml's
    // "AI: Image → 3D" section stays collapsed and never instantiates its heavy
    // component tree (image preview, comboboxes, buttons). MainWindowTest builds
    // and destroys a real MainWindow — hence a QQmlEngine loading PropertiesPanel
    // — dozens of times under Mesa/Xvfb; adding this branch's extra QML surface to
    // every construct/destruct cycle perturbed the already-fragile GL teardown into
    // a SIGSEGV (the same class of failure the HDR first-run defaults hit — see
    // HdrBundledLibrary::applyFirstRunDefaultsIfNeeded's identical org-name guard).
    // The feature itself is unchanged for the real app; only the test harness (which
    // sets this org name) skips the surface. Pure-data pieces are covered directly.
    if (QCoreApplication::organizationName() == QLatin1String("QtMeshEditorTests"))
        return false;
    // The TRELLIS.2 sidecar backend needs no ONNX build — a machine with its
    // runtime installed gets the section even on a non-ONNX build.
    return MeshGenPredictor::isAvailable() || Trellis2Predictor::runtimeAvailable();
}

bool MeshGenController::trellis2Available() const
{
    return Trellis2Predictor::runtimeAvailable();
}

QString MeshGenController::trellis2RuntimeHint() const
{
    return Trellis2Predictor::runtimeDescription();
}

void MeshGenController::setBusy(bool b)
{
    if (m_busy == b) return;
    m_busy = b;
    emit busyChanged();
}

void MeshGenController::cancel()
{
    if (m_busy) {
        m_cancel = true;
        SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.image_to_3d"),
                                      QStringLiteral("MeshGenController cancel"));
        emit statusMessage(tr("Cancelling…"));
    }
}

bool MeshGenController::modelsPresent(int quality) const
{
    return MeshGenPredictor::modelsPresent(qualityFromInt(quality));
}

void MeshGenController::downloadModels(int quality)
{
    if (m_busy) return;
    if (!available()) {
        emit modelDownloadFinished(false);
        emit error(tr("Image-to-3D needs an ONNX build (rebuild with -DENABLE_ONNX)."));
        return;
    }
    const MeshGenPredictor::Quality q = qualityFromInt(quality);
    if (MeshGenPredictor::modelsPresent(q)) { emit modelDownloadFinished(true); return; }

    setBusy(true);
    emit statusMessage(tr("Downloading model…"));
    // Blocks on this (GUI) thread's event loop; ModelDownloader drives the shared
    // progress bar in the AI Settings dialog. Also fetch the bg-removal model.
    const QString enc = MeshGenPredictor::ensureModelBlocking(q);
    BackgroundRemover::ensureModelBlocking();
    const bool ok = !enc.isEmpty() && MeshGenPredictor::modelsPresent(q);
    setBusy(false);
    emit statusMessage(ok ? tr("Model ready.")
                          : tr("Model download failed (not hosted yet?)."));
    emit modelDownloadFinished(ok);
}

void MeshGenController::selectImage()
{
    if (m_busy) return;
    const QString path = QFileDialog::getOpenFileName(
        nullptr, tr("Select an image to reconstruct in 3D"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.webp)"),
        nullptr, QFileDialog::DontUseNativeDialog);
    if (path.isEmpty()) return;

    m_selectedImage = path;

    // Build a small preview thumbnail as a data:image/png;base64 URL (same idiom
    // as the texture-packer previews). Fit within 220px so it's a light payload.
    m_previewSource.clear();
    QImage img(path);
    if (!img.isNull()) {
        const QImage thumb = img.scaled(220, 220, Qt::KeepAspectRatio,
                                        Qt::SmoothTransformation);
        QByteArray png;
        QBuffer buf(&png);
        buf.open(QIODevice::WriteOnly);
        if (thumb.save(&buf, "PNG"))
            m_previewSource = QStringLiteral("data:image/png;base64,")
                              + QString::fromLatin1(png.toBase64());
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.image_to_3d"),
        QStringLiteral("MeshGenController selectImage %1").arg(QFileInfo(path).fileName()));
    emit selectedImageChanged();
    emit statusMessage(tr("Selected: %1").arg(QFileInfo(path).fileName()));

    // Caption the image in the BACKGROUND now, so it's ready by the time the
    // (slow) mesh generation finishes and the AI texture pass needs it — no
    // blocking the UI to caption. Shown under the thumbnail as it lands.
    m_caption.clear();
    startCaptioning(path);
}

void MeshGenController::startCaptioning(const QString& path)
{
    if (!ImageCaptioner::isAvailable() || path.isEmpty()) {
        m_captioning = false;
        emit captionChanged();
        return;
    }
    m_captionForPath = path;
    m_captioning = true;
    emit captionChanged();

    // Detached worker: ensure the model (first-use download) + caption, then
    // marshal the result back to the main thread via a queued invocation.
    QPointer<MeshGenController> self(this);
    std::thread([self, path]() {
        QString cap;
        const QString model = ImageCaptioner::ensureModelBlocking();
        if (!model.isEmpty()) {
            QImage img(path);
            if (!img.isNull())
                cap = ImageCaptioner::caption(img);
        }
        if (!self) return;
        QMetaObject::invokeMethod(self, "setCaptionResult", Qt::QueuedConnection,
                                  Q_ARG(QString, cap), Q_ARG(QString, path));
    }).detach();
}

void MeshGenController::setCaptionResult(const QString& caption, const QString& forPath)
{
    // Drop a stale result if the user picked a different image meanwhile.
    if (forPath != m_captionForPath) return;
    m_caption = caption;
    m_captioning = false;
    emit captionChanged();
    if (!caption.isEmpty())
        emit statusMessage(tr("Image described: \"%1\"").arg(caption));
}

void MeshGenController::generateSelected(int resolution, bool removeBackground,
                                         int quality, const QVariantMap& options)
{
    if (m_selectedImage.isEmpty()) {
        emit error(tr("Select an image first."));
        return;
    }
    generate(m_selectedImage, resolution, removeBackground, quality, options);
}

MeshGenPredictor::Quality MeshGenController::qualityFromInt(int q)
{
    // Dropdown/CLI index: 0 = fp32 (best), 1 = int8 (smallest).
    return (q == 1) ? MeshGenPredictor::Quality::Int8
                    : MeshGenPredictor::Quality::Fp32;
}

void MeshGenController::pickImageAndGenerate(int resolution, bool removeBackground,
                                             int quality, const QVariantMap& options)
{
    if (m_busy) return;
    const QString path = QFileDialog::getOpenFileName(
        nullptr, tr("Select an image to reconstruct in 3D"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.webp)"),
        nullptr, QFileDialog::DontUseNativeDialog);
    if (path.isEmpty()) return;
    generate(path, resolution, removeBackground, quality, options);
}

void MeshGenController::generate(const QString& imagePath, int resolution,
                                 bool removeBackground, int quality,
                                 const QVariantMap& options)
{
    if (m_busy) return;
    if (!available()) {
        emit error(tr("Image-to-3D needs an ONNX build (rebuild with -DENABLE_ONNX)."));
        return;
    }
    QFileInfo fi(imagePath);
    if (!fi.exists()) { emit error(tr("Image not found: %1").arg(imagePath)); return; }

    QImage image(fi.absoluteFilePath());
    if (image.isNull()) { emit error(tr("Could not load image: %1").arg(imagePath)); return; }

    m_quality = qualityFromInt(quality);

    // Parse the user-selectable pipeline stages (defaults match the predictor's
    // — see generateSelected()'s doc). Missing keys keep the defaults.
    auto optBool = [&options](const char* key, bool def) {
        return options.contains(QLatin1String(key))
            ? options.value(QLatin1String(key)).toBool() : def;
    };
    const bool wantSmooth  = optBool("smooth", true);
    const bool wantRefine  = optBool("refine", true);
    const bool wantBake    = optBool("bake_texture", true);
    m_upscaleTexture       = optBool("upscale_texture", false);
    m_generatePbr          = optBool("generate_pbr", true) && wantBake;
    const int  textureSize = options.contains(QLatin1String("texture_size"))
        ? options.value(QLatin1String("texture_size")).toInt() : 1024;
    // Backend: "trellis2" (the default whenever its sidecar runtime is
    // installed), "triposr" (fast + textured) or "triposg" (rectified flow —
    // higher-fidelity geometry, geometry-only, slower). Empty/unknown values
    // resolve through defaultBackend().
    const QString backendStr =
        options.value(QLatin1String("backend")).toString().toLower();
    MeshGenPredictor::Backend backend = MeshGenPredictor::defaultBackend();
    if (backendStr == QLatin1String("triposr"))
        backend = MeshGenPredictor::Backend::TripoSR;
    else if (backendStr == QLatin1String("triposg"))
        backend = MeshGenPredictor::Backend::TripoSG;
    else if (backendStr.startsWith(QLatin1String("trellis")))
        backend = MeshGenPredictor::Backend::Trellis2;
    const bool useSG = (backend == MeshGenPredictor::Backend::TripoSG);
    const bool useT2 = (backend == MeshGenPredictor::Backend::Trellis2);
    const int flowSteps = options.contains(QLatin1String("flow_steps"))
        ? options.value(QLatin1String("flow_steps")).toInt() : 25;
    const unsigned t2Seed = options.contains(QLatin1String("seed"))
        ? options.value(QLatin1String("seed")).toUInt() : 42u;
    const QString t2Preset = options.contains(QLatin1String("preset"))
        ? options.value(QLatin1String("preset")).toString().toLower()
        : QStringLiteral("balanced");
    const int t2TargetTris = options.contains(QLatin1String("target_tris"))
        ? options.value(QLatin1String("target_tris")).toInt() : 0;

    GamificationManager::noteFeature(QStringLiteral("image_to_3d"));

    // Mark busy BEFORE ensureModelBlocking() — it spins a nested QEventLoop for the
    // first-use download, during which the QML button would otherwise stay enabled
    // and could re-enter generate(), racing over m_pending. setBusy disables it.
    m_cancel = false;
    setBusy(true);
    emit progress(QStringLiteral("prep"), 0, 1);

    // Ensure the chosen backend's models on the MAIN thread first —
    // ensureModelBlocking() spins a local QEventLoop for the download, which
    // must not run on the worker thread. Once present, the worker only reads
    // the files (no event loop needed).
    emit statusMessage(tr("Checking model…"));
    if (useT2) {
        if (!Trellis2Predictor::runtimeAvailable()) {
            setBusy(false);
            emit error(Trellis2Predictor::runtimeDescription());
            return;
        }
        // The alpha-matte model must be ensured HERE (main thread — nested
        // event loop); the worker-side predictor only reads it.
        BackgroundRemover::ensureModelBlocking();
    } else if (useSG) {
        // TripoSG always runs the fp32 DiT — the int8 tier is dropped
        // (quantized geometry degrades to blobs; no ARM speed win).
        const QString enc = TripoSGPredictor::ensureModelBlocking(false);
        if (enc.isEmpty()) {
            setBusy(false);
            emit error(tr("TripoSG models unavailable — they download on first "
                          "use; if not hosted yet, set "
                          "QTMESH_TRIPOSG_MODEL_BASE_URL or drop the files in "
                          "the ai_models/triposg/ cache."));
            return;
        }
        // TripoSG's colour bake queries TripoSR's image-conditioned colour
        // field — ensure those models too (best-effort: if unavailable the
        // predictor falls back to the clay look with a warning).
        if (wantBake) {
            emit statusMessage(tr("Checking colour model…"));
            MeshGenPredictor::ensureModelBlocking(m_quality);
        }
    } else {
        const QString enc = MeshGenPredictor::ensureModelBlocking(m_quality);
        if (enc.isEmpty() || !MeshGenPredictor::modelsPresent(m_quality)) {
            setBusy(false);
            emit error(tr("TripoSR model unavailable — it downloads on first use; if it "
                          "is not hosted yet, set QTMESH_TRIPOSR_MODEL_BASE_URL or drop "
                          "the files in the ai_models/triposr/ cache."));
            return;
        }
    }
    if (removeBackground)
        BackgroundRemover::ensureModelBlocking();   // best-effort; falls back if absent

    // The optional post-bake upscale runs on the WORKER, so its model must be
    // ensured here on the main thread (event loop) first. Best-effort: an empty
    // path just skips the upscale later.
    m_upscaleModelPath.clear();
    if (m_upscaleTexture) {
        emit statusMessage(tr("Checking upscale model…"));
        m_upscaleModelPath = AIAssistManager::instance()->ensureUpscaleModel(2);
    }

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.image_to_3d"),
        QStringLiteral("MeshGenController generate %1 res=%2 rembg=%3 smooth=%4 "
                       "refine=%5 bake=%6 upscale=%7 pbr=%8")
            .arg(fi.fileName()).arg(resolution).arg(removeBackground)
            .arg(wantSmooth).arg(wantRefine).arg(wantBake)
            .arg(m_upscaleTexture).arg(m_generatePbr));

    emit statusMessage(tr("Preparing…"));

    delete m_pending;
    m_pending = new Pending();
    m_pending->imageName = fi.fileName();

    const int res = resolution;
    const bool rembg = removeBackground;

    // --- Worker thread: model download + background removal + inference --------
    // Everything here is pure data (no Ogre). Progress is emitted via a queued
    // connection so the GUI thread updates the bar.
    m_pending->worker = std::thread([this, image, res, rembg,
                                     wantSmooth, wantRefine, wantBake,
                                     textureSize, useSG, useT2, flowSteps,
                                     backend, t2Seed, t2Preset, t2TargetTris,
                                     imageStem = fi.completeBaseName()]() {
        auto post = [this](const QString& stage, int done, int total) {
            QMetaObject::invokeMethod(this, "progress", Qt::QueuedConnection,
                Q_ARG(QString, stage), Q_ARG(int, done), Q_ARG(int, total));
        };

        // Models are guaranteed present (ensured on the main thread before this
        // worker started), so this thread only reads files — no event loop
        // needed. (The encode stage is reported by the predictor itself.)
        QImage subject = image;
        if (rembg && !useSG && !useT2) {
            // TripoSR path: composite over gray-128 (its training background).
            // The TripoSG path leaves removal to the predictor dispatch, which
            // composites over WHITE per its reference pipeline.
            post(QStringLiteral("background"), 0, 1);
            QMetaObject::invokeMethod(this, "statusMessage", Qt::QueuedConnection,
                Q_ARG(QString, tr("Removing background…")));
            const QString bgModel = BackgroundRemover::modelPath();
            const auto br = BackgroundRemover::removeBackground(image, bgModel, {});
            subject = br.image;
            post(QStringLiteral("background"), 1, 1);
        }

        MeshGenPredictor::Options opts;
        opts.sdfResolution   = res;
        opts.vertexColor     = true;
        // TripoSR removal already ran above; TripoSG's white-background and
        // TRELLIS.2's keep-alpha matte removal happen inside the predictor
        // dispatch. TRELLIS.2 ALWAYS gets the matte (the CLI forces it too):
        // its preprocess needs an alpha channel to keep the non-commercial
        // rembg model unused, so the GUI checkbox only governs the Tripo
        // backends.
        opts.removeBackground = useT2 || (rembg && useSG);
        opts.smoothMesh      = wantSmooth;
        opts.refineSurface   = wantRefine;
        opts.bakeTexture     = wantBake;
        opts.textureSize     = textureSize;
        opts.backend         = backend;
        opts.flowSteps       = flowSteps;
        opts.quality         = m_quality;
        // Game-ready simplification target — ALL backends (TripoSR/TripoSG
        // run the weld/debris/simplify + detail-normal-bake pass in the
        // predictor; TRELLIS.2 does it natively in its own pipeline).
        opts.targetTriangles = t2TargetTris;
        opts.bakeNormalMap   = m_generatePbr;
        if (useT2) {
            opts.seed            = t2Seed;
            opts.trellis2Preset  = t2Preset;
            // Phase 9: keep the raw full-res generation in AppData so
            // textures/LODs can be re-baked without re-running inference.
            opts.trellis2SourceKeepDir =
                QDir(AppStorage::persistentRoot())
                    .filePath(QStringLiteral("generated_sources"));
            opts.trellis2SourceKeepBaseName = imageStem;
        }

        QMetaObject::invokeMethod(this, "statusMessage", Qt::QueuedConnection,
            Q_ARG(QString, tr("Reconstructing…")));

        // Map the predictor's typed stages onto the string stages the QML
        // step list keys on. total <= 0 = pure cancellation check (no post).
        auto progressFn = [this, &post](MeshGenPredictor::Stage st,
                                        int done, int total) -> bool {
            if (total > 0) {
                const char* name = nullptr;
                switch (st) {
                    case MeshGenPredictor::Stage::Encode:  name = "encode";  break;
                    case MeshGenPredictor::Stage::Denoise: name = "denoise"; break;
                    case MeshGenPredictor::Stage::Decode:  name = "decode";  break;
                    case MeshGenPredictor::Stage::Refine:  name = "refine";  break;
                    case MeshGenPredictor::Stage::Bake:    name = "bake";    break;
                    case MeshGenPredictor::Stage::Color:   name = "color";   break;
                }
                if (name) post(QString::fromLatin1(name), done, total);
            }
            return !m_cancel.load();
        };

        MeshGenPredictor::Result r = MeshGenPredictor::predict(
            subject, MeshGenPredictor::encoderModelPath(m_quality),
            MeshGenPredictor::decoderModelPath(), opts, progressFn);

        // Optional Real-ESRGAN 2x on the baked diffuse — pure CPU, so it stays
        // on this worker. Model was ensured on the main thread; best-effort.
        if (r.ok && m_upscaleTexture && !m_upscaleModelPath.isEmpty()
            && !r.uvs.empty() && !r.texture.isNull() && !m_cancel.load()) {
            QMetaObject::invokeMethod(this, "statusMessage", Qt::QueuedConnection,
                Q_ARG(QString, tr("Upscaling texture…")));
            const TextureUpscaler::Result ur = TextureUpscaler::upscale(
                r.texture, m_upscaleModelPath, {},
                [this, &post](int done, int total) {
                    post(QStringLiteral("upscale"), done, total);
                    return !m_cancel.load();
                });
            if (ur.ok && !ur.image.isNull())
                r.texture = ur.image;
        }

        m_pending->result = std::move(r);
        // Hand back to the main thread to build/attach the Ogre mesh.
        QMetaObject::invokeMethod(this, "buildOnMainThread", Qt::QueuedConnection);
    });
}

void MeshGenController::buildOnMainThread()
{
    // Runs on the GUI/main thread (queued from the worker). Join the worker first.
    if (m_pending && m_pending->worker.joinable())
        m_pending->worker.join();

    auto finish = [this]() { setBusy(false); m_cancel = false; };

    if (!m_pending) { finish(); return; }
    const MeshGenPredictor::Result& r = m_pending->result;

    if (!r.ok) {
        finish();
        emit error(r.error.isEmpty() ? tr("Image-to-3D failed.") : r.error);
        return;
    }

    emit progress(QStringLiteral("build"), 0, 1);
    emit statusMessage(m_generatePbr && !r.uvs.empty()
                           ? tr("Building mesh + PBR maps…")
                           : tr("Building mesh…"));
    // Give the QML a chance to repaint the step list before buildSceneNode
    // blocks this (main) thread on mesh construction + PBR synthesis.
    QCoreApplication::processEvents();

    // PBR synthesis (when enabled) runs inside buildSceneNode on this (main)
    // thread — same as the Material Editor's button; the PBRify models are
    // small and download on first use via the main-thread event loop.
    MeshGenBuilder::BuildOptions buildOpts;
    buildOpts.generatePbrMaps = m_generatePbr;
    Ogre::SceneNode* node =
        MeshGenBuilder::buildSceneNode(r, QStringLiteral("qtmesh_gen3d"), buildOpts);
    emit progress(QStringLiteral("build"), 1, 1);
    finish();

    if (!node) { emit error(tr("Failed to build mesh from prediction.")); return; }

    QVariantMap out{
        {"ok", true},
        {"vertexCount", r.vertexCount},
        {"triangleCount", r.triangleCount},
    };
    // Expose the built entity's name so QML can run a follow-up AI texture bake
    // on it (the "Generate texture (AI)" option for the geometry-only TripoSG
    // backend). The node has exactly one attached Entity.
    if (node->numAttachedObjects() > 0) {
        if (auto* obj = node->getAttachedObject(0))
            out["entityName"] = QString::fromStdString(obj->getName());
    }
    if (!r.warning.isEmpty()) out["warning"] = r.warning;
    emit statusMessage(!r.warning.isEmpty()
                           ? tr("Generated %1 verts, %2 tris (%3)")
                                 .arg(r.vertexCount).arg(r.triangleCount).arg(r.warning)
                           : r.uvs.empty()
                                 ? tr("Generated %1 verts, %2 tris")
                                       .arg(r.vertexCount).arg(r.triangleCount)
                                 : tr("Generated %1 verts, %2 tris + %3px texture")
                                       .arg(r.vertexCount).arg(r.triangleCount)
                                       .arg(r.texture.width()));
    emit completed(out);
}
