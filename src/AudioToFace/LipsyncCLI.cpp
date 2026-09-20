#include "LipsyncCLI.h"

#include "A2FPredictor.h"
#include "WavReader.h"

#include "../CLIPipeline.h"
#include "../Manager.h"
#include "../MeshImporterExporter.h"
#include "../MorphAnimationManager.h"
#include "../SentryReporter.h"


#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QHash>
#include <QString>
#include <QStringList>

#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSceneNode.h>

namespace AudioToFace {
namespace LipsyncCLI {

namespace {

// Explicit emotion, in the order network_info.json lists them. User-set only:
// the Audio2Emotion model that would predict these is licensed "use allowed
// with Audio2Face only", which fails this project's redistribution bar, so it
// is never downloaded and never inferred.
const char* const kEmotionNames[] = {
    "amazement", "anger", "cheekiness", "disgust", "fear",
    "grief", "joy", "outofbreath", "pain", "sadness",
};
constexpr int kEmotionCount = 10;

void usage()
{
    CLIPipeline::writeOutput(QStringLiteral(
        "Usage: qtmesh lipsync <audio.wav> --mesh <head.glb> [-o <out.glb>] [options]\n"
        "\n"
        "Generates ARKit blendshape animation from speech and writes it onto the\n"
        "mesh's morph targets. The mesh needs ARKit-named targets — `qtmesh facerig`\n"
        "adds them to any humanoid face.\n"
        "\n"
        "Options:\n"
        "  --mesh <file>       mesh carrying ARKit morph targets (required)\n"
        "  -o <file>           write the animated mesh here\n"
        "  --fps <n>           output frame rate (default 30)\n"
        "  --clip <name>       animation clip name (default Lipsync)\n"
        "  --emotion <k=v>     explicit emotion, repeatable; e.g. --emotion joy=0.6\n"
        "                      (amazement anger cheekiness disgust fear grief joy\n"
        "                       outofbreath pain sadness)\n"
        "  --no-normalise      do not peak-normalise the audio first\n"
        "  --map <file>        morph-target name overrides (JSON)\n"
        "  --json              machine-readable report\n"));
}

}  // namespace

int run(int argc, char* argv[])
{
    QString audioPath, meshPath, outPath, mapPath;
    QString clipName = QStringLiteral("Lipsync");
    double fps = 30.0;
    bool normalise = true, json = false;
    std::vector<float> emotion(size_t(kEmotionCount), 0.0f);

    for (int i = 2; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        auto next = [&]() -> QString {
            return (i + 1 < argc) ? QString::fromLocal8Bit(argv[++i]) : QString();
        };
        if (a == QStringLiteral("--mesh")) meshPath = next();
        else if (a == QStringLiteral("-o") || a == QStringLiteral("--output")) outPath = next();
        else if (a == QStringLiteral("--fps")) fps = next().toDouble();
        else if (a == QStringLiteral("--clip")) clipName = next();
        else if (a == QStringLiteral("--map")) mapPath = next();
        else if (a == QStringLiteral("--no-normalise") || a == QStringLiteral("--no-normalize"))
            normalise = false;
        else if (a == QStringLiteral("--json")) json = true;
        else if (a == QStringLiteral("--help") || a == QStringLiteral("-h")) { usage(); return 0; }
        else if (a == QStringLiteral("--emotion")) {
            const QString kv = next();
            const int eq = kv.indexOf(QLatin1Char('='));
            if (eq <= 0) {
                CLIPipeline::writeCliError(QStringLiteral(
                    "Error: --emotion wants name=value, e.g. --emotion joy=0.6\n"));
                return 2;
            }
            const QString name = kv.left(eq).trimmed().toLower();
            bool ok = false;
            const float v = kv.mid(eq + 1).toFloat(&ok);
            int idx = -1;
            for (int e = 0; e < kEmotionCount; ++e)
                if (name == QString::fromLatin1(kEmotionNames[e])) { idx = e; break; }
            if (idx < 0 || !ok) {
                QStringList valid;
                for (const char* n : kEmotionNames) valid << QString::fromLatin1(n);
                CLIPipeline::writeCliError(
                    QStringLiteral("Error: unknown emotion '%1'. Valid: %2\n")
                        .arg(name, valid.join(QStringLiteral(", "))));
                return 2;
            }
            emotion[size_t(idx)] = qBound(0.0f, v, 1.0f);
        }
        else if (!a.startsWith(QLatin1Char('-')) && audioPath.isEmpty()) audioPath = a;
        else {
            CLIPipeline::writeCliError(QStringLiteral("Error: unknown option %1\n").arg(a));
            return 2;
        }
    }

    if (audioPath.isEmpty() || meshPath.isEmpty()) { usage(); return 2; }
    if (fps <= 0.0 || fps > 240.0) {
        CLIPipeline::writeCliError(QStringLiteral("Error: --fps must be in (0, 240]\n"));
        return 2;
    }

    if (!A2FPredictor::available()) {
        CLIPipeline::writeCliError(QStringLiteral(
            "Error: Audio2Face needs ONNX Runtime; rebuild with -DENABLE_ONNX=ON.\n"));
        return 1;
    }
    const QFileInfo audioFi(audioPath), meshFi(meshPath);
    if (!audioFi.exists()) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: audio not found: %1\n").arg(audioPath));
        return 1;
    }
    if (!meshFi.exists()) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: mesh not found: %1\n").arg(meshPath));
        return 1;
    }

    QString err;
    const WavData wavData = readWav(audioFi.absoluteFilePath(), &err);
    if (!wavData.valid()) {
        CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(err));
        return 1;
    }

    if (!CLIPipeline::initOgreHeadless()) return 1;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.audio2face"),
        QStringLiteral("cli lipsync mesh=.%1 %2s @%3fps")
            .arg(meshFi.suffix()).arg(wavData.durationSec(), 0, 'f', 1).arg(fps));

    MeshImporterExporter::importer({meshFi.absoluteFilePath()});
    Ogre::Entity* entity = nullptr;
    for (Ogre::Entity* e : Manager::getSingleton()->getEntities())
        if (e && e->getMovableType() == "Entity") { entity = e; break; }
    if (!entity) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: failed to load %1\n").arg(meshPath));
        return 1;
    }

    const QStringList targets =
        MorphAnimationManager::instance()->morphTargetsFor(entity);
    if (targets.isEmpty()) {
        // The actionable part is WHERE to get the targets — this is the most
        // likely way a first attempt fails.
        CLIPipeline::writeCliError(QStringLiteral(
            "Error: %1 has no morph targets.\n"
            "Lipsync drives ARKit-named blendshapes (jawOpen, mouthPucker, ...).\n"
            "Add them with:  qtmesh facerig %1 -o rigged.glb\n").arg(meshPath));
        return 1;
    }

    A2FPredictor predictor;
    if (!predictor.load(&err)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(err));
        return 1;
    }

    PredictOptions popts;
    popts.fps = fps;
    popts.normalise = normalise;
    popts.emotion = emotion;
    if (!json) {
        popts.progress = [](int done, int total) {
            if (total > 0 && (done % 30) == 0)
                CLIPipeline::writeCliError(
                    QStringLiteral("\rSolving %1/%2...").arg(done).arg(total));
            return true;
        };
    }
    // `--map` overrides, in the same JSON shape the mocap CLI accepts:
    //   { "map": { "jawOpen": "JawDrop" }, "ignore": [ "tongueOut" ] }
    // A bad path or an override naming a target the mesh does not have is a
    // hard error: the user asked for a specific binding, and quietly falling
    // back to name matching would look like the file was honoured.
    QHash<QString, QString> overrideMap;
    QSet<QString> ignoreSet;
    if (!mapPath.isEmpty()) {
        QFile mf(mapPath);
        if (!mf.open(QIODevice::ReadOnly)) {
            CLIPipeline::writeCliError(
                QStringLiteral("Error: cannot open --map file: %1\n").arg(mapPath));
            return 1;
        }
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(mf.readAll(), &perr);
        if (doc.isNull() || !doc.isObject()) {
            CLIPipeline::writeCliError(
                QStringLiteral("Error: --map parse error in %1: %2\n")
                    .arg(mapPath, perr.errorString()));
            return 1;
        }
        const QJsonObject root = doc.object();
        const QJsonObject m = root.value(QLatin1String("map")).toObject();
        for (auto it = m.begin(); it != m.end(); ++it)
            overrideMap.insert(it.key(), it.value().toString());
        for (const auto& v : root.value(QLatin1String("ignore")).toArray())
            ignoreSet.insert(v.toString());

        // Validate the bindings NOW, before a minute of inference: an
        // override naming a target the mesh lacks is the likely mistake, and
        // reporting it after the solve wastes the user's time.
        for (auto it = overrideMap.constBegin(); it != overrideMap.constEnd(); ++it) {
            if (!targets.contains(it.value())) {
                CLIPipeline::writeCliError(
                    QStringLiteral("Error: --map binds %1 to '%2', which is not a "
                                   "morph target on this mesh.\n")
                        .arg(it.key(), it.value()));
                return 1;
            }
        }
    }

    const PredictResult pred =
        predictor.predict(wavData.samples, wavData.sampleRate, wavData.channels, popts);
    if (!json) CLIPipeline::writeCliError(QStringLiteral("\r                         \r"));
    if (!pred.ok()) {
        CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(pred.error));
        return 1;
    }

    // Write the weight keys directly through MorphAnimationManager rather
    // than via the mocap recorder. The recorder does the same thing, but it
    // lives behind ENABLE_MOCAP, which pulls in Qt Multimedia for a webcam
    // this feature never touches — lipsync should not require a camera stack
    // to animate from a file.
    //
    // Matching is by NAME against the mesh's own targets, case-insensitively
    // and ignoring a leading underscore, so a rig that spells it `JawOpen` or
    // `_jawOpen` still drives. Anything unmatched is reported rather than
    // silently dropped: a mesh missing half the ARKit set produces a
    // half-moving face, and the user needs to know which half.
    QHash<QString, QString> byNormalised;   // normalised -> the mesh's spelling
    auto canonical = [](QString n) {
        while (n.startsWith(QLatin1Char('_'))) n.remove(0, 1);
        return n.toLower();
    };
    for (const QString& t : targets) byNormalised.insert(canonical(t), t);


    const QStringList poses = predictor.poseNames();
    QStringList matched, unmatched, ignored;
    std::vector<QString> poseTarget(size_t(poses.size()));
    for (int i = 0; i < poses.size(); ++i) {
        if (ignoreSet.contains(poses[i])) { ignored << poses[i]; continue; }
        QString hit;
        if (overrideMap.contains(poses[i])) {
            hit = overrideMap.value(poses[i]);
            if (!targets.contains(hit)) {
                CLIPipeline::writeCliError(
                    QStringLiteral("Error: --map binds %1 to '%2', which is not a "
                                   "morph target on this mesh.\n").arg(poses[i], hit));
                return 1;
            }
        } else {
            hit = byNormalised.value(canonical(poses[i]));
        }
        poseTarget[size_t(i)] = hit;
        if (hit.isEmpty()) unmatched << poses[i]; else matched << poses[i];
    }
    if (matched.isEmpty()) {
        CLIPipeline::writeCliError(QStringLiteral(
            "Error: none of the mesh's %1 morph targets match an ARKit pose name.\n"
            "Expected names like jawOpen, mouthPucker, mouthSmileLeft.\n"
            "`qtmesh facerig` produces targets with the right names.\n")
            .arg(targets.size()));
        return 1;
    }

    // Choose key TIMES first, then write every matched pose at each one.
    //
    // Doing this per channel -- skipping a pose at frames where it did not
    // change -- looks like a harmless saving and is not. ARKit targets on one
    // submesh share a single VAT_POSE track, so a keyframe created for one
    // channel is a keyframe for ALL of them, and Ogre interpolates a pose that
    // is missing from the next keyframe TOWARD ZERO
    // (`VertexAnimationTrack::applyToVertexData`: "Search for entry in
    // keyframe 2 list (if not there, will be 0)"). A steady channel therefore
    // dips to 0 and back every time a sibling keys. Measured on a real
    // 59-frame take before this change: 39 such dips across 14 channels, e.g.
    // jawOpen 0.056 -> 0.000 -> 0.080 between adjacent frames.
    //
    // So the epsilon still decides WHEN to key -- that saving is real, and a
    // quiet take still collapses to a handful of times -- but never WHICH
    // poses a key carries.
    constexpr float kEpsilon = 0.01f;
    std::vector<float> lastWritten(size_t(poses.size()), -1.0f);
    std::vector<size_t> keyFrameIndices;
    for (size_t fi = 0; fi < pred.frames.size(); ++fi) {
        const auto& f = pred.frames[fi];
        // First and last frame are always keyed, so the clip starts and ends
        // at a defined pose instead of holding whatever preceded it.
        bool key = (fi == 0) || (fi + 1 == pred.frames.size());
        for (int i = 0; !key && i < poses.size() && i < int(f.weights.size()); ++i) {
            if (poseTarget[size_t(i)].isEmpty()) continue;
            if (std::abs(f.weights[size_t(i)] - lastWritten[size_t(i)]) >= kEpsilon)
                key = true;
        }
        if (!key) continue;
        keyFrameIndices.push_back(fi);
        for (int i = 0; i < poses.size() && i < int(f.weights.size()); ++i)
            if (!poseTarget[size_t(i)].isEmpty())
                lastWritten[size_t(i)] = f.weights[size_t(i)];
    }

    int keyframes = 0;
    const std::string clip = clipName.toStdString();

    // Replace, don't merge. `writeWeightKeyOn` reuses an existing animation
    // and only updates keys that land on the same time, and the clip length
    // only ever grows -- so re-running into a name that already exists leaves
    // stale keys from the previous take and can play motion past the end of
    // the new one. Same rule as MocapRecorder's replaceExisting.
    if (Ogre::MeshPtr mesh = entity->getMesh(); mesh && mesh->hasAnimation(clip))
        mesh->removeAnimation(clip);

    for (size_t fi : keyFrameIndices) {
        const auto& f = pred.frames[fi];
        for (int i = 0; i < poses.size() && i < int(f.weights.size()); ++i) {
            if (poseTarget[size_t(i)].isEmpty()) continue;
            if (MorphAnimationManager::writeWeightKeyOn(
                    entity, clip, poseTarget[size_t(i)].toStdString(),
                    float(f.timeSec), f.weights[size_t(i)]))
                ++keyframes;
        }
    }
    entity->refreshAvailableAnimationState();

    if (keyframes == 0) {
        CLIPipeline::writeCliError(QStringLiteral(
            "Error: the solve produced no motion above the %1 threshold — the "
            "audio may be silent.\n").arg(double(kEpsilon)));
        return 1;
    }

    if (!outPath.isEmpty()) {
        QDir().mkpath(QFileInfo(outPath).absolutePath());
        Ogre::SceneNode* node = entity->getParentSceneNode();
        const QString fmt = CLIPipeline::formatForExtension(outPath);
        if (!node || MeshImporterExporter::exporter(
                         node, QFileInfo(outPath).absoluteFilePath(), fmt) != 0) {
            CLIPipeline::writeCliError(
                QStringLiteral("Error: failed to write %1\n").arg(outPath));
            return 1;
        }
    }

    double meanResidual = 0.0;
    for (const auto& f : pred.frames) meanResidual += f.solveResidual;
    meanResidual /= double(pred.frames.size());

    if (json) {
        QJsonObject o;
        o["audio"] = audioFi.fileName();
        o["duration_sec"] = wavData.durationSec();
        o["frames"] = int(pred.frames.size());
        o["fps"] = fps;
        o["clip"] = clipName;
        o["keyframes"] = keyframes;
        o["model"] = pred.modelVersion;
        o["mean_solve_residual"] = meanResidual;
        QJsonArray matchedArr;
        for (const QString& m : matched) matchedArr.append(m);
        o["matched_channels"] = matchedArr;
        QJsonArray unmatchedArr;
        for (const QString& m : unmatched) unmatchedArr.append(m);
        o["unmatched_canonical"] = unmatchedArr;
        if (!ignored.isEmpty()) {
            QJsonArray ignoredArr;
            for (const QString& m : ignored) ignoredArr.append(m);
            o["ignored_channels"] = ignoredArr;
        }
        if (!outPath.isEmpty()) o["output"] = QFileInfo(outPath).fileName();
        CLIPipeline::writeOutput(
            QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
    } else {
        CLIPipeline::writeOutput(QStringLiteral(
            "Lipsync: %1 (%2s) -> clip '%3'\n"
            "  model            %4\n"
            "  frames           %5 @ %6 fps\n"
            "  keyframes        %7\n"
            "  channels matched %8 of %9\n"
            "  solve residual   %10\n")
            .arg(audioFi.fileName())
            .arg(wavData.durationSec(), 0, 'f', 2)
            .arg(clipName, pred.modelVersion)
            .arg(pred.frames.size()).arg(fps, 0, 'f', 0)
            .arg(keyframes)
            .arg(matched.size())
            .arg(matched.size() + unmatched.size())
            .arg(meanResidual, 0, 'f', 3));
        if (!outPath.isEmpty())
            CLIPipeline::writeOutput(QStringLiteral("  written to       %1\n").arg(outPath));
        if (!unmatched.isEmpty())
            CLIPipeline::writeOutput(
                QStringLiteral("  note: %1 ARKit channels have no target on this mesh\n")
                    .arg(unmatched.size()));
    }
    return 0;
}

}  // namespace LipsyncCLI
}  // namespace AudioToFace
