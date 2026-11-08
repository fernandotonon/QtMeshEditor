#include "MeshGenController.h"

#include "GamificationManager.h"

#include "MeshGenPredictor.h"
#include "GameReadyPresets.h"
#include "TripoSGPredictor.h"
#include "TripoSplatPredictor.h"
#include "Trellis2Predictor.h"
#include "MeshGenBuilder.h"
#include "BackgroundRemover.h"
#include "MeshImporterExporter.h"
#include "SentryReporter.h"
#include "AIAssistManager.h"    // ensureUpscaleModel (main-thread model fetch)
#include "TextureUpscaler.h"    // worker-side Real-ESRGAN 2x on the baked diffuse
#include "ImageSourceController.h"   // the shared source image (picker, prompt, caption)

#include <OgreSceneNode.h>

#include <QBuffer>
#include <QByteArray>
#include <QApplication>
#include <QFileDialog>
#include <QDateTime>
#include <QDir>

#include <algorithm>
#include <QFileInfo>
#include <QStandardPaths>
#include <QImage>
#include <QCoreApplication>   // organizationName() — test-harness guard
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUrl>

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
    // the shared source image cannot be swapped or cleared under a running generation
    ImageSourceController::instance()->setLocked(this, b);
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

void MeshGenController::generateSelected(int resolution, bool removeBackground,
                                         int quality, const QVariantMap& options)
{
    const QString image = ImageSourceController::instance()->selectedImagePath();
    if (image.isEmpty()) {
        emit error(tr("Select an image first."));
        return;
    }
    generate(image, resolution, removeBackground, quality, options);
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

// Ensure the matting model(s) on the main thread (the worker cannot download).
// Best-effort: a failed BiRefNet fetch degrades to U²-Net. When Best is wanted,
// U²-Net is cached too so a BiRefNet INFERENCE failure on the worker can still
// fall back (removeBackgroundWithFallback). Stop is polled during the ~930 MB
// transfer. Returns false (after resetting busy) only when the user cancelled.
bool MeshGenController::ensureMatteModels(bool wantBest)
{
    const auto cancelPoll = [this]() { return m_cancel.load(); };
    BackgroundRemover::Quality ensuredQ = BackgroundRemover::Quality::Fast;
    BackgroundRemover::resolveModelBlocking(
        wantBest ? BackgroundRemover::Quality::Best
                 : BackgroundRemover::Quality::Fast,
        &ensuredQ, cancelPoll);
    if (wantBest && !m_cancel.load())
        BackgroundRemover::resolveModelBlocking(BackgroundRemover::Quality::Fast,
                                                nullptr, cancelPoll);
    if (m_cancel.load()) {
        setBusy(false);
        emit statusMessage(tr("Cancelled."));
        return false;
    }
    return true;
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
    // #1016: matting tier. BiRefNet Best is the default; only an explicit
    // "fast" opts out to U²-Net. Only meaningful when background removal is on.
    const bool wantBestMatte =
        options.value(QLatin1String("matting")).toString().toLower()
            != QLatin1String("fast");
    const bool wantSmooth  = optBool("smooth", true);
    const bool wantRefine  = optBool("refine", true);
    const bool wantBake    = optBool("bake_texture", true);
    const bool wantClean   = optBool("remove_degenerates", true);
    m_upscaleTexture       = optBool("upscale_texture", false);
    m_generatePbr          = optBool("generate_pbr", true) && wantBake;
    int textureSize = options.contains(QLatin1String("texture_size"))
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
    else if (backendStr.startsWith(QLatin1String("pixal")))
        backend = MeshGenPredictor::Backend::Pixal3D;
    else if (backendStr == QLatin1String("triposplat"))
        backend = MeshGenPredictor::Backend::TripoSplat;
    const bool useSG = (backend == MeshGenPredictor::Backend::TripoSG);
    const bool useSplat = (backend == MeshGenPredictor::Backend::TripoSplat);
    // Pixal3D shares the trellis-cli runtime, so every useT2 gate applies.
    const bool useT2 = MeshGenPredictor::isTrellisRuntime(backend);
    const int flowSteps = options.contains(QLatin1String("flow_steps"))
        ? options.value(QLatin1String("flow_steps")).toInt() : 25;
    const unsigned t2Seed = options.contains(QLatin1String("seed"))
        ? options.value(QLatin1String("seed")).toUInt() : 42u;
    const QString t2Preset = options.contains(QLatin1String("preset"))
        ? options.value(QLatin1String("preset")).toString().toLower()
        : QStringLiteral("balanced");
    int t2TargetTris = options.contains(QLatin1String("target_tris"))
        ? options.value(QLatin1String("target_tris")).toInt() : 0;
    bool t2TargetTrisStrict = false;
    int  t2MaxTexture = 0;      // platform cap on the FINAL images (0 = none)
    // A named preset (the picker's id) wins over a raw target_tris: it is
    // the same table the CLI/MCP resolve, and the Roblox presets carry the
    // platform's upload limits — a hard triangle ceiling plus a 1024 px
    // texture cap that is applied HERE, not only in QML, so a caller that
    // bypasses the picker's snap still gets a Roblox-legal bake.
    if (options.contains(QLatin1String("game_preset"))) {
        const QString id = options.value(QLatin1String("game_preset")).toString();
        if (const GameReady::Preset* gp = GameReady::find(id)) {
            t2TargetTris = gp->targetTriangles;
            t2TargetTrisStrict = gp->strictTriangles;
            t2MaxTexture = gp->maxTextureSize;
            if (gp->maxTextureSize > 0 && textureSize > gp->maxTextureSize)
                textureSize = gp->maxTextureSize;
            SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.image_to_3d"),
                QStringLiteral("game_preset=%1 tris=%2 strict=%3 tex=%4")
                    .arg(gp->id).arg(t2TargetTris).arg(t2TargetTrisStrict)
                    .arg(textureSize));
        } else if (!id.isEmpty()) {
            qWarning() << "MeshGenController: unknown game_preset" << id
                       << "- using target_tris" << t2TargetTris;
        }
    }
    // Subsamples per baked texel (TRELLIS.2). 1 = one sample at the texel
    // centre (default); 2 = 2x2, which averages four positions and softens
    // the speckle a single centre sample leaves where a texel straddles a
    // colour boundary — at roughly 4x the bake cost. Anything else is
    // ignored rather than trusted, since it comes in as a loose QVariant.
    const int t2Supersample =
        options.value(QLatin1String("texture_supersample")).toInt() == 2 ? 2 : 1;

    GamificationManager::noteFeature(QStringLiteral("image_to_3d"));

    // Mark busy BEFORE ensureModelBlocking() — it spins a nested QEventLoop for the
    // first-use download, during which the QML button would otherwise stay enabled
    // and could re-enter generate(), racing over m_pending. setBusy disables it.
    m_cancel = false;
    setBusy(true);
    // #1016: set once the matte model has been ensured, so the generic
    // pre-ensure below does not repeat a failed (and slow) Best download.
    bool matteEnsured = false;
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
        // event loop); the worker-side predictor only reads it. #1016: ensure
        // the REQUESTED tier — pre-ensuring Fast while the predictor then asks
        // for Best leaves BiRefNet uncached and silently degrades the matte.
        // resolveModelBlocking (not ensureModelBlocking) so a failed ~930 MB
        // BiRefNet fetch still leaves U²-Net cached: the worker cannot download
        // (no nested event loop off the main thread), so without the Fast
        // fallback HERE it would find nothing and skip matting entirely.
        matteEnsured = true;
        if (!ensureMatteModels(wantBestMatte))
            return;
    } else if (useSplat) {
        QString dlErr;
        emit statusMessage(tr("Checking TripoSplat models (~6 GB on first use)…"));
        // Forward Stop into ModelFetch's 200 ms cancel poll — without this the
        // ~6 GB first-use download ignores m_cancel until every graph finishes.
        if (!TripoSplatPredictor::ensureModelsBlocking(
                &dlErr, [this]() { return m_cancel.load(); })) {
            setBusy(false);
            if (m_cancel.load() || dlErr == QLatin1String("cancelled"))
                emit statusMessage(tr("Cancelled."));
            else
                emit error(tr("TripoSplat models unavailable: %1").arg(dlErr));
            return;
        }
        if (m_cancel.load()) {
            setBusy(false);
            emit statusMessage(tr("Cancelled."));
            return;
        }
        // TripoSplat mattes on the WORKER — which can only read models already
        // on disk, so ensure the matte here.
        matteEnsured = true;
        if (!ensureMatteModels(wantBestMatte))
            return;
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
    // Skip when the TRELLIS.2 branch above already ensured it: repeating a
    // failed Best fetch would burn a SECOND ~930 MB / 40-minute timeout before
    // generation even starts.
    if (removeBackground && !matteEnsured && !ensureMatteModels(wantBestMatte))
        return;

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
    m_pending->worker = std::thread([this, image, res, rembg, wantBestMatte,
                                     wantSmooth, wantRefine, wantBake,
                                     textureSize, useSG, useT2, useSplat,
                                     flowStepsSet = options.contains(QLatin1String("flow_steps")),
                                     flowSteps,
                                     backend, t2Seed, t2Preset, t2TargetTris,
                                     t2TargetTrisStrict, t2MaxTexture,
                                     t2Supersample, wantClean,
                                     imageStem = fi.completeBaseName()]() {
        auto post = [this](const QString& stage, int done, int total) {
            QMetaObject::invokeMethod(this, "progress", Qt::QueuedConnection,
                Q_ARG(QString, stage), Q_ARG(int, done), Q_ARG(int, total));
        };

        // Models are guaranteed present (ensured on the main thread before this
        // worker started), so this thread only reads files — no event loop
        // needed. (The encode stage is reported by the predictor itself.)
        QImage subject = image;
        if (rembg && !useSG && !useT2 && !useSplat) {
            // TripoSR path: composite over gray-128 (its training background).
            // TripoSG / TripoSplat / TRELLIS.2 leave removal to the predictor
            // dispatch (white / keep-alpha-over-black / keep-alpha respectively).
            post(QStringLiteral("background"), 0, 1);
            QMetaObject::invokeMethod(this, "statusMessage", Qt::QueuedConnection,
                Q_ARG(QString, tr("Removing background…")));
            // #1016: pick the model for the requested tier, degrading to Fast
            // when Best is not cached. Passing a Best tier with a Fast model
            // feeds a 1024² tensor into U²-Net's fixed 320² graph — inference
            // throws and the matte is silently lost.
            BackgroundRemover::Quality mq =
                wantBestMatte ? BackgroundRemover::Quality::Best
                              : BackgroundRemover::Quality::Fast;
            if (!BackgroundRemover::modelPresent(mq))
                mq = BackgroundRemover::Quality::Fast;
            BackgroundRemover::Options bgo;
            bgo.quality = mq;
            const QString bgModel = BackgroundRemover::modelPath(mq);
            const auto br = BackgroundRemover::removeBackgroundWithFallback(image, bgModel, bgo);
            subject = br.image;
            post(QStringLiteral("background"), 1, 1);
        }

        MeshGenPredictor::Options opts;
        opts.sdfResolution   = res;
        opts.vertexColor     = true;
        // TripoSR removal already ran above; TripoSG (white), TripoSplat
        // (keep-alpha → black via prepareImage) and TRELLIS.2 (keep-alpha)
        // matte inside their predictor dispatch. TRELLIS.2 ALWAYS gets the
        // matte (the CLI forces it too): its preprocess needs an alpha
        // channel to keep the non-commercial rembg model unused. The GUI
        // checkbox governs TripoSR / TripoSG / TripoSplat.
        opts.removeBackground = useT2 || (rembg && (useSG || useSplat));
        opts.mattingQuality  = wantBestMatte
            ? BackgroundRemover::Quality::Best
            : BackgroundRemover::Quality::Fast;
        opts.smoothMesh      = wantSmooth;
        opts.refineSurface   = wantRefine;
        opts.bakeTexture     = wantBake;
        opts.removeDegenerates = wantClean;
        opts.textureSize     = textureSize;
        opts.backend         = backend;
        opts.flowSteps       = flowSteps;
        opts.quality         = m_quality;
        // Game-ready simplification target — ALL backends (TripoSR/TripoSG
        // run the weld/debris/simplify + detail-normal-bake pass in the
        // predictor; TRELLIS.2 does it natively in its own pipeline).
        opts.targetTriangles = t2TargetTris;
        opts.targetTrianglesStrict = t2TargetTrisStrict;
        opts.maxTextureSize  = t2MaxTexture;
        opts.bakeNormalMap   = m_generatePbr;
        if (useSplat) {
            // TripoSplat defaults (TripoSplatPredictor::kDefault*): a 150k
            // game-ready target unless the picker set one, and the raw
            // Gaussians kept as a .ply. The splat→mesh grid resolution comes
            // from the shared Resolution picker (GUI) / --resolution (CLI) —
            // same control as TripoSR/TripoSG; 1024 is the recommended default
            // (matches splat spacing) and is what the QML combo snaps to when
            // this backend is selected.
            if (opts.targetTriangles <= 0)
                opts.targetTriangles = TripoSplatPredictor::kDefaultTargetTriangles;
            opts.seed = t2Seed;
            if (flowStepsSet)
                opts.splatFlowSteps = std::clamp(flowSteps, 1, 100);
            const QString dir = QDir(AppStorage::persistentRoot())
                                    .filePath(QStringLiteral("generated_splats"));
            QDir().mkpath(dir);
            // Unique per run (each costs ~25 min): a re-run with another seed,
            // or a same-named photo from another folder, must not replace it.
            opts.splatPlyPath = QDir(dir).filePath(
                QStringLiteral("%1_%2_splats.ply")
                    .arg(imageStem)
                    .arg(QDateTime::currentMSecsSinceEpoch()));
        }
        if (useT2) {
            opts.seed            = t2Seed;
            opts.trellis2Preset  = t2Preset;
            opts.textureSupersample = t2Supersample;
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
        // A platform preset caps the FINAL image (Roblox: 1024 px): a 2x
        // that would cross it is skipped, and the result says so.
        // Without a preset the 8192 px bake ceiling still applies: a 2x of
        // an 8192 bake would be a 16384² image (1 GB of RGBA) that most GPUs
        // cannot even sample.
        constexpr int kMaxTexturePx = 8192;
        const int upscaleCap = t2MaxTexture > 0 ? t2MaxTexture : kMaxTexturePx;
        const bool upscaleBlockedByCap = !r.texture.isNull()
            && (r.texture.width() * 2 > upscaleCap
                || r.texture.height() * 2 > upscaleCap);
        if (r.ok && m_upscaleTexture && upscaleBlockedByCap) {
            if (!r.warning.isEmpty()) r.warning += QStringLiteral(" ");
            r.warning += t2MaxTexture > 0
                ? tr("Upscale skipped: the preset caps textures at %1 px.")
                      .arg(t2MaxTexture)
                : tr("Upscale skipped: the texture is already at the %1 px maximum.")
                      .arg(kMaxTexturePx);
        }
        if (r.ok && m_upscaleTexture && !upscaleBlockedByCap
            && !m_upscaleModelPath.isEmpty()
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
    if (!r.splatPlyPath.isEmpty()) out["splatPath"] = r.splatPlyPath;
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

// ── Game-ready presets (GameReadyPresets.h) ─────────────────────────────────

QVariantList MeshGenController::gameReadyPresets() const
{
    QVariantList out;
    for (const GameReady::Preset& p : GameReady::presets()) {
        QVariantMap m;
        m.insert(QStringLiteral("id"), p.id);
        m.insert(QStringLiteral("label"), p.label);
        m.insert(QStringLiteral("tris"), p.targetTriangles);
        m.insert(QStringLiteral("strict"), p.strictTriangles);
        m.insert(QStringLiteral("maxTexture"), p.maxTextureSize);
        m.insert(QStringLiteral("note"), p.note);
        out.append(m);
    }
    return out;
}

int MeshGenController::gameReadyDefaultIndex() const
{
    const int i = GameReady::indexOf(GameReady::defaultId());
    return i < 0 ? 0 : i;
}
