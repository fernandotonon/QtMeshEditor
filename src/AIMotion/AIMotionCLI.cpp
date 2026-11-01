#include "AIMotionCLI.h"

#include "KimodoNative.h"
#include "MotionInterchange.h"
#include "MotionSceneApply.h"
#include "QtMeshT2M.h"
#include "SomaRetarget.h"
#include "SomaSkeleton.h"
#include "../AnimationMerger.h"
#include "../CLIPipeline.h"
#include "../Manager.h"
#include "../MeshImporterExporter.h"
#include "../SentryReporter.h"

#include <OgreEntity.h>
#include <OgreSceneNode.h>

#include <QFileInfo>

#include <cmath>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace AIMotion {

namespace {

void note(const QString& s) { CLIPipeline::writeCliError(s + QLatin1Char('\n')); }

int fail(const QString& s, int code = 1)
{
    note(QStringLiteral("Error: ") + s);
    return code;
}


} // namespace

bool parseSegment(const QString& spec, MotionSegment* out, QString* error)
{
    const int at = spec.lastIndexOf(QLatin1Char('@'));
    bool ok = false;
    const double seconds = at > 0 ? spec.mid(at + 1).trimmed().toDouble(&ok) : 0.0;
    if (at <= 0 || !ok || !(seconds > 0.0) || seconds > 600.0) {
        if (error) *error = QStringLiteral("segment '%1' must be TEXT@SECONDS with 0 < SECONDS <= 600").arg(spec);
        return false;
    }
    out->text = spec.left(at).trimmed();
    out->duration = seconds;
    if (out->text.isEmpty()) {
        if (error) *error = QStringLiteral("segment '%1' has no text").arg(spec);
        return false;
    }
    return true;
}

namespace {

// Request -> generation options. 0 = ok, else the exit code (the message is already printed).
int buildOptions(const CliRequest& req, KimodoPredictor::Options* o)
{
    if (!req.segments.isEmpty()) {
        for (const QString& s : req.segments) {
            MotionSegment seg;
            QString e;
            if (!parseSegment(s, &seg, &e)) return fail(e, 2);
            o->segments.push_back(seg);
        }
    } else {
        if (req.prompt.trimmed().isEmpty()) return fail(QStringLiteral("--generate needs a prompt"), 2);
        o->segments.push_back({req.prompt, req.duration > 0.0 ? req.duration : 5.0});
    }
    o->numSamples = req.variations;
    o->hasSeed = req.hasSeed;
    o->seed = req.seed;
    o->diffusionSteps = req.diffusionSteps;
    o->rootMotion = req.rootMotion;
    o->postprocess = req.postprocess;
    o->constraintsPath = req.constraintsPath;
    o->textEncoderUrl = req.textEncoderUrl;
    o->textEncoderQuant = req.textEncoderQuant;
    o->runtime = req.runtime;
    if (!o->runtime.isEmpty() && !KimodoPredictor::runtimeModes().contains(o->runtime))
        return fail(QStringLiteral("--runtime must be one of: %1").arg(KimodoPredictor::runtimeModes().join(QStringLiteral(", "))), 2);
    if (QString e; !KimodoPredictor::validateOptions(*o, &e)) return fail(e, 2);
    return 0;
}

// Generate (downloading the native models first when --download asks for it). 0 = ok, else the exit code.
int generateFiles(const CliRequest& req, QStringList* files, QJsonObject* manifest)
{
    KimodoPredictor::Options o;
    if (const int code = buildOptions(req, &o)) return code;
    if (req.download && !KimodoNative::modelsInstalled()) {
        note(QStringLiteral("(kimodo: downloading the native models, ~%1 GB)").arg(double(KimodoNative::missingBytes()) / 1e9, 0, 'f', 1));
        if (QString e; !KimodoNative::downloadModels(&e)) return fail(QStringLiteral("download failed: %1").arg(e));
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.kimodo"),
                                  QStringLiteral("CLI generate segments=%1 variations=%2").arg(o.segments.size()).arg(o.numSamples));
    QString lastStage;
    const auto r = KimodoPredictor::predict(o, [&lastStage](const QString& stage, int, int) {
        if (stage != lastStage) {
            note(QStringLiteral("(kimodo: %1)").arg(stage));
            lastStage = stage;
        }
        return true;
    });
    if (!r.ok) return fail(r.error);
    *files = r.files;
    *manifest = r.manifest;
    return 0;
}

Ogre::Entity* firstSkinnedEntity()
{
    for (auto* e : Manager::getSingleton()->getEntities())
        if (e && e->getMovableType() == "Entity" && static_cast<Ogre::Entity*>(e)->hasSkeleton())
            return static_cast<Ogre::Entity*>(e);
    return nullptr;
}

// #854 arm space, as the other generate backends do. A rig with no keyed arm roles cannot take it: say so.
void applyArmSpace(Ogre::Entity* entity, const QStringList& clips, float degrees)
{
    if (std::abs(degrees) <= 1e-4f) return;
    for (const QString& clip : clips) {
        if (AnimationMerger::adjustArmSpace(entity->getMesh()->getSkeleton().get(), clip.toStdString(), degrees))
            note(QStringLiteral("(arm-space: %1 deg on %2)").arg(degrees).arg(clip));
        else
            note(QStringLiteral("Warning: --arm-space skipped on %1 (no keyed shoulder/collar bones)").arg(clip));
    }
}

void writeReport(const CliRequest& req, const QJsonArray& clips, const QString& output, const QStringList& files,
                 const QJsonObject& manifest)
{
    if (req.json) {
        QJsonObject root{{QStringLiteral("backend"), QStringLiteral("kimodo")},
                         {QStringLiteral("mapVersion"), QString::fromLatin1(kSomaMapVersion)},
                         {QStringLiteral("clips"), clips},
                         {QStringLiteral("output"), QFileInfo(output).absoluteFilePath()},
                         {QStringLiteral("source"), manifest.isEmpty() ? QJsonValue(QJsonArray::fromStringList(files)) : QJsonValue(manifest)}};
        CLIPipeline::writeOutput(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)) + QLatin1Char('\n'));
    } else {
        CLIPipeline::writeOutput(QStringLiteral("Kimodo motion: %1 clip(s) on %2 -> %3\n")
                                     .arg(clips.size()).arg(QFileInfo(req.rigPath).fileName(), QFileInfo(output).fileName()));
    }
}

} // namespace

QString kimodoOnlyOption(const CliRequest& req)
{
    const std::pair<bool, const char*> flags[] = {
        {!req.segments.isEmpty(), "--segment"},
        {req.variations != 1, "--variations"},
        {req.hasSeed, "--seed"},
        {req.diffusionSteps != 0, "--diffusion-steps"},
        {!req.rootMotion, "--no-root-motion"},
        {!req.postprocess, "--no-postprocess"},
        {!req.constraintsPath.isEmpty(), "--constraints"},
        {!req.textEncoderUrl.isEmpty(), "--text-encoder-url"},
        {!req.textEncoderQuant.isEmpty(), "--text-encoder-quant"},
        {!req.runtime.isEmpty(), "--runtime"},
        {req.download, "--download"},
        {!req.name.isEmpty(), "--name"},
    };
    for (const auto& [set, flag] : flags)
        if (set) return QString::fromLatin1(flag);
    return {};
}

int runCli(const CliRequest& req)
{
    if (!QFileInfo::exists(req.rigPath)) return fail(QStringLiteral("File not found: %1").arg(req.rigPath));
    if (req.variations < 1 || req.variations > 16) return fail(QStringLiteral("--variations must be 1..16"), 2);
    // A non-finite angle would write NaN rotations, and the default output overwrites the input rig.
    if (!std::isfinite(req.armSpaceDeg)) return fail(QStringLiteral("--arm-space must be a finite number of degrees"), 2);

    QStringList files = req.applyMotion;
    QJsonObject manifest;
    if (files.isEmpty())
        if (const int code = generateFiles(req, &files, &manifest)) return code;

    std::vector<Motion> motions;
    if (QString e; !loadMotionFiles(files, &motions, &e)) return fail(e);

    if (!CLIPipeline::initOgreHeadless()) return 1;
    MeshImporterExporter::importer({QFileInfo(req.rigPath).absoluteFilePath()});
    Ogre::Entity* entity = firstSkinnedEntity();
    if (!entity) return fail(QStringLiteral("%1 has no skinned mesh: Kimodo motion needs a rigged humanoid").arg(req.rigPath));
    SceneApplyOptions so;
    so.baseName = req.name;
    so.rootMotion = req.rootMotion;
    so.footPin = req.footPin;
    so.undoable = false;   // one-shot process
    const SceneApplyResult applied = applyMotionsToEntity(entity, motions, so);
    if (!applied.ok) return fail(applied.error);
    applyArmSpace(entity, applied.clips, req.armSpaceDeg);

    QJsonArray clips;
    for (const ApplyOutcome& out : applied.outcomes) {
        note(QStringLiteral("(%1: %2 bones mapped, %3 fingers; %4 rig bones undriven)")
                 .arg(QString::fromStdString(out.clip)).arg(out.mappedBones).arg(out.fingerBones).arg(out.unmappedTarget.size()));
        clips.append(outcomeJson(out));
    }

    const QString output = req.outputPath.isEmpty() ? req.rigPath : req.outputPath;
    if (MeshImporterExporter::exporter(entity->getParentSceneNode(), QFileInfo(output).absoluteFilePath(),
                                       CLIPipeline::formatForExtension(output)) != 0)
        return fail(QStringLiteral("export failed"));
    writeReport(req, clips, output, files, manifest);
    return 0;
}

int runT2MCli(const CliRequest& req)
{
    if (!QFileInfo::exists(req.rigPath)) return fail(QStringLiteral("File not found: %1").arg(req.rigPath));
    if (!std::isfinite(req.armSpaceDeg)) return fail(QStringLiteral("--arm-space must be a finite number of degrees"), 2);
    const std::pair<bool, const char*> kimodoOnly[] = {
        {!req.segments.isEmpty(), "--segment"},
        {req.diffusionSteps != 0, "--diffusion-steps"},
        {!req.postprocess, "--no-postprocess"},
        {!req.constraintsPath.isEmpty(), "--constraints"},
        {!req.textEncoderUrl.isEmpty(), "--text-encoder-url"},
        {!req.textEncoderQuant.isEmpty(), "--text-encoder-quant"},
        {!req.runtime.isEmpty(), "--runtime"},
        {!req.applyMotion.isEmpty(), "--apply-motion"},
    };
    for (const auto& [set, flag] : kimodoOnly)
        if (set) return fail(QStringLiteral("%1 is a Kimodo option (QtMesh T2M generates one motion per prompt)").arg(QLatin1String(flag)), 2);

    QtMeshT2M::Options o;
    o.prompt = req.prompt;
    o.duration = req.duration;   // the caller defaults an ABSENT --duration; an invalid one must reach validateOptions
    o.variations = req.variations;
    o.hasSeed = req.hasSeed;
    o.seed = req.seed;
    if (QString e; !QtMeshT2M::validateOptions(o, &e)) return fail(e, 2);
    if (!QtMeshT2M::buildSupported()) return fail(QtMeshT2M::description());
    if (!QtMeshT2M::modelsInstalled()) {
        if (!req.download)
            return fail(QStringLiteral("%1: add --download to fetch it").arg(QtMeshT2M::description()));
        note(QStringLiteral("(qtmesh_t2m: downloading the model, ~%1 MB)").arg(double(QtMeshT2M::missingBytes()) / 1e6, 0, 'f', 0));
        if (QString e; !QtMeshT2M::downloadModels(&e)) return fail(QStringLiteral("download failed: %1").arg(e));
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.qtmesh_t2m"), QStringLiteral("CLI generate variations=%1").arg(o.variations));
    const QtMeshT2M::Result gen = QtMeshT2M::generate(o);
    if (!gen.ok) return fail(gen.error);
    if (!gen.note.isEmpty()) note(QStringLiteral("(qtmesh_t2m: %1)").arg(gen.note));

    if (!CLIPipeline::initOgreHeadless()) return 1;
    MeshImporterExporter::importer({QFileInfo(req.rigPath).absoluteFilePath()});
    Ogre::Entity* entity = firstSkinnedEntity();
    if (!entity) return fail(QStringLiteral("%1 has no skinned mesh: generated motion needs a rigged humanoid").arg(req.rigPath));
    std::vector<CanonicalMotion> motions;
    for (const auto& c : gen.clips) motions.push_back({c.quats, gen.fps, gen.restWorld, gen.restDir});
    SceneApplyOptions so;
    so.baseName = req.name.isEmpty() ? QStringLiteral("generated_qtmesh_t2m") : req.name;
    so.rootMotion = req.rootMotion;
    so.footPin = req.footPin;
    so.smoothFps = req.smoothFps;
    so.undoable = false;
    const SceneApplyResult applied = applyCanonicalMotionsToEntity(entity, motions, so);
    if (!applied.ok) return fail(applied.error);
    applyArmSpace(entity, applied.clips, req.armSpaceDeg);

    const QString output = req.outputPath.isEmpty() ? req.rigPath : req.outputPath;
    if (MeshImporterExporter::exporter(entity->getParentSceneNode(), QFileInfo(output).absoluteFilePath(),
                                       CLIPipeline::formatForExtension(output)) != 0)
        return fail(QStringLiteral("export failed"));
    if (req.json) {
        QJsonArray clips;
        for (size_t i = 0; i < gen.clips.size() && i < size_t(applied.clips.size()); ++i)
            clips.append(QJsonObject{{QStringLiteral("name"), applied.clips[int(i)]},
                                     {QStringLiteral("frames"), int(gen.clips[i].quats.size())},
                                     {QStringLiteral("trimmedLeadInFrames"), gen.clips[i].trimmedFrames}});
        const QJsonObject root{{QStringLiteral("backend"), QStringLiteral("qtmesh_t2m")},
                               {QStringLiteral("experimental"), true},
                               {QStringLiteral("clips"), clips},
                               {QStringLiteral("encodeSeconds"), gen.encodeSeconds},
                               {QStringLiteral("sampleSeconds"), gen.sampleSeconds},
                               {QStringLiteral("note"), gen.note},
                               {QStringLiteral("output"), QFileInfo(output).absoluteFilePath()}};
        CLIPipeline::writeOutput(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)) + QLatin1Char('\n'));
    } else {
        CLIPipeline::writeOutput(QStringLiteral("QtMesh T2M motion (experimental): %1 clip(s) on %2 -> %3 (text %4 s, motion %5 s)\n")
                                     .arg(applied.clips.size()).arg(QFileInfo(req.rigPath).fileName(), QFileInfo(output).fileName())
                                     .arg(gen.encodeSeconds, 0, 'f', 2).arg(gen.sampleSeconds, 0, 'f', 2));
    }
    return 0;
}

} // namespace AIMotion
