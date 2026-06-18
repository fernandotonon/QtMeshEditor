#include "AIAssistManager.h"

#include "NormalMapGenerator.h"
#include "ModelDownloader.h"
#include "SentryReporter.h"

#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSettings>

AIAssistManager* AIAssistManager::s_instance = nullptr;

namespace {
// Filename of the bundled/downloaded PBR UNet. The production model is a
// permissively-licensed UNet (DeepBump is GPL and out); the actual host URL is
// resolved separately (settings/env override below). Tests inject their own.
constexpr const char* kPbrModelFile = "pbr_unet.onnx";
constexpr const char* kModelUrlSettingsKey = "ai/pbrModelUrl";
// TODO(#404): point at a permissively-licensed ONNX UNet host. Empty until then;
// ensureModel() no-ops when empty and synthesize fails gracefully.
constexpr const char* kDefaultPbrModelUrl = "";
} // namespace

AIAssistManager::AIAssistManager(QObject* parent) : QObject(parent)
{
    if (auto* dl = ModelDownloader::instance()) {
        connect(dl, &ModelDownloader::downloadProgressUpdated, this,
            [this](const QString& name, qint64 r, qint64 t) {
                if (name == QLatin1String("PBR UNet"))
                    emit modelDownloadProgress(r, t);
            });
        connect(dl, &ModelDownloader::downloadCompleted, this,
            [this](const QString& name, const QString&) {
                if (name == QLatin1String("PBR UNet"))
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

QString AIAssistManager::modelPath() const
{
    const QString dataPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return QDir(dataPath).filePath(QStringLiteral("ai_models/pbr/") + kPbrModelFile);
}

bool AIAssistManager::isModelReady() const
{
    return QFileInfo::exists(modelPath());
}

QString AIAssistManager::defaultModelUrl() const
{
    QSettings s;
    const QString fromSettings =
        s.value(QString::fromLatin1(kModelUrlSettingsKey)).toString();
    if (!fromSettings.isEmpty())
        return fromSettings;
    const QByteArray env = qgetenv("QTMESH_PBR_MODEL_URL");
    if (!env.isEmpty())
        return QString::fromUtf8(env);
    return QString::fromLatin1(kDefaultPbrModelUrl);
}

void AIAssistManager::ensureModel()
{
    if (isModelReady())
        return;
    const QString url = defaultModelUrl();
    if (url.isEmpty())
        return; // no configured source — synthesize() will fail gracefully
    const QString dest = modelPath();
    QDir().mkpath(QFileInfo(dest).absolutePath());
    if (auto* dl = ModelDownloader::instance())
        dl->startDownload(url, dest, QStringLiteral("PBR UNet"));
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

    PbrMapSynth::Result res = PbrMapSynth::synthesize(albedo, modelPath(), opts);
    if (!res.ok) {
        out.error = res.error;
        emit synthesisError(out.error);
        return out;
    }

    // If the model produced height but no normal, derive the normal via the
    // existing Sobel path (NormalMapGenerator) so all requested maps exist.
    if (opts.generateNormal && res.normal.isNull() && !res.height.isNull()) {
        // Persist height first so NormalMapGenerator can read it from disk.
        res.height.save(heightOut, "PNG");
        NormalMapGenerator::GenSpec spec;
        spec.sourcePath = heightOut;
        spec.invertG = opts.invertG;
        spec.strength = 2.0f * opts.normalStrength;
        const NormalMapGenerator::GenResult ng = NormalMapGenerator::generate(spec);
        if (ng.ok)
            res.normal = ng.image;
    }

    if (opts.generateNormal && !res.normal.isNull()
        && res.normal.save(normalOut, "PNG"))
        out.normalPath = normalOut;
    if (opts.generateRoughness && !res.roughness.isNull()
        && res.roughness.save(roughOut, "PNG"))
        out.roughnessPath = roughOut;
    if (opts.generateHeight && !res.height.isNull()
        && (QFileInfo::exists(heightOut) || res.height.save(heightOut, "PNG")))
        out.heightPath = heightOut;

    out.ok = (!opts.generateNormal    || !out.normalPath.isEmpty())
          && (!opts.generateRoughness || !out.roughnessPath.isEmpty())
          && (!opts.generateHeight    || !out.heightPath.isEmpty());
    if (!out.ok && out.error.isEmpty())
        out.error = QStringLiteral("one or more requested maps could not be written");

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
