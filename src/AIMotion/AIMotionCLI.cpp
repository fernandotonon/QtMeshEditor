#include "AIMotionCLI.h"

#include "KimodoNative.h"
#include "MotionInterchange.h"
#include "MotionSceneApply.h"
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

int runCli(const CliRequest& req)
{
    if (!QFileInfo::exists(req.rigPath)) return fail(QStringLiteral("File not found: %1").arg(req.rigPath));
    if (req.variations < 1 || req.variations > 16) return fail(QStringLiteral("--variations must be 1..16"), 2);

    QStringList files = req.applyMotion;
    QJsonObject manifest;
    if (files.isEmpty()) {
        KimodoPredictor::Options o;
        if (!req.segments.isEmpty()) {
            for (const QString& s : req.segments) {
                MotionSegment seg;
                QString e;
                if (!parseSegment(s, &seg, &e)) return fail(e, 2);
                o.segments.push_back(seg);
            }
        } else {
            if (req.prompt.trimmed().isEmpty()) return fail(QStringLiteral("--generate needs a prompt"), 2);
            o.segments.push_back({req.prompt, req.duration > 0.0 ? req.duration : 5.0});
        }
        o.numSamples = req.variations;
        o.hasSeed = req.hasSeed;
        o.seed = req.seed;
        o.diffusionSteps = req.diffusionSteps;
        o.rootMotion = req.rootMotion;
        o.postprocess = req.postprocess;
        o.constraintsPath = req.constraintsPath;
        o.textEncoderUrl = req.textEncoderUrl;
        o.textEncoderQuant = req.textEncoderQuant;
        o.runtime = req.runtime;
        if (!o.runtime.isEmpty() && !KimodoPredictor::runtimeModes().contains(o.runtime))
            return fail(QStringLiteral("--runtime must be one of: %1").arg(KimodoPredictor::runtimeModes().join(QStringLiteral(", "))), 2);
        if (req.download && !KimodoNative::modelsInstalled()) {
            note(QStringLiteral("(kimodo: downloading the native models, ~%1 GB)").arg(double(KimodoNative::missingBytes()) / 1e9, 0, 'f', 1));
            QString e;
            if (!KimodoNative::downloadModels(&e)) return fail(QStringLiteral("download failed: %1").arg(e));
        }
        SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.kimodo"),
                                      QStringLiteral("CLI generate segments=%1 variations=%2").arg(o.segments.size()).arg(o.numSamples));
        QString lastStage;
        const auto r = KimodoPredictor::predict(o, [&](const QString& stage, int done, int total) {
            if (stage != lastStage) { note(QStringLiteral("(kimodo: %1)").arg(stage)); lastStage = stage; }
            Q_UNUSED(done); Q_UNUSED(total);
            return true;
        });
        if (!r.ok) return fail(r.error);
        files = r.files;
        manifest = r.manifest;
    }

    std::vector<Motion> motions;
    if (QString e; !loadMotionFiles(files, &motions, &e)) return fail(e);

    if (!CLIPipeline::initOgreHeadless()) return 1;
    MeshImporterExporter::importer({QFileInfo(req.rigPath).absoluteFilePath()});
    Ogre::Entity* entity = nullptr;
    for (auto* e : Manager::getSingleton()->getEntities())
        if (e && e->getMovableType() == "Entity" && static_cast<Ogre::Entity*>(e)->hasSkeleton()) {
            entity = static_cast<Ogre::Entity*>(e);
            break;
        }
    if (!entity) return fail(QStringLiteral("%1 has no skinned mesh: Kimodo motion needs a rigged humanoid").arg(req.rigPath));
    SceneApplyOptions so;
    so.baseName = req.name;
    so.rootMotion = req.rootMotion;
    so.footPin = req.footPin;
    so.undoable = false;   // one-shot process
    const SceneApplyResult applied = applyMotionsToEntity(entity, motions, so);
    if (!applied.ok) return fail(applied.error);
    if (std::abs(req.armSpaceDeg) > 1e-4f)   // #854, as the other generate backends do
        for (const QString& clip : applied.clips)
            if (AnimationMerger::adjustArmSpace(entity->getMesh()->getSkeleton().get(), clip.toStdString(), req.armSpaceDeg))
                note(QStringLiteral("(arm-space: %1 deg on %2)").arg(req.armSpaceDeg).arg(clip));

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
    return 0;
}

} // namespace AIMotion
