#include "ImageSourceController.h"

#include "ImageCaptioner.h"     // background SmolVLM caption of the picked image
#include "SDManager.h"          // text → image (FLUX.2 / any SD checkpoint)
#include "SDWorker.h"           // Flux2Set detection
#include "SentryReporter.h"

#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QMetaObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <thread>

ImageSourceController* ImageSourceController::s_instance = nullptr;

ImageSourceController::ImageSourceController(QObject* parent) : QObject(parent) {}

ImageSourceController* ImageSourceController::instance()
{
    // Unparented process-wide singleton, like the other PropertiesPanel controllers; kill() is called from the MainWindow teardown.
    if (!s_instance) s_instance = new ImageSourceController();
    return s_instance;
}

ImageSourceController* ImageSourceController::create(QQmlEngine*, QJSEngine*)
{
    QJSEngine::setObjectOwnership(instance(), QJSEngine::CppOwnership);
    return instance();
}

void ImageSourceController::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

void ImageSourceController::setLocked(const QObject* owner, bool locked)
{
    const bool was = !m_lockOwners.isEmpty();
    if (locked) m_lockOwners.insert(owner);
    else m_lockOwners.remove(owner);
    if (was != !m_lockOwners.isEmpty()) emit lockedChanged();
}

void ImageSourceController::setSelectedImage(const QString& path)
{
    if (path.isEmpty()) { clearSelectedImage(); return; }
    applySelectedImage(path);
}

void ImageSourceController::selectImage()
{
    if (m_lockOwners.size() > 0 || m_imageGenActive) return;
    const QString path = QFileDialog::getOpenFileName(
        nullptr, tr("Select an image"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.webp)"),
        nullptr, QFileDialog::DontUseNativeDialog);
    if (path.isEmpty()) return;
    applySelectedImage(path);
}

void ImageSourceController::applySelectedImage(const QString& path)
{
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
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("image source: select %1").arg(QFileInfo(path).fileName()));
    emit selectedImageChanged();
    emit statusMessage(tr("Selected: %1").arg(QFileInfo(path).fileName()));

    // Caption the image in the BACKGROUND now, so it's ready by the time the
    // (slow) mesh generation finishes and the AI texture pass needs it — no
    // blocking the UI to caption. Shown under the thumbnail as it lands.
    m_caption.clear();
    startCaptioning(path);
}

QString ImageSourceController::selectedImageUrl() const
{
    return m_selectedImage.isEmpty()
        ? QString()
        : QUrl::fromLocalFile(m_selectedImage).toString();
}

void ImageSourceController::clearSelectedImage()
{
    if (!m_lockOwners.isEmpty() || m_imageGenActive) return;
    m_selectedImage.clear();
    m_previewSource.clear();
    m_caption.clear();
    m_captioning = false;
    // Drop any in-flight caption for the cleared image — its late result
    // must not repopulate the caption of a selection that no longer exists.
    m_captionForPath.clear();
    emit selectedImageChanged();
    emit captionChanged();
    emit statusMessage(tr("Image cleared."));
}

int ImageSourceController::imageGenSizeForModel(const QString& model)
{
    if (SDManager::isFluxKleinModelName(model)) return 1024;
    // SDXL checkpoints train at 1024²; SD 1.x at 512² (bigger makes SD 1.5
    // duplicate the subject). Matches SDManager's own auto-detect naming.
    const QString lower = model.toLower();
    if (lower.contains(QLatin1String("sdxl")) || lower.contains(QLatin1String("sd_xl")))
        return 1024;
    return 512;
}

QString ImageSourceController::normalisedSavePath(const QString& destPath)
{
    QString dest = destPath.trimmed();
    if (dest.startsWith(QLatin1String("file://"))) dest = QUrl(dest).toLocalFile();
    if (!dest.isEmpty() && QFileInfo(dest).suffix().isEmpty())
        dest += QStringLiteral(".png");
    return dest;
}

QString ImageSourceController::suggestedImageFileName() const
{
    // Prefer the caption/prompt so a folder of saved images is readable;
    // fall back to the source file's own name.
    QString stem = m_caption.trimmed();
    if (stem.isEmpty()) stem = QFileInfo(m_selectedImage).completeBaseName();
    if (stem.isEmpty()) return QStringLiteral("generated_image.png");
    // Keep it filesystem-safe and short: captions are sentences, not names.
    stem.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9 _-]")), QString());
    stem = stem.simplified();
    stem.replace(' ', '_');
    if (stem.size() > 60) stem = stem.left(60);
    if (stem.isEmpty()) return QStringLiteral("generated_image.png");
    return stem + QStringLiteral(".png");
}

bool ImageSourceController::saveSelectedImageAs(const QString& destPath)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Save source image"));
    if (m_selectedImage.isEmpty() || !QFileInfo::exists(m_selectedImage)) {
        emit error(tr("There is no source image to save."));
        return false;
    }
    // QML's FileDialog hands back a file:// URL; a caller may pass a plain
    // path. One normaliser, shared with the interactive variant so the path
    // it RETURNS is the path this writes.
    const QString dest = normalisedSavePath(destPath);
    if (dest.isEmpty()) {
        emit error(tr("No save location chosen."));
        return false;
    }
    // Saving onto the source would delete it via the overwrite below.
    if (QFileInfo(dest).absoluteFilePath()
        == QFileInfo(m_selectedImage).absoluteFilePath()) {
        emit statusMessage(tr("Image is already saved there."));
        return true;
    }
    QDir().mkpath(QFileInfo(dest).absolutePath());
    // Write through QSaveFile: it stages into a temporary and commits on
    // commit(), so a failure part-way leaves an EXISTING destination intact.
    // Removing the target first (QFile::copy refuses to overwrite) would
    // destroy the user's previous file if the copy then failed.
    QFile src(m_selectedImage);
    if (!src.open(QIODevice::ReadOnly)) {
        emit error(tr("Could not read the source image."));
        return false;
    }
    QSaveFile out(dest);
    if (!out.open(QIODevice::WriteOnly)) {
        emit error(tr("Could not save the image to %1.").arg(dest));
        return false;
    }
    char buf[64 * 1024];
    while (!src.atEnd()) {
        const qint64 n = src.read(buf, sizeof(buf));
        if (n < 0 || out.write(buf, n) != n) {
            out.cancelWriting();     // existing destination untouched
            emit error(tr("Could not save the image to %1.").arg(dest));
            return false;
        }
    }
    if (!out.commit()) {
        emit error(tr("Could not save the image to %1.").arg(dest));
        return false;
    }
    emit statusMessage(tr("Saved %1").arg(QFileInfo(dest).fileName()));
    return true;
}

QString ImageSourceController::saveSelectedImageInteractive()
{
    if (m_selectedImage.isEmpty() || !QFileInfo::exists(m_selectedImage)) {
        emit error(tr("There is no source image to save."));
        return {};
    }
    const QString startDir =
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString suggested =
        QDir(startDir.isEmpty() ? QDir::homePath() : startDir)
            .filePath(suggestedImageFileName());
    // Same recipe as MaterialEditorQML::openFileDialog — a dialog opened
    // from a QQuickWidget context with a null parent and the NATIVE backend
    // silently fails to appear on macOS (the button looks dead). Pump the
    // queue, raise/activate the real window, parent to it, and use Qt's own
    // dialog.
    QApplication::processEvents();
    if (QWidget* activeWin = QApplication::activeWindow()) {
        activeWin->raise();
        activeWin->activateWindow();
    }
    QApplication::processEvents();
    const QString dest = QFileDialog::getSaveFileName(
        QApplication::activeWindow(), tr("Save Source Image"), suggested,
        tr("PNG image (*.png);;All files (*)"), nullptr,
        QFileDialog::DontUseNativeDialog
            | QFileDialog::DontUseCustomDirectoryIcons);
    if (dest.isEmpty()) return {};          // cancelled — not an error
    // Return the path actually WRITTEN: saveSelectedImageAs appends .png to a
    // suffix-less name, so returning the raw dialog result would hand callers
    // a path that does not exist.
    const QString written = normalisedSavePath(dest);
    return saveSelectedImageAs(written) ? written : QString();
}

// ── Prompt-to-3D: generate the SOURCE image from text (FLUX.2-klein via
// stable-diffusion.cpp) instead of importing one ─────────────────────────────

bool ImageSourceController::imageGenAvailable() const
{
#ifdef ENABLE_STABLE_DIFFUSION
    SDManager* sd = SDManager::instance();
    if (!sd) return false;
    return SDWorker::detectFlux2Set(SDManager::flux2KleinDirectory()).valid()
           || sd->isModelLoaded()
           || !sd->availableModels().isEmpty();
#else
    return false;
#endif
}

QString ImageSourceController::imageGenModelName() const
{
#ifdef ENABLE_STABLE_DIFFUSION
    SDManager* sd = SDManager::instance();
    // An EXPLICITLY LOADED model wins. Klein used to short-circuit here, so
    // downloading and loading another checkpoint (e.g. SDXL Base, the whole
    // point of offering a non-distilled option) silently still generated
    // with klein — the alternative was unusable unless klein was deleted.
    if (sd && sd->isModelLoaded() && !sd->currentModelName().isEmpty())
        return sd->currentModelName();
    if (SDWorker::detectFlux2Set(SDManager::flux2KleinDirectory()).valid())
        return SDManager::flux2KleinModelName();
    if (sd && !sd->availableModels().isEmpty())
        return sd->availableModels().first();
#endif
    return {};
}

void ImageSourceController::generateSourceImage(const QString& prompt)
{
#ifdef ENABLE_STABLE_DIFFUSION
    if (!m_lockOwners.isEmpty() || m_imageGenActive) {
        // The QML button flips its local busy flag BEFORE calling in — a
        // silent return would leave it stuck. Report the rejection.
        emit imageGenStatus(
            tr("Busy — wait for the current generation to finish."), true);
        return;
    }
    const QString trimmed = prompt.trimmed();
    if (trimmed.isEmpty()) {
        emit imageGenStatus(tr("Enter a prompt first."), true);
        return;
    }
    SDManager* sd = SDManager::instance();
    const QString model = imageGenModelName();
    if (!sd || model.isEmpty()) {
        emit imageGenStatus(
            tr("No image model — download FLUX.2-klein-4B in AI Model "
               "Settings (or install a Stable Diffusion model)."), true);
        return;
    }

    // EDIT mode: an image is already selected → the prompt describes a
    // CHANGE to it (FLUX.2 kontext-style reference editing). Only klein can
    // do that; on an SD checkpoint tell the user instead of silently
    // generating something unrelated.
    m_imageGenRef.clear();
    if (!m_selectedImage.isEmpty()) {
        if (!SDManager::isFluxKleinModelName(model)) {
            emit imageGenStatus(
                tr("Editing the loaded image needs FLUX.2-klein-4B or FLUX.3 (AI Model "
                   "Settings) — or remove the image (🗑) to generate from "
                   "scratch."), true);
            return;
        }
        m_imageGenRef = m_selectedImage;
    }

    // Fresh generation: steer toward what the 3D reconstruction wants — one
    // isolated subject on a plain backdrop. Edits keep the reference image's
    // composition, so no suffix there.
    // NB "full body" and "single subject" caption HUMAN figure photography
    // in the training data, and on a short prompt that association can beat
    // the subject itself: "capybara" reproducibly generated a person in
    // plain clothes on a grey backdrop — the suffix, rendered literally,
    // with the animal absent. Steering must describe the FRAMING without
    // implying a person.
    m_imageGenPrompt = m_imageGenRef.isEmpty()
        ? trimmed + QStringLiteral(", the entire subject fully visible, centered, "
                                   "isolated on a plain light gray background")
        : trimmed;
    // Explicit output name: SDManager's generationCompleted is GLOBAL, so a
    // texture generation finishing while we wait must not be mistaken for our
    // image — onImageGenCompleted correlates on this exact filename.
    m_imageGenFileName = QStringLiteral("prompt3d_%1.png")
        .arg(QDateTime::currentMSecsSinceEpoch());
    m_imageGenActive = true;

    // One-time signal wiring. NB: Qt::UniqueConnection only works with
    // member-function-pointer connections — on a LAMBDA it silently fails to
    // connect at all (which is how the progress bar stayed "indeterminate"
    // forever: the forwarder below never fired). Guard with a flag instead.
    if (!m_imageGenWired) {
        m_imageGenWired = true;
        connect(sd, &SDManager::modelLoadCompleted, this,
                &ImageSourceController::onImageGenModelLoaded);
        connect(sd, &SDManager::modelLoadError, this,
                &ImageSourceController::onImageGenError);
        connect(sd, &SDManager::generationCompleted, this,
                &ImageSourceController::onImageGenCompleted);
        connect(sd, &SDManager::generationError, this,
                &ImageSourceController::onImageGenError);
        connect(sd, &SDManager::generationProgressChanged, this, [this, sd]() {
            if (!m_imageGenActive) return;
            emit imageGenProgress(sd->generationStep(),
                                  sd->generationTotalSteps());
        });
    }

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.image_gen"),
        QStringLiteral("prompt image gen (%1)").arg(model));

    const bool loadedIsTarget =
        sd->isModelLoaded() && sd->currentModelName() == model;
    // Native training resolution: klein and SDXL are 1024 models; SD 1.x is
    // 512 (and duplicates the subject when pushed higher). Running SDXL at
    // 512 would quarter the pixel count of the very option added for quality.
    m_imageGenSize = imageGenSizeForModel(model);
    if (loadedIsTarget) {
        emit imageGenStatus(m_imageGenRef.isEmpty()
                                ? tr("Generating image…")
                                : tr("Editing the loaded image…"), false);
        sd->generateImage(m_imageGenPrompt, m_imageGenSize, m_imageGenSize,
                          m_imageGenFileName, m_imageGenRef);
    } else {
        emit imageGenStatus(tr("Loading %1…").arg(model), false);
        sd->loadModel(model);   // continues in onImageGenModelLoaded
    }
#else
    Q_UNUSED(prompt);
    emit imageGenStatus(
        tr("This build has no Stable Diffusion support."), true);
#endif
}

void ImageSourceController::onImageGenModelLoaded()
{
#ifdef ENABLE_STABLE_DIFFUSION
    if (!m_imageGenActive) return;
    emit imageGenStatus(m_imageGenRef.isEmpty()
                            ? tr("Generating image…")
                            : tr("Editing the loaded image…"), false);
    SDManager::instance()->generateImage(m_imageGenPrompt,
                                         m_imageGenSize, m_imageGenSize,
                                         m_imageGenFileName, m_imageGenRef);
#endif
}

void ImageSourceController::onImageGenCompleted(const QString& outputPath)
{
    if (!m_imageGenActive) return;   // someone else's SD generation
    // Correlate: only OUR file ends the run (a texture generation finishing
    // in parallel emits the same global signal with a different path).
    if (QFileInfo(outputPath).fileName() != m_imageGenFileName) return;
    m_imageGenActive = false;
    applySelectedImage(outputPath);
    emit imageGenStatus(
        tr("Image ready."), false);
}

void ImageSourceController::onImageGenError(const QString& message)
{
    if (!m_imageGenActive) return;
    m_imageGenActive = false;
    emit imageGenStatus(message, true);
}

void ImageSourceController::startCaptioning(const QString& path)
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
    QPointer<ImageSourceController> self(this);
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

void ImageSourceController::setCaptionResult(const QString& caption, const QString& forPath)
{
    // Drop a stale result if the user picked a different image meanwhile.
    if (forPath != m_captionForPath) return;
    m_caption = caption;
    m_captioning = false;
    emit captionChanged();
    if (!caption.isEmpty())
        emit statusMessage(tr("Image described: \"%1\"").arg(caption));
}

