#include "MeshGenController.h"

#include "MeshGenPredictor.h"
#include "MeshGenBuilder.h"
#include "BackgroundRemover.h"
#include "MeshImporterExporter.h"
#include "SentryReporter.h"

#include <OgreSceneNode.h>

#include <QBuffer>
#include <QByteArray>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QThread>

#include <thread>

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
    if (!s_instance) s_instance = new MeshGenController();
    return s_instance;
}

MeshGenController* MeshGenController::create(QQmlEngine*, QJSEngine*)
{
    QQmlEngine::setObjectOwnership(instance(), QQmlEngine::CppOwnership);
    return instance();
}

bool MeshGenController::available() const
{
    return MeshGenPredictor::isAvailable();
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
    emit selectedImageChanged();
    emit statusMessage(tr("Selected: %1").arg(QFileInfo(path).fileName()));
}

void MeshGenController::generateSelected(int resolution, bool removeBackground, int quality)
{
    if (m_selectedImage.isEmpty()) {
        emit error(tr("Select an image first."));
        return;
    }
    generate(m_selectedImage, resolution, removeBackground, quality);
}

MeshGenPredictor::Quality MeshGenController::qualityFromInt(int q)
{
    // Dropdown/CLI index: 0 = fp32 (best), 1 = int8 (smallest).
    return (q == 1) ? MeshGenPredictor::Quality::Int8
                    : MeshGenPredictor::Quality::Fp32;
}

void MeshGenController::pickImageAndGenerate(int resolution, bool removeBackground, int quality)
{
    if (m_busy) return;
    const QString path = QFileDialog::getOpenFileName(
        nullptr, tr("Select an image to reconstruct in 3D"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.webp)"),
        nullptr, QFileDialog::DontUseNativeDialog);
    if (path.isEmpty()) return;
    generate(path, resolution, removeBackground, quality);
}

void MeshGenController::generate(const QString& imagePath, int resolution,
                                 bool removeBackground, int quality)
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

    // Mark busy BEFORE ensureModelBlocking() — it spins a nested QEventLoop for the
    // first-use download, during which the QML button would otherwise stay enabled
    // and could re-enter generate(), racing over m_pending. setBusy disables it.
    m_cancel = false;
    setBusy(true);
    emit progress(QStringLiteral("prep"), 0, 1);

    // Ensure models on the MAIN thread first — ensureModelBlocking() spins a local
    // QEventLoop for the download, which must not run on the worker thread. Once
    // present, the worker only reads the files (no event loop needed).
    emit statusMessage(tr("Checking model…"));
    const QString enc = MeshGenPredictor::ensureModelBlocking(m_quality);
    if (enc.isEmpty() || !MeshGenPredictor::modelsPresent(m_quality)) {
        setBusy(false);
        emit error(tr("TripoSR model unavailable — it downloads on first use; if it "
                      "is not hosted yet, set QTMESH_TRIPOSR_MODEL_BASE_URL or drop "
                      "the files in the ai_models/triposr/ cache."));
        return;
    }
    if (removeBackground)
        BackgroundRemover::ensureModelBlocking();   // best-effort; falls back if absent

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.image_to_3d"),
        QStringLiteral("MeshGenController generate %1 res=%2 rembg=%3")
            .arg(fi.fileName()).arg(resolution).arg(removeBackground));

    emit statusMessage(tr("Preparing…"));

    delete m_pending;
    m_pending = new Pending();
    m_pending->imageName = fi.fileName();

    const int res = resolution;
    const bool rembg = removeBackground;

    // --- Worker thread: model download + background removal + inference --------
    // Everything here is pure data (no Ogre). Progress is emitted via a queued
    // connection so the GUI thread updates the bar.
    m_pending->worker = std::thread([this, image, res, rembg]() {
        auto post = [this](const QString& stage, int done, int total) {
            QMetaObject::invokeMethod(this, "progress", Qt::QueuedConnection,
                Q_ARG(QString, stage), Q_ARG(int, done), Q_ARG(int, total));
        };

        // Models are guaranteed present (ensured on the main thread before this
        // worker started), so this thread only reads files — no event loop needed.
        post(QStringLiteral("encode"), 0, 1);

        QImage subject = image;
        if (rembg) {
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
        opts.removeBackground = false;   // already handled above

        QMetaObject::invokeMethod(this, "statusMessage", Qt::QueuedConnection,
            Q_ARG(QString, tr("Reconstructing…")));

        auto progressFn = [this, &post](int done, int total) -> bool {
            post(QStringLiteral("decode"), done, total);
            return !m_cancel.load();
        };

        MeshGenPredictor::Result r = MeshGenPredictor::predict(
            subject, MeshGenPredictor::encoderModelPath(m_quality),
            MeshGenPredictor::decoderModelPath(), opts, progressFn);

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
    emit statusMessage(tr("Building mesh…"));

    Ogre::SceneNode* node =
        MeshGenBuilder::buildSceneNode(r, QStringLiteral("qtmesh_gen3d"));
    emit progress(QStringLiteral("build"), 1, 1);
    finish();

    if (!node) { emit error(tr("Failed to build mesh from prediction.")); return; }

    QVariantMap out{
        {"ok", true},
        {"vertexCount", r.vertexCount},
        {"triangleCount", r.triangleCount},
    };
    emit statusMessage(tr("Generated %1 verts, %2 tris")
                           .arg(r.vertexCount).arg(r.triangleCount));
    emit completed(out);
}
