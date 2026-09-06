#include "AIAssistManager.h"

#include "GamificationManager.h"
#include "NormalMapGenerator.h"
#include "TextureUpscaler.h"
#include "ModelDownloader.h"
#include "SentryReporter.h"
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSettings>
#include <QEventLoop>
#include <QTimer>
#include "AppStorage.h"

AIAssistManager* AIAssistManager::s_instance = nullptr;

namespace {
// PBRify_Remix (CC0-1.0) per-map SPAN models, exported to ONNX via
// scripts/export-pbrify-onnx.py. Each map is a separate model file/URL.
// The host base URL is configurable (QSettings ai/pbrModelBaseUrl or
// QTMESH_PBR_MODEL_BASE_URL env); empty default until the .onnx files are
// hosted, in which case ensureModel() no-ops and synthesis fails gracefully.
constexpr const char* kModelBaseUrlSettingsKey = "ai/pbrModelBaseUrl";
// CC0 PBRify ONNX models hosted on Hugging Face (re-export of
// Kim2091/PBRify_Remix; see scripts/export-pbrify-onnx.py). Override via the
// QSettings key / QTMESH_PBR_MODEL_BASE_URL env for self-hosting or testing.
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/";

const char* mapDownloadLabel(AIAssistManager::Map m) {
    switch (m) {
        case AIAssistManager::Map::Normal:    return "PBR Normal";
        case AIAssistManager::Map::Roughness: return "PBR Roughness";
        case AIAssistManager::Map::Height:    return "PBR Height";
        case AIAssistManager::Map::UpscaleX2: return "Upscale 2x";
        case AIAssistManager::Map::UpscaleX4: return "Upscale 4x";
    }
    return "PBR";
}
} // namespace

QString AIAssistManager::mapModelFile(Map map)
{
    switch (map) {
        case Map::Normal:    return QStringLiteral("1x-PBRify_NormalV3.onnx");
        case Map::Roughness: return QStringLiteral("1x-PBRify_RoughnessV2.onnx");
        case Map::Height:    return QStringLiteral("1x-PBRify_Height.onnx");
        case Map::UpscaleX2: return QStringLiteral("RealESRGAN_x2plus.onnx");
        case Map::UpscaleX4: return QStringLiteral("RealESRGAN_x4plus.onnx");
    }
    return {};
}

AIAssistManager::AIAssistManager(QObject* parent) : QObject(parent)
{
    if (auto* dl = ModelDownloader::instance()) {
        connect(dl, &ModelDownloader::downloadProgressUpdated, this,
            [this](const QString& name, qint64 r, qint64 t) {
                if (name.startsWith(QLatin1String("PBR ")))
                    emit modelDownloadProgress(r, t);
            });
        connect(dl, &ModelDownloader::downloadCompleted, this,
            [this](const QString& name, const QString&) {
                if (name.startsWith(QLatin1String("PBR ")))
                    emit modelReadyChanged();
            });
    }
}

AIAssistManager* AIAssistManager::instance()
{
    if (!s_instance)
        s_instance = new AIAssistManager();
    return s_instance;
}

AIAssistManager* AIAssistManager::qmlInstance(QQmlEngine*, QJSEngine*)
{
    return instance();
}

bool AIAssistManager::isAvailable() const
{
#ifdef ENABLE_ONNX
    return true;
#else
    return false;
#endif
}

QString AIAssistManager::modelPath(Map map) const
{
    return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("pbr/") + mapModelFile(map));
}

bool AIAssistManager::isModelReady() const
{
    // Ready when every per-map model is present.
    return QFileInfo::exists(modelPath(Map::Normal))
        && QFileInfo::exists(modelPath(Map::Roughness))
        && QFileInfo::exists(modelPath(Map::Height));
}

QString AIAssistManager::defaultModelUrl(Map map) const
{
    QSettings s;
    QString base = s.value(QString::fromLatin1(kModelBaseUrlSettingsKey)).toString();
    if (base.isEmpty()) {
        const QByteArray env = qgetenv("QTMESH_PBR_MODEL_BASE_URL");
        base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                             : QString::fromUtf8(env);
    }
    if (base.isEmpty())
        return {};
    if (!base.endsWith('/')) base += '/';
    return base + mapModelFile(map);
}

bool AIAssistManager::ensureModelBlocking(Map map)
{
    const QString dest = modelPath(map);
    if (QFileInfo::exists(dest))
        return true;
    // Offline / test guard: never hit the network when this is set (unit tests
    // set it so the synchronous synthesize path can't hang on a download).
    if (!qEnvironmentVariableIsEmpty("QTMESH_PBR_NO_DOWNLOAD"))
        return false;
    const QString url = defaultModelUrl(map);
    if (url.isEmpty())
        return false;
    auto* dl = ModelDownloader::instance();
    if (!dl)
        return false;
    QDir().mkpath(QFileInfo(dest).absolutePath());

    // Block until this specific model finishes (or errors). ModelDownloader is
    // async; drive it with a local event loop. The download label is per-map so
    // we only react to our own.
    const QString label = QString::fromLatin1(mapDownloadLabel(map));
    QEventLoop loop;
    bool ok = false;
    auto onDone = connect(dl, &ModelDownloader::downloadCompleted, &loop,
        [&](const QString& name, const QString&) {
            if (name == label) { ok = true; loop.quit(); }
        });
    auto onErr = connect(dl, &ModelDownloader::downloadError, &loop,
        [&](const QString& name, const QString&) {
            if (name == label) { ok = false; loop.quit(); }
        });
    // Hard timeout so a hung/stalled connection (DNS stall, unresponsive host)
    // can't block the synchronous synthesize call forever — the map then
    // reports the graceful "model not available" error / roughness falls back.
    bool timedOut = false;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, [&]() { timedOut = true; loop.quit(); });
    timeout.start(120000);  // 120s — generous for a ~1.6 MB model on slow links
    dl->startDownload(url, dest, label);
    loop.exec();
    disconnect(onDone);
    disconnect(onErr);
    if (timedOut && dl)
        dl->cancelDownload();
    return ok && !timedOut && QFileInfo::exists(dest);
}

void AIAssistManager::ensureModel()
{
    auto* dl = ModelDownloader::instance();
    if (!dl) return;
    for (Map m : {Map::Normal, Map::Roughness, Map::Height}) {
        if (QFileInfo::exists(modelPath(m))) continue;
        const QString url = defaultModelUrl(m);
        if (url.isEmpty()) continue; // no source — synthesize() fails gracefully
        const QString dest = modelPath(m);
        QDir().mkpath(QFileInfo(dest).absolutePath());
        dl->startDownload(url, dest, QString::fromLatin1(mapDownloadLabel(m)));
    }
}

PbrMapSynthResult AIAssistManager::synthesizePbrMaps(const QString& albedoPath,
                                                     const PbrMapSynth::Options& opts)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.pbr_synth"),
        QStringLiteral("synthesize from %1 (normal=%2 rough=%3 height=%4)")
            .arg(QFileInfo(albedoPath).fileName())
            .arg(opts.generateNormal).arg(opts.generateRoughness)
            .arg(opts.generateHeight));
    emit synthesisStarted();

    PbrMapSynthResult out;
    const QFileInfo fi(albedoPath);
    if (!fi.exists()) {
        out.error = QStringLiteral("albedo not found: %1").arg(albedoPath);
        emit synthesisError(out.error);
        return out;
    }

    const QString dir = fi.absolutePath();
    const QString stem = fi.completeBaseName();
    const QString normalOut = QDir(dir).filePath(stem + QStringLiteral("_normal.png"));
    const QString roughOut  = QDir(dir).filePath(stem + QStringLiteral("_roughness.png"));
    const QString heightOut = QDir(dir).filePath(stem + QStringLiteral("_height.png"));

    // Cache hit: every requested map already on disk → reuse.
    auto wantHave = [&](bool want, const QString& p) {
        return !want || QFileInfo::exists(p);
    };
    if (!opts.overwriteCache
        && wantHave(opts.generateNormal, normalOut)
        && wantHave(opts.generateRoughness, roughOut)
        && wantHave(opts.generateHeight, heightOut)) {
        out.ok = true;
        out.fromCache = true;
        if (opts.generateNormal)    out.normalPath = normalOut;
        if (opts.generateRoughness) out.roughnessPath = roughOut;
        if (opts.generateHeight)    out.heightPath = heightOut;
        emit synthesisCompleted(out.toVariantMap());
        return out;
    }

    const QImage albedo(albedoPath);
    if (albedo.isNull()) {
        out.error = QStringLiteral("could not load albedo image: %1").arg(albedoPath);
        emit synthesisError(out.error);
        return out;
    }

    // Run each requested map through its own per-map model. A model-dependent
    // map (normal/height) that fails sets the error; roughness gracefully
    // falls back to the offline luminance heuristic when its model is absent.
#ifdef ENABLE_ONNX
    if (opts.generateNormal) {
        ensureModelBlocking(Map::Normal);
        int w = 0, h = 0; QString err;
        const std::vector<float> t =
            PbrMapSynth::runTiledModel(albedo, modelPath(Map::Normal), opts, &w, &h, &err);
        if (t.empty()) { out.error = err; emit synthesisError(out.error); return out; }
        const QImage n = PbrMapSynth::decodeNormal(t, w, h, opts.normalStrength, opts.invertG);
        if (n.save(normalOut, "PNG")) out.normalPath = normalOut;
    }
    if (opts.generateHeight) {
        ensureModelBlocking(Map::Height);
        int w = 0, h = 0; QString err;
        const std::vector<float> t =
            PbrMapSynth::runTiledModel(albedo, modelPath(Map::Height), opts, &w, &h, &err);
        if (t.empty()) { out.error = err; emit synthesisError(out.error); return out; }
        const QImage hImg = PbrMapSynth::decodeGrayscaleFromRgb(t, w, h);
        if (hImg.save(heightOut, "PNG")) out.heightPath = heightOut;
    }
    if (opts.generateRoughness) {
        ensureModelBlocking(Map::Roughness);  // falls back to heuristic if it can't download
        QImage rough;
        int w = 0, h = 0; QString err;
        const std::vector<float> t =
            PbrMapSynth::runTiledModel(albedo, modelPath(Map::Roughness), opts, &w, &h, &err);
        if (!t.empty())
            rough = PbrMapSynth::decodeGrayscaleFromRgb(t, w, h);
        else  // model missing/offline → luminance heuristic (always available)
            rough = PbrMapSynth::roughnessFromAlbedo(albedo, opts.roughnessBase,
                                                     opts.roughnessContrast);
        if (!rough.isNull() && rough.save(roughOut, "PNG"))
            out.roughnessPath = roughOut;
    }
#else
    if (opts.generateNormal || opts.generateHeight) {
        out.error = QStringLiteral("PBR map synthesis was not built into this binary "
            "(rebuild with -DENABLE_ONNX=ON).");
        emit synthesisError(out.error);
        return out;
    }
    if (opts.generateRoughness) {
        const QImage rough = PbrMapSynth::roughnessFromAlbedo(
            albedo, opts.roughnessBase, opts.roughnessContrast);
        if (!rough.isNull() && rough.save(roughOut, "PNG"))
            out.roughnessPath = roughOut;
    }
#endif

    out.ok = (!opts.generateNormal    || !out.normalPath.isEmpty())
          && (!opts.generateRoughness || !out.roughnessPath.isEmpty())
          && (!opts.generateHeight    || !out.heightPath.isEmpty());
    if (!out.ok && out.error.isEmpty())
        out.error = QStringLiteral("one or more requested maps could not be written");

    if (out.ok) {
        const int mapsGenerated = (out.normalPath.isEmpty() ? 0 : 1)
                                + (out.roughnessPath.isEmpty() ? 0 : 1)
                                + (out.heightPath.isEmpty() ? 0 : 1);
        GamificationManager::noteOperation(
            QStringLiteral("pbr_synth"),
            {{QStringLiteral("maps_generated"), mapsGenerated}});
    }

    if (out.ok) emit synthesisCompleted(out.toVariantMap());
    else        emit synthesisError(out.error);
    return out;
}

QVariantMap AIAssistManager::synthesizePbrMapsQml(const QString& albedoPath,
                                                  const QVariantMap& o)
{
    PbrMapSynth::Options opts;
    if (o.contains("generateNormal"))    opts.generateNormal    = o["generateNormal"].toBool();
    if (o.contains("generateRoughness")) opts.generateRoughness = o["generateRoughness"].toBool();
    if (o.contains("generateHeight"))    opts.generateHeight    = o["generateHeight"].toBool();
    if (o.contains("tileSize"))          opts.tileSize          = o["tileSize"].toInt();
    if (o.contains("normalStrength"))    opts.normalStrength    = o["normalStrength"].toFloat();
    if (o.contains("invertG"))           opts.invertG           = o["invertG"].toBool();
    if (o.contains("overwriteCache"))    opts.overwriteCache    = o["overwriteCache"].toBool();
    return synthesizePbrMaps(albedoPath, opts).toVariantMap();
}

QString AIAssistManager::ensureUpscaleModel(int scale)
{
    const Map m = (scale == 2) ? Map::UpscaleX2 : Map::UpscaleX4;
    ensureModelBlocking(m);   // event-loop driven; call on the GUI thread
    const QString p = modelPath(m);
    return QFileInfo::exists(p) ? p : QString();
}

QString AIAssistManager::upscaleTexture(const QString& srcPath, int scale, bool overwrite)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.upscale"),
        QStringLiteral("upscale %1 x%2").arg(QFileInfo(srcPath).fileName()).arg(scale));
    GamificationManager::noteFeature(QStringLiteral("pbr_synth"));
    emit upscaleStarted();

    auto failUp = [&](const QString& msg) -> QString {
        emit upscaleError(msg);
        return {};
    };
    const QFileInfo fi(srcPath);
    if (!fi.exists())
        return failUp(tr("Texture not found: %1").arg(srcPath));
    if (scale != 2 && scale != 4)
        return failUp(tr("Upscale factor must be 2 or 4."));

    // Scale-specific cache name so a cached 2× result is never returned for a
    // 4× request (and vice versa) on the overwrite=false path.
    const QString outPath = QDir(fi.absolutePath())
        .filePath(fi.completeBaseName()
                  + QStringLiteral("_upscaled_x%1.png").arg(scale));
    if (!overwrite && QFileInfo::exists(outPath)) {  // cache: skip re-upscale
        emit upscaleCompleted(outPath);
        return outPath;
    }

#ifndef ENABLE_ONNX
    return failUp(tr("Texture upscaling is not enabled. Rebuild with -DENABLE_ONNX=ON."));
#else
    const QImage src(srcPath);
    if (src.isNull())
        return failUp(tr("Could not load image: %1").arg(srcPath));

    const Map m = (scale == 2) ? Map::UpscaleX2 : Map::UpscaleX4;
    ensureModelBlocking(m);
    const TextureUpscaler::Result res =
        TextureUpscaler::upscale(src, modelPath(m), {});
    if (!res.ok)
        return failUp(res.error.isEmpty() ? tr("Upscale failed.") : res.error);
    if (!res.image.save(outPath, "PNG"))
        return failUp(tr("Could not write the upscaled image."));
    emit upscaleCompleted(outPath);
    return outPath;
#endif
}

// #764 image-to-3D: no AIAssistManager entry point — the GUI runs the pipeline
// on a worker thread via MeshGenController, and the CLI/MCP surfaces drive
// MeshGenPredictor + MeshGenBuilder directly (see CLIPipeline::cmdGenerate3d
// and MCPServer::toolGenerateMeshFromImage).
