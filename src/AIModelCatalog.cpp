#include "AIModelCatalog.h"

#include "ModelDownloader.h"
#include "SentryReporter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QSet>
#include <QTimer>
#include <QUrl>

AIModelCatalog* AIModelCatalog::s_instance = nullptr;

namespace {
QString joinUrl(QString base, const QString& fileName)
{
    if (base.isEmpty())
        return {};
    if (!base.endsWith('/'))
        base += '/';
    return base + fileName;
}

QString capabilityForFeature(const QString& feature)
{
    const QString f = feature.toLower();
    if (f.contains(QStringLiteral("mesh tools"))) return QStringLiteral("segmentation");
    if (f.contains(QStringLiteral("image to 3d"))) return QStringLiteral("generation");
    if (f.contains(QStringLiteral("animation"))) return QStringLiteral("mocap");
    if (f.contains(QStringLiteral("texture"))) return QStringLiteral("material");
    return QStringLiteral("other");
}

QString sourceForModelId(const QString& id)
{
    if (id.startsWith(QStringLiteral("tripos")) || id.contains(QStringLiteral("caption")))
        return QStringLiteral("huggingface");
    return QStringLiteral("bundled");
}
}

AIModelCatalog* AIModelCatalog::instance()
{
    if (!s_instance)
        s_instance = new AIModelCatalog();
    return s_instance;
}

AIModelCatalog* AIModelCatalog::qmlInstance(QQmlEngine*, QJSEngine*)
{
    AIModelCatalog* catalog = instance();
    QJSEngine::setObjectOwnership(catalog, QJSEngine::CppOwnership);
    return catalog;
}

AIModelCatalog::AIModelCatalog(QObject* parent)
    : QObject(parent)
{
    auto* dl = ModelDownloader::instance();
    connect(dl, &ModelDownloader::downloadCompleted, this,
            [this](const QString& name, const QString&) {
                if (!m_busy || m_pendingFiles.isEmpty())
                    return;
                if (m_pendingFiles.first().label != name)
                    return;
                m_pendingFiles.removeFirst();
                SentryReporter::addBreadcrumb(QStringLiteral("ai.model_download"),
                                              QStringLiteral("downloaded catalog file"));
                emit modelsChanged();
                QTimer::singleShot(0, this, &AIModelCatalog::startNextQueuedFile);
            });
    connect(dl, &ModelDownloader::downloadError, this,
            [this](const QString& name, const QString& error) {
                if (!m_busy || m_pendingFiles.isEmpty())
                    return;
                if (m_pendingFiles.first().label != name)
                    return;
                QList<ModelSpec> owner;
                const ModelSpec* spec = findSpec(m_activeModelId, &owner);
                captureModelTelemetry(QStringLiteral("ai.model_download.failed"), spec,
                                      m_activeDownloadStartedMs > 0
                                          ? QDateTime::currentMSecsSinceEpoch() - m_activeDownloadStartedMs : -1,
                                      error);
                SentryReporter::addBreadcrumb(QStringLiteral("ai.model_download"),
                                              QStringLiteral("download failed"),
                                              QStringLiteral("error"));
                m_pendingFiles.clear();
                setStatusMessage(tr("Download failed: %1").arg(error));
                clearActive();
                setBusy(false);
                emit modelsChanged();
            });
    connect(dl, &ModelDownloader::downloadCanceled, this,
            [this](const QString& name) {
                if (!m_busy || m_pendingFiles.isEmpty())
                    return;
                if (m_pendingFiles.first().label != name)
                    return;
                QList<ModelSpec> owner;
                captureModelTelemetry(QStringLiteral("ai.model_download.canceled"),
                                      findSpec(m_activeModelId, &owner),
                                      m_activeDownloadStartedMs > 0
                                          ? QDateTime::currentMSecsSinceEpoch() - m_activeDownloadStartedMs : -1);
                SentryReporter::addBreadcrumb(QStringLiteral("ai.model_download"),
                                              QStringLiteral("download canceled"),
                                              QStringLiteral("warning"));
                m_pendingFiles.clear();
                setStatusMessage(tr("Download canceled."));
                clearActive();
                setBusy(false);
                emit modelsChanged();
            });
}

QString AIModelCatalog::rootPath() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
        .filePath(QStringLiteral("ai_models"));
}

QString AIModelCatalog::modelsRootUrl() const
{
    return QUrl::fromLocalFile(rootPath()).toString();
}

QString AIModelCatalog::resolveBaseUrl(const char* settingsKey, const char* envKey,
                                       const char* fallback) const
{
    QSettings settings;
    QString base = settings.value(QString::fromLatin1(settingsKey)).toString();
    if (base.isEmpty()) {
        const QByteArray env = qgetenv(envKey);
        base = env.isEmpty() ? QString::fromLatin1(fallback) : QString::fromUtf8(env);
    }
    return base;
}

QList<AIModelCatalog::ModelSpec> AIModelCatalog::specs() const
{
    const QString root = rootPath();
    auto path = [&](const QString& relative) {
        return QDir(root).filePath(relative);
    };
    auto file = [&](const QString& relative, const QString& fileName,
                    const QString& base, const QString& label) {
        return FileSpec{fileName, path(relative + QLatin1Char('/') + fileName),
                        joinUrl(base, fileName), label};
    };

#ifdef ENABLE_ONNX
    constexpr bool onnxAvailable = true;
#else
    constexpr bool onnxAvailable = false;
#endif
#ifdef ENABLE_MTMD
    constexpr bool captionAvailable = true;
#else
    constexpr bool captionAvailable = false;
#endif

    const QString pbrBase = resolveBaseUrl(
        "ai/pbrModelBaseUrl", "QTMESH_PBR_MODEL_BASE_URL",
        "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/");
    const QString rembgBase = resolveBaseUrl(
        "ai/rembgModelBaseUrl", "QTMESH_REMBG_MODEL_BASE_URL",
        "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/rembg/");
    const QString triposrBase = resolveBaseUrl(
        "ai/triposrModelBaseUrl", "QTMESH_TRIPOSR_MODEL_BASE_URL",
        "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/triposr/");
    const QString triposgBase = resolveBaseUrl(
        "ai/triposgModelBaseUrl", "QTMESH_TRIPOSG_MODEL_BASE_URL",
        "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/triposg/");
    const QString unirigBase = resolveBaseUrl(
        "ai/unirigModelBaseUrl", "QTMESH_UNIRIG_MODEL_BASE_URL",
        "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/unirig/");
    const QString skinTokensBase = resolveBaseUrl(
        "ai/skintokensModelBaseUrl", "QTMESH_SKINTOKENS_MODEL_BASE_URL",
        "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/skintokens/");
    const QString inbetweenBase = resolveBaseUrl(
        "ai/inbetweenModelBaseUrl", "QTMESH_INBETWEEN_MODEL_BASE_URL",
        "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/inbetween/");
    const QString motionBase = resolveBaseUrl(
        "ai/motionLibraryBaseUrl", "QTMESH_MOTION_LIBRARY_BASE_URL",
        "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/motion/");
    const QString t2mBase = resolveBaseUrl(
        "ai/t2mModelBaseUrl", "QTMESH_T2M_MODEL_BASE_URL",
        "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/motion/");
    const QString segmentBase = resolveBaseUrl(
        "ai/segmentModelBaseUrl", "QTMESH_SEGMENT_MODEL_BASE_URL",
        "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/segment/");
    const QString captionBase = resolveBaseUrl(
        "ai/captionModelBaseUrl", "QTMESH_CAPTION_MODEL_BASE_URL",
        "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/caption/");

    QList<ModelSpec> out;
    out << ModelSpec{
        QStringLiteral("pbr-maps"), tr("PBR Map Synthesis"), tr("Textures"),
        tr("Normal, roughness, and height ONNX models used to synthesize PBR maps from an albedo texture."),
        QStringLiteral("Small"), onnxAvailable ? QString() : tr("Requires an ONNX-enabled build"),
        onnxAvailable,
        {
            file(QStringLiteral("pbr"), QStringLiteral("1x-PBRify_NormalV3.onnx"), pbrBase, QStringLiteral("PBR Normal")),
            file(QStringLiteral("pbr"), QStringLiteral("1x-PBRify_RoughnessV2.onnx"), pbrBase, QStringLiteral("PBR Roughness")),
            file(QStringLiteral("pbr"), QStringLiteral("1x-PBRify_Height.onnx"), pbrBase, QStringLiteral("PBR Height")),
        }};
    out << ModelSpec{
        QStringLiteral("upscale-2x"), tr("Texture Upscaler 2x"), tr("Textures"),
        tr("Real-ESRGAN 2x ONNX model used for texture upscaling."),
        QStringLiteral("Medium"), onnxAvailable ? QString() : tr("Requires an ONNX-enabled build"),
        onnxAvailable,
        { file(QStringLiteral("pbr"), QStringLiteral("RealESRGAN_x2plus.onnx"), pbrBase, QStringLiteral("Upscale 2x")) }};
    out << ModelSpec{
        QStringLiteral("upscale-4x"), tr("Texture Upscaler 4x"), tr("Textures"),
        tr("Real-ESRGAN 4x ONNX model used for texture upscaling."),
        QStringLiteral("Medium"), onnxAvailable ? QString() : tr("Requires an ONNX-enabled build"),
        onnxAvailable,
        { file(QStringLiteral("pbr"), QStringLiteral("RealESRGAN_x4plus.onnx"), pbrBase, QStringLiteral("Upscale 4x")) }};
    out << ModelSpec{
        QStringLiteral("background-removal"), tr("Background Removal"), tr("Image to 3D"),
        tr("U2Net ONNX model used to remove backgrounds before image-to-3D generation."),
        QStringLiteral("~170 MB"), onnxAvailable ? QString() : tr("Requires an ONNX-enabled build"),
        onnxAvailable,
        { file(QStringLiteral("rembg"), QStringLiteral("u2net.onnx"), rembgBase, QStringLiteral("U2Net background-removal model")) }};
    out << ModelSpec{
        QStringLiteral("triposr-fp32"), tr("TripoSR fp32"), tr("Image to 3D"),
        tr("High quality TripoSR image encoder and decoder used for image-to-3D mesh generation."),
        QStringLiteral("~1.7 GB"), onnxAvailable ? QString() : tr("Requires an ONNX-enabled build"),
        onnxAvailable,
        {
            file(QStringLiteral("triposr"), QStringLiteral("triposr_encoder.onnx"), triposrBase, QStringLiteral("TripoSR encoder model")),
            file(QStringLiteral("triposr"), QStringLiteral("triposr_decoder.onnx"), triposrBase, QStringLiteral("TripoSR decoder model")),
        }};
    out << ModelSpec{
        QStringLiteral("triposr-int8"), tr("TripoSR int8"), tr("Image to 3D"),
        tr("Smaller quantized TripoSR encoder plus the shared decoder for image-to-3D mesh generation."),
        QStringLiteral("~430 MB + decoder"), onnxAvailable ? QString() : tr("Requires an ONNX-enabled build"),
        onnxAvailable,
        {
            file(QStringLiteral("triposr"), QStringLiteral("triposr_encoder_int8.onnx"), triposrBase, QStringLiteral("TripoSR encoder model")),
            file(QStringLiteral("triposr"), QStringLiteral("triposr_decoder.onnx"), triposrBase, QStringLiteral("TripoSR decoder model")),
        }};
    out << ModelSpec{
        QStringLiteral("triposg-fp32"), tr("TripoSG fp32"), tr("Image to 3D"),
        tr("Large TripoSG rectified-flow image-to-3D model, including the external DiT weights sidecar."),
        QStringLiteral("~3 GB+"), onnxAvailable ? QString() : tr("Requires an ONNX-enabled build"),
        onnxAvailable,
        {
            file(QStringLiteral("triposg"), QStringLiteral("triposg_image_encoder.onnx"), triposgBase, QStringLiteral("TripoSG triposg_image_encoder.onnx")),
            file(QStringLiteral("triposg"), QStringLiteral("triposg_vae_latents.onnx"), triposgBase, QStringLiteral("TripoSG triposg_vae_latents.onnx")),
            file(QStringLiteral("triposg"), QStringLiteral("triposg_vae_decoder.onnx"), triposgBase, QStringLiteral("TripoSG triposg_vae_decoder.onnx")),
            file(QStringLiteral("triposg"), QStringLiteral("triposg_dit_step.onnx"), triposgBase, QStringLiteral("TripoSG triposg_dit_step.onnx")),
            file(QStringLiteral("triposg"), QStringLiteral("triposg_dit_step.onnx.data"), triposgBase, QStringLiteral("TripoSG triposg_dit_step.onnx.data")),
        }};
    out << ModelSpec{
        QStringLiteral("triposg-int8"), tr("TripoSG int8"), tr("Image to 3D"),
        tr("Smaller TripoSG DiT tier plus shared image encoder and VAE models."),
        QStringLiteral("Large"), onnxAvailable ? QString() : tr("Requires an ONNX-enabled build"),
        onnxAvailable,
        {
            file(QStringLiteral("triposg"), QStringLiteral("triposg_image_encoder.onnx"), triposgBase, QStringLiteral("TripoSG triposg_image_encoder.onnx")),
            file(QStringLiteral("triposg"), QStringLiteral("triposg_vae_latents.onnx"), triposgBase, QStringLiteral("TripoSG triposg_vae_latents.onnx")),
            file(QStringLiteral("triposg"), QStringLiteral("triposg_vae_decoder.onnx"), triposgBase, QStringLiteral("TripoSG triposg_vae_decoder.onnx")),
            file(QStringLiteral("triposg"), QStringLiteral("triposg_dit_step_int8.onnx"), triposgBase, QStringLiteral("TripoSG triposg_dit_step_int8.onnx")),
        }};
    out << ModelSpec{
        QStringLiteral("unirig"), tr("UniRig Auto-Rigging"), tr("Rigging"),
        tr("Encoder, decoder, and embedding ONNX models used for learned auto-rig joint prediction."),
        QStringLiteral("~1.4 GB"), onnxAvailable ? QString() : tr("Requires an ONNX-enabled build"),
        onnxAvailable,
        {
            file(QStringLiteral("unirig"), QStringLiteral("encoder.onnx"), unirigBase, QStringLiteral("UniRig encoder model")),
            file(QStringLiteral("unirig"), QStringLiteral("decoder.onnx"), unirigBase, QStringLiteral("UniRig decoder model")),
            file(QStringLiteral("unirig"), QStringLiteral("embed.onnx"), unirigBase, QStringLiteral("UniRig embed model")),
        }};
    out << ModelSpec{
        QStringLiteral("skintokens"), tr("SkinTokens Skinning"), tr("Rigging"),
        tr("Manifest and ONNX models used for learned skin-weight generation."),
        QStringLiteral("Large"), onnxAvailable ? QString() : tr("Requires an ONNX-enabled build"),
        onnxAvailable,
        {
            file(QStringLiteral("skintokens"), QStringLiteral("skintokens.json"), skinTokensBase, QStringLiteral("SkinTokens manifest")),
            file(QStringLiteral("skintokens"), QStringLiteral("mesh_cond.onnx"), skinTokensBase, QStringLiteral("SkinTokens mesh conditioner")),
            file(QStringLiteral("skintokens"), QStringLiteral("vae_cond.onnx"), skinTokensBase, QStringLiteral("SkinTokens VAE conditioner")),
            file(QStringLiteral("skintokens"), QStringLiteral("embed.onnx"), skinTokensBase, QStringLiteral("SkinTokens embedding model")),
            file(QStringLiteral("skintokens"), QStringLiteral("decoder.onnx"), skinTokensBase, QStringLiteral("SkinTokens decoder model")),
            file(QStringLiteral("skintokens"), QStringLiteral("decoder.onnx.data"), skinTokensBase, QStringLiteral("SkinTokens decoder weights")),
            file(QStringLiteral("skintokens"), QStringLiteral("skin_decode.onnx"), skinTokensBase, QStringLiteral("SkinTokens skin decoder")),
        }};
    out << ModelSpec{
        QStringLiteral("motion-inbetween"), tr("Motion In-Betweening"), tr("Animation"),
        tr("RMIB ONNX model used to interpolate animation key poses with a learned motion prior."),
        QStringLiteral("~10 MB"), onnxAvailable ? QString() : tr("Requires an ONNX-enabled build"),
        onnxAvailable,
        { file(QStringLiteral("inbetween"), QStringLiteral("rmib.onnx"), inbetweenBase, QStringLiteral("RMIB in-betweening model")) }};
    out << ModelSpec{
        QStringLiteral("motion-library"), tr("Motion Library"), tr("Animation"),
        tr("Prompt-matched animation template library used for text-driven motion generation fallback."),
        QStringLiteral("~1 MB"), QString(), true,
        { file(QStringLiteral("motion"), QStringLiteral("motion-library.json"), motionBase, QStringLiteral("Motion library")) }};
    out << ModelSpec{
        QStringLiteral("text-to-motion"), tr("Text-to-Motion"), tr("Animation"),
        tr("ONNX model and vocabulary used to generate motion from text prompts."),
        QStringLiteral("Small"), onnxAvailable ? QString() : tr("Requires an ONNX-enabled build"),
        onnxAvailable,
        {
            file(QStringLiteral("motion"), QStringLiteral("t2m.onnx"), t2mBase, QStringLiteral("Text-to-motion model")),
            file(QStringLiteral("motion"), QStringLiteral("t2m-vocab.json"), t2mBase, QStringLiteral("Text-to-motion vocab")),
        }};
    out << ModelSpec{
        QStringLiteral("mesh-segmentation"), tr("Mesh Segmentation"), tr("Mesh Tools"),
        tr("ONNX models used to classify mesh categories and parts for segmentation-aware tools."),
        QStringLiteral("Small"), onnxAvailable ? QString() : tr("Requires an ONNX-enabled build"),
        onnxAvailable,
        {
            file(QStringLiteral("segment"), QStringLiteral("meshseg.onnx"), segmentBase, QStringLiteral("Body mesh segmentation model")),
            file(QStringLiteral("segment"), QStringLiteral("meshseg_vegetation.onnx"), segmentBase, QStringLiteral("Vegetation mesh segmentation model")),
            file(QStringLiteral("segment"), QStringLiteral("meshseg_vehicle.onnx"), segmentBase, QStringLiteral("Vehicle mesh segmentation model")),
            file(QStringLiteral("segment"), QStringLiteral("meshseg_building.onnx"), segmentBase, QStringLiteral("Building mesh segmentation model")),
            file(QStringLiteral("segment"), QStringLiteral("meshseg_category.onnx"), segmentBase, QStringLiteral("Mesh category classifier")),
        }};
    out << ModelSpec{
        QStringLiteral("image-captioning"), tr("Image Captioning"), tr("Image to 3D"),
        tr("SmolVLM GGUF model and vision projector used to caption reference images."),
        QStringLiteral("< 600 MB"), captionAvailable ? QString() : tr("Requires an MTMD-enabled build"),
        captionAvailable,
        {
            file(QStringLiteral("caption"), QStringLiteral("SmolVLM-500M-Instruct-Q8_0.gguf"), captionBase, QStringLiteral("Caption SmolVLM-500M-Instruct-Q8_0.gguf")),
            file(QStringLiteral("caption"), QStringLiteral("mmproj-SmolVLM-500M-Instruct-Q8_0.gguf"), captionBase, QStringLiteral("Caption mmproj-SmolVLM-500M-Instruct-Q8_0.gguf")),
        }};
    return out;
}

QVariantList AIModelCatalog::models() const
{
    QVariantList list;
    const auto all = specs();
    for (const ModelSpec& spec : all)
        list << toVariantMap(spec);
    return list;
}

QVariantMap AIModelCatalog::toVariantMap(const ModelSpec& spec) const
{
    int present = 0;
    QVariantList files;
    for (const FileSpec& f : spec.files) {
        const bool exists = QFileInfo::exists(f.path);
        if (exists)
            ++present;
        files << QVariantMap{
            {QStringLiteral("fileName"), f.fileName},
            {QStringLiteral("path"), f.path},
            {QStringLiteral("downloaded"), exists},
        };
    }

    const bool downloaded = present == spec.files.size();
    const bool partial = present > 0 && !downloaded;
    return {
        {QStringLiteral("id"), spec.id},
        {QStringLiteral("name"), spec.name},
        {QStringLiteral("feature"), spec.feature},
        {QStringLiteral("description"), spec.description},
        {QStringLiteral("size"), spec.size},
        {QStringLiteral("buildRequirement"), spec.buildRequirement},
        {QStringLiteral("available"), spec.available},
        {QStringLiteral("downloaded"), downloaded},
        {QStringLiteral("partial"), partial},
        {QStringLiteral("presentFiles"), present},
        {QStringLiteral("totalFiles"), spec.files.size()},
        {QStringLiteral("installedBytes"), installedBytes(spec)},
        {QStringLiteral("files"), files},
        {QStringLiteral("isActive"), m_activeModelId == spec.id},
    };
}

bool AIModelCatalog::isDownloaded(const ModelSpec& spec) const
{
    for (const FileSpec& f : spec.files) {
        if (!QFileInfo::exists(f.path))
            return false;
    }
    return !spec.files.isEmpty();
}

qint64 AIModelCatalog::installedBytes(const ModelSpec& spec) const
{
    qint64 total = 0;
    for (const FileSpec& f : spec.files) {
        const QFileInfo info(f.path);
        if (info.exists())
            total += info.size();
    }
    return total;
}

void AIModelCatalog::refresh()
{
    SentryReporter::captureTelemetryEvent(QStringLiteral("ai.model_catalog.opened"),
        QJsonObject{{QStringLiteral("source_surface"), QStringLiteral("gui")}});
    emit modelsChanged();
}


void AIModelCatalog::captureModelTelemetry(const QString& eventName, const ModelSpec* spec,
                                           qint64 durationMs, const QString& failureCategory) const
{
    QJsonObject props;
    props["source_surface"] = QStringLiteral("gui");
    props["model_id"] = spec ? spec->id : m_activeModelId;
    props["capability"] = spec ? capabilityForFeature(spec->feature) : QStringLiteral("other");
    props["source"] = spec ? sourceForModelId(spec->id) : QStringLiteral("custom");
    props["build_available"] = spec ? spec->available : false;
    if (durationMs >= 0)
        props["duration_ms"] = durationMs;
    const qint64 bytes = spec ? installedBytes(*spec) : 0;
    props["byte_size_bucket"] = SentryReporter::sizeBucket(bytes);
    if (!failureCategory.isEmpty())
        props["failure_category"] = SentryReporter::sanitizedErrorCategory(failureCategory);
    SentryReporter::captureTelemetryEvent(eventName, props,
        eventName.endsWith(QStringLiteral(".failed")) ? QStringLiteral("error") : QStringLiteral("info"));
}

void AIModelCatalog::downloadModel(const QString& id)
{
    if (m_busy || ModelDownloader::instance()->isDownloading()) {
        setStatusMessage(tr("Another model download is already running."));
        return;
    }

    QList<ModelSpec> owner;
    const ModelSpec* spec = findSpec(id, &owner);
    if (!spec)
        return;
    if (!spec->available) {
        captureModelTelemetry(QStringLiteral("ai.feature_model_missing"), spec, -1, QStringLiteral("build_unavailable"));
        setStatusMessage(spec->buildRequirement);
        return;
    }
    if (isDownloaded(*spec)) {
        setStatusMessage(tr("%1 is already downloaded.").arg(spec->name));
        return;
    }

    m_pendingFiles.clear();
    for (const FileSpec& f : spec->files) {
        if (!QFileInfo::exists(f.path))
            m_pendingFiles << f;
    }
    if (m_pendingFiles.isEmpty())
        return;

    m_activeModelId = spec->id;
    m_activeModelName = spec->name;
    m_activeDownloadStartedMs = QDateTime::currentMSecsSinceEpoch();
    emit activeModelChanged();
    captureModelTelemetry(QStringLiteral("ai.model_download.started"), spec);
    SentryReporter::addBreadcrumb(QStringLiteral("ai.model_download"),
                                  QStringLiteral("started"));
    setBusy(true);
    setStatusMessage(tr("Downloading %1...").arg(spec->name));
    emit modelsChanged();
    startNextQueuedFile();
}

void AIModelCatalog::downloadAllModels()
{
    if (m_busy || ModelDownloader::instance()->isDownloading()) {
        setStatusMessage(tr("Another model download is already running."));
        return;
    }

    m_pendingFiles.clear();
    QSet<QString> queuedPaths;
    int unavailable = 0;
    const auto all = specs();
    for (const ModelSpec& spec : all) {
        if (!spec.available) {
            ++unavailable;
            continue;
        }
        for (const FileSpec& f : spec.files) {
            if (QFileInfo::exists(f.path) || queuedPaths.contains(f.path))
                continue;
            queuedPaths.insert(f.path);
            m_pendingFiles << f;
        }
    }

    if (m_pendingFiles.isEmpty()) {
        setStatusMessage(unavailable > 0
            ? tr("All available QtMeshEditor models are already downloaded.")
            : tr("All QtMeshEditor models are already downloaded."));
        emit modelsChanged();
        return;
    }

    m_activeModelId = QStringLiteral("__all__");
    m_activeDownloadStartedMs = QDateTime::currentMSecsSinceEpoch();
    m_activeModelName = unavailable > 0
        ? tr("all available QtMeshEditor models")
        : tr("all QtMeshEditor models");
    emit activeModelChanged();
    SentryReporter::captureTelemetryEvent(QStringLiteral("ai.model_download_all.started"),
        QJsonObject{{QStringLiteral("source_surface"), QStringLiteral("gui")},
                    {QStringLiteral("model_id"), QStringLiteral("__all__")},
                    {QStringLiteral("capability"), QStringLiteral("other")},
                    {QStringLiteral("source"), QStringLiteral("bundled")},
                    {QStringLiteral("build_available"), true}});
    SentryReporter::addBreadcrumb(QStringLiteral("ai.model_download"),
                                  QStringLiteral("started all"));
    setBusy(true);
    setStatusMessage(tr("Downloading %1...").arg(m_activeModelName));
    emit modelsChanged();
    startNextQueuedFile();
}

void AIModelCatalog::deleteModel(const QString& id)
{
    if (m_busy || ModelDownloader::instance()->isDownloading()) {
        setStatusMessage(tr("Cancel the current download before deleting models."));
        return;
    }

    QList<ModelSpec> owner;
    const ModelSpec* spec = findSpec(id, &owner);
    if (!spec)
        return;

    const qint64 deleteStartedMs = QDateTime::currentMSecsSinceEpoch();
    captureModelTelemetry(QStringLiteral("ai.model_delete.started"), spec);
    SentryReporter::addBreadcrumb(QStringLiteral("ai.model_delete"),
                                  QStringLiteral("started"));

    QSet<QString> sharedPaths;
    for (const ModelSpec& other : std::as_const(owner)) {
        if (other.id == spec->id || !isDownloaded(other))
            continue;
        for (const FileSpec& otherFile : other.files) {
            sharedPaths.insert(otherFile.path);
        }
    }

    bool removedAny = false;
    QStringList failedPaths;
    QStringList keptSharedPaths;
    auto removeExisting = [&](const QString& path) {
        if (!QFileInfo::exists(path))
            return;
        if (QFile::remove(path)) {
            removedAny = true;
        } else {
            failedPaths << path;
        }
    };

    for (const FileSpec& f : spec->files) {
        if (sharedPaths.contains(f.path)) {
            if (QFileInfo::exists(f.path) || QFileInfo::exists(f.path + QStringLiteral(".part")))
                keptSharedPaths << QFileInfo(f.path).fileName();
            continue;
        }
        removeExisting(f.path);
        removeExisting(f.path + QStringLiteral(".part"));
    }

    if (!failedPaths.isEmpty()) {
        setStatusMessage(tr("Could not delete %1 file(s) for %2.")
                             .arg(failedPaths.size())
                             .arg(spec->name));
        captureModelTelemetry(QStringLiteral("ai.model_delete.failed"), spec,
                              QDateTime::currentMSecsSinceEpoch() - deleteStartedMs,
                              QStringLiteral("delete_failed"));
        SentryReporter::addBreadcrumb(QStringLiteral("ai.model_delete"),
                                      QStringLiteral("failed"),
                                      QStringLiteral("error"));
    } else if (removedAny) {
        setStatusMessage(keptSharedPaths.isEmpty()
            ? tr("Deleted %1.").arg(spec->name)
            : tr("Deleted %1. Shared files used by other models were kept.").arg(spec->name));
        captureModelTelemetry(QStringLiteral("ai.model_delete.completed"), spec,
                              QDateTime::currentMSecsSinceEpoch() - deleteStartedMs);
        SentryReporter::addBreadcrumb(QStringLiteral("ai.model_delete"),
                                      QStringLiteral("completed"));
    } else if (!keptSharedPaths.isEmpty()) {
        setStatusMessage(tr("No exclusive downloaded files found for %1; shared files were kept.")
                             .arg(spec->name));
        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                                      tr("Kept shared files for %1").arg(spec->name));
    } else {
        setStatusMessage(tr("No downloaded files found for %1.").arg(spec->name));
    }
    emit modelsChanged();
}

void AIModelCatalog::deleteAllModels()
{
    if (m_busy || ModelDownloader::instance()->isDownloading()) {
        setStatusMessage(tr("Cancel the current download before deleting models."));
        return;
    }

    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                                  tr("Deleting all QtMeshEditor model files"));

    bool removedAny = false;
    QStringList failedPaths;
    QSet<QString> seenPaths;
    auto removeExisting = [&](const QString& path) {
        if (!QFileInfo::exists(path))
            return;
        if (QFile::remove(path)) {
            removedAny = true;
        } else {
            failedPaths << path;
        }
    };

    const auto all = specs();
    for (const ModelSpec& spec : all) {
        for (const FileSpec& f : spec.files) {
            if (seenPaths.contains(f.path))
                continue;
            seenPaths.insert(f.path);
            removeExisting(f.path);
            removeExisting(f.path + QStringLiteral(".part"));
        }
    }

    if (!failedPaths.isEmpty()) {
        setStatusMessage(tr("Could not delete %1 QtMeshEditor model file(s).").arg(failedPaths.size()));
        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                                      tr("Failed to delete %1 QtMeshEditor model file(s)")
                                          .arg(failedPaths.size()),
                                      QStringLiteral("error"));
    } else {
        setStatusMessage(removedAny
            ? tr("Deleted all downloaded QtMeshEditor model files.")
            : tr("No downloaded QtMeshEditor model files found."));
        if (removedAny) {
            SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                                          tr("Deleted all QtMeshEditor model files"));
        }
    }
    emit modelsChanged();
}

void AIModelCatalog::startNextQueuedFile()
{
    if (m_pendingFiles.isEmpty()) {
        const qint64 durationMs = m_activeDownloadStartedMs > 0
            ? QDateTime::currentMSecsSinceEpoch() - m_activeDownloadStartedMs : -1;
        if (m_activeModelId == QStringLiteral("__all__")) {
            SentryReporter::captureTelemetryEvent(QStringLiteral("ai.model_download_all.completed"),
                QJsonObject{{QStringLiteral("source_surface"), QStringLiteral("gui")},
                            {QStringLiteral("model_id"), QStringLiteral("__all__")},
                            {QStringLiteral("capability"), QStringLiteral("other")},
                            {QStringLiteral("duration_ms"), durationMs}});
        } else {
            QList<ModelSpec> owner;
            captureModelTelemetry(QStringLiteral("ai.model_download.completed"),
                                  findSpec(m_activeModelId, &owner), durationMs);
        }
        SentryReporter::addBreadcrumb(QStringLiteral("ai.model_download"),
                                      QStringLiteral("completed"));
        setStatusMessage(tr("%1 downloaded.").arg(m_activeModelName));
        clearActive();
        setBusy(false);
        emit modelsChanged();
        return;
    }

    const FileSpec f = m_pendingFiles.first();
    if (f.url.isEmpty()) {
        m_pendingFiles.clear();
        QList<ModelSpec> owner;
        captureModelTelemetry(QStringLiteral("ai.model_download.failed"),
                              findSpec(m_activeModelId, &owner),
                              m_activeDownloadStartedMs > 0
                                  ? QDateTime::currentMSecsSinceEpoch() - m_activeDownloadStartedMs : -1,
                              QStringLiteral("missing_url"));
        SentryReporter::addBreadcrumb(QStringLiteral("ai.model_download"),
                                      QStringLiteral("missing url"),
                                      QStringLiteral("error"));
        setStatusMessage(tr("No download URL configured for %1.").arg(m_activeModelName));
        clearActive();
        setBusy(false);
        emit modelsChanged();
        return;
    }

    QDir().mkpath(QFileInfo(f.path).absolutePath());
    ModelDownloader::instance()->startDownload(f.url, f.path, f.label);
}

void AIModelCatalog::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void AIModelCatalog::setStatusMessage(const QString& message)
{
    if (m_statusMessage == message)
        return;
    m_statusMessage = message;
    emit statusMessageChanged();
}

void AIModelCatalog::clearActive()
{
    if (m_activeModelId.isEmpty() && m_activeModelName.isEmpty())
        return;
    m_activeModelId.clear();
    m_activeModelName.clear();
    emit activeModelChanged();
}

const AIModelCatalog::ModelSpec* AIModelCatalog::findSpec(const QString& id,
                                                          QList<ModelSpec>* owner) const
{
    if (!owner)
        return nullptr;
    *owner = specs();
    for (const ModelSpec& spec : std::as_const(*owner)) {
        if (spec.id == id)
            return &spec;
    }
    return nullptr;
}
