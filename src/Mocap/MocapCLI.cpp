#include "MocapCLI.h"

#include "../CLIPipeline.h"

#include <QString>

#ifndef ENABLE_MOCAP

namespace MocapCLI {

int run(int, char*[])
{
    CLIPipeline::writeCliError(QStringLiteral(
        "Error: this build has no performance-capture support — rebuild with "
        "-DENABLE_MOCAP=ON -DENABLE_ONNX=ON.\n"));
    return 1;
}

}  // namespace MocapCLI

#else  // ENABLE_MOCAP

#include "FaceCapCanonicalData.h"
#include "FaceCapMapper.h"
#include "FaceCapPredictor.h"
#include "MocapRecorder.h"
#include "OneEuroFilter.h"
#include "VideoFrameSource.h"

#include "../GamificationManager.h"
#include "../Manager.h"
#include "../MeshImporterExporter.h"
#include "../MorphAnimationManager.h"
#include "../SentryReporter.h"

#include <OgreEntity.h>
#include <OgreSceneNode.h>

#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <array>
#include <vector>

namespace MocapCLI {

namespace {

const char* kUsage =
    "Usage: qtmesh mocap <video> --face --mesh <meshfile> [-o out.glb]\n"
    "              [--clip-name NAME] [--fps 30] [--smooth-cutoff HZ]\n"
    "              [--no-smooth] [--map overrides.json] [--no-head]\n"
    "              [--frames-dir DIR] [--json]\n"
    "\n"
    "  Captures facial performance from a video (or an image sequence via\n"
    "  --frames-dir) onto the mesh's ARKit-style morph targets as a weight\n"
    "  clip, plus head rotation on the Head bone (skinned) or node (static).\n"
    "  Body capture (--body) lands with #874.\n";

QJsonObject reportToJson(const MocapRecorder::FaceRecordReport& r)
{
    QJsonObject o;
    o.insert(QLatin1String("clipName"), r.clipName);
    o.insert(QLatin1String("framesProcessed"), r.framesProcessed);
    o.insert(QLatin1String("framesNoFace"), r.framesNoFace);
    o.insert(QLatin1String("keyframesWritten"), r.keyframesWritten);
    o.insert(QLatin1String("headKeyframesWritten"), r.headKeyframesWritten);
    o.insert(QLatin1String("headTarget"), r.headTarget);
    o.insert(QLatin1String("clipLength"), r.clipLength);
    o.insert(QLatin1String("matchedChannels"),
             QJsonArray::fromStringList(r.matchedChannels));
    o.insert(QLatin1String("unmatchedCanonical"),
             QJsonArray::fromStringList(r.unmatchedCanonical));
    o.insert(QLatin1String("unmatchedMesh"),
             QJsonArray::fromStringList(r.unmatchedMesh));
    if (!r.error.isEmpty())
        o.insert(QLatin1String("error"), r.error);
    return o;
}

}  // namespace

int run(int argc, char* argv[])
{
    QString videoPath, meshPath, outputPath, mapPath, framesDir;
    QString clipName = QStringLiteral("FaceCap");
    double fps = 30.0;
    double smoothCutoff = 1.0;
    bool smooth = true;
    bool face = false;
    bool head = true;
    bool jsonOutput = false;

    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QLatin1String("mocap") || arg == QLatin1String("--cli"))
            continue;
        if (arg == QLatin1String("--face")) { face = true; continue; }
        if (arg == QLatin1String("--body")) {
            CLIPipeline::writeCliError(QStringLiteral(
                "Error: --body is not implemented yet (tracked in #874); "
                "only --face is available.\n"));
            return 2;
        }
        if (arg == QLatin1String("--json")) { jsonOutput = true; continue; }
        if (arg == QLatin1String("--no-head")) { head = false; continue; }
        if (arg == QLatin1String("--no-smooth")) { smooth = false; continue; }
        auto value = [&](const char* flag) -> QString {
            if (i + 1 >= argc) {
                CLIPipeline::writeCliError(
                    QStringLiteral("Error: %1 requires a value.\n")
                        .arg(QLatin1String(flag)));
                return {};
            }
            return QString::fromLocal8Bit(argv[++i]);
        };
        if (arg == QLatin1String("--mesh")) {
            meshPath = value("--mesh");
            if (meshPath.isEmpty()) return 2;
            continue;
        }
        if (arg == QLatin1String("-o") || arg == QLatin1String("--output")) {
            outputPath = value("-o");
            if (outputPath.isEmpty()) return 2;
            continue;
        }
        if (arg == QLatin1String("--clip-name")) {
            clipName = value("--clip-name");
            if (clipName.isEmpty()) return 2;
            continue;
        }
        if (arg == QLatin1String("--map")) {
            mapPath = value("--map");
            if (mapPath.isEmpty()) return 2;
            continue;
        }
        if (arg == QLatin1String("--frames-dir")) {
            framesDir = value("--frames-dir");
            if (framesDir.isEmpty()) return 2;
            continue;
        }
        if (arg == QLatin1String("--fps")) {
            const QString v = value("--fps");
            if (v.isEmpty()) return 2;
            fps = v.toDouble();
            if (fps <= 0 || fps > 240) {
                CLIPipeline::writeCliError(
                    QStringLiteral("Error: --fps out of range (1..240).\n"));
                return 2;
            }
            continue;
        }
        if (arg == QLatin1String("--smooth-cutoff")) {
            const QString v = value("--smooth-cutoff");
            if (v.isEmpty()) return 2;
            smoothCutoff = v.toDouble();
            if (smoothCutoff <= 0) {
                CLIPipeline::writeCliError(
                    QStringLiteral("Error: --smooth-cutoff must be > 0 Hz.\n"));
                return 2;
            }
            continue;
        }
        if (!arg.startsWith(QLatin1Char('-')) && videoPath.isEmpty()) {
            videoPath = arg;
            continue;
        }
        CLIPipeline::writeCliError(
            QStringLiteral("Error: unknown option '%1'.\n%2")
                .arg(arg, QLatin1String(kUsage)));
        return 2;
    }

    if (!face) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: nothing to capture — pass --face.\n%1")
                .arg(QLatin1String(kUsage)));
        return 2;
    }
    if (meshPath.isEmpty()) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: --mesh <meshfile> is required.\n%1")
                .arg(QLatin1String(kUsage)));
        return 2;
    }
    if (videoPath.isEmpty() && framesDir.isEmpty()) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: no input video (or --frames-dir).\n%1")
                .arg(QLatin1String(kUsage)));
        return 2;
    }
    const QFileInfo meshFi(meshPath);
    if (!meshFi.exists()) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: mesh not found: %1\n").arg(meshPath));
        return 1;
    }
    if (!framesDir.isEmpty() && !QFileInfo(framesDir).isDir()) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: --frames-dir is not a directory: %1\n")
                .arg(framesDir));
        return 1;
    }
    if (framesDir.isEmpty() && !QFileInfo::exists(videoPath)) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: video not found: %1\n").arg(videoPath));
        return 1;
    }

    if (!CLIPipeline::initOgreHeadless())
        return 1;

    SentryReporter::addBreadcrumb(
        QStringLiteral("ai.assist.mocap_face"),
        QStringLiteral("cli mocap --face mesh=.%1 source=%2")
            .arg(meshFi.suffix(),
                 framesDir.isEmpty() ? QStringLiteral("video")
                                     : QStringLiteral("frames-dir")));

    // --- load the mesh + enumerate morph targets -----------------------------
    MeshImporterExporter::importer({meshFi.absoluteFilePath()});
    Ogre::Entity* entity = nullptr;
    for (Ogre::Entity* e : Manager::getSingleton()->getEntities()) {
        if (e && e->getMovableType() == "Entity") { entity = e; break; }
    }
    if (!entity) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: failed to load %1\n").arg(meshPath));
        return 1;
    }

    const QStringList targets =
        MorphAnimationManager::instance()->morphTargetsFor(entity);
    if (targets.isEmpty() && !head) {
        CLIPipeline::writeCliError(QStringLiteral(
            "Error: %1 has NO morph targets and --no-head was given — nothing "
            "to record.\nFace capture drives ARKit-style blendshape targets "
            "(jawOpen, mouthSmileLeft, ...). Ready Player Me avatars and\n"
            "CC/iClone heads ship with them; the Edit-Mode 'Vertex Morph "
            "Animation' section authors them manually.\n").arg(meshPath));
        return 1;
    }

    const FaceCapMapper::Mapping mapping = FaceCapMapper::build(targets, mapPath);
    if (!mapping.error.isEmpty()) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: %1\n").arg(mapping.error));
        return 1;
    }
    if (!jsonOutput) {
        QString table = QStringLiteral("Channel mapping (%1 matched):\n")
                            .arg(mapping.channels.size());
        for (const auto& ch : mapping.channels)
            table += QStringLiteral("  %1 -> %2\n")
                         .arg(QLatin1String(
                                  FaceCap::kBlendshapeNames[ch.canonicalIndex]),
                              ch.meshTargetName);
        if (!mapping.unmatchedCanonical.isEmpty())
            table += QStringLiteral("  unmatched capture channels: %1\n")
                         .arg(mapping.unmatchedCanonical.join(
                             QStringLiteral(", ")));
        if (!mapping.unmatchedMesh.isEmpty())
            table += QStringLiteral("  unmatched mesh targets: %1\n")
                         .arg(mapping.unmatchedMesh.join(QStringLiteral(", ")));
        CLIPipeline::writeOutput(table);
    }
    if (mapping.channels.isEmpty() && !targets.isEmpty() && !head) {
        CLIPipeline::writeCliError(QStringLiteral(
            "Error: none of the %1 mesh morph targets matched a capture "
            "channel — see the table above; a --map overrides.json can bind "
            "custom names.\n").arg(targets.size()));
        return 1;
    }

    // --- predictor ------------------------------------------------------------
    if (!FaceCapPredictor::modelsPresent()
        && FaceCapPredictor::ensureModelsBlocking().isEmpty()) {
        CLIPipeline::writeCliError(QStringLiteral(
            "Error: face capture models are not available (download failed, "
            "offline guard set, or not hosted yet).\nSet "
            "QTMESH_MOCAP_MODEL_BASE_URL or place the three graphs in %1.\n")
            .arg(FaceCapPredictor::modelDir()));
        return 1;
    }
    FaceCapPredictor predictor;
    if (!predictor.load()) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: %1\n").arg(predictor.lastError()));
        return 1;
    }

    // --- frame source -> samples ----------------------------------------------
    std::unique_ptr<VideoFrameSource> source;
    if (!framesDir.isEmpty()) {
        QDir dir(framesDir);
        QStringList images = dir.entryList(
            {QStringLiteral("*.png"), QStringLiteral("*.jpg"),
             QStringLiteral("*.jpeg"), QStringLiteral("*.bmp")},
            QDir::Files, QDir::Name);
        for (QString& f : images)
            f = dir.filePath(f);
        source = std::make_unique<ImageSequenceFrameSource>(images, fps);
    } else {
        source = std::make_unique<FileFrameSource>(videoPath, fps);
    }
    QString openError;
    if (!source->open(&openError)) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: %1\n").arg(openError));
        return 1;
    }

    std::vector<FaceSample> samples;
    std::array<OneEuroFilter, 52> weightFilters;
    OneEuroQuatFilter headFilter;
    if (smooth) {
        OneEuroFilter::Params params;
        params.minCutoff = smoothCutoff;
        for (auto& f : weightFilters)
            f = OneEuroFilter(params);
        headFilter = OneEuroQuatFilter(params);
    }

    QEventLoop loop;
    bool finished = false;
    QString streamError;
    QObject::connect(source.get(), &VideoFrameSource::frameReady,
                     [&](const MocapFrame& frame) {
                         FaceSample s = predictor.predict(frame.image,
                                                          frame.timeSec);
                         if (smooth && s.confidence > 0.f) {
                             for (int c = 0; c < 52; ++c)
                                 s.weights[c] = static_cast<float>(
                                     weightFilters[c].filter(s.weights[c],
                                                             s.timeSec));
                             s.headRotation =
                                 headFilter.filter(s.headRotation, s.timeSec);
                         }
                         samples.push_back(s);
                     });
    QObject::connect(source.get(), &VideoFrameSource::finished, [&] {
        finished = true;
        loop.quit();
    });
    QObject::connect(source.get(), &VideoFrameSource::errorOccurred,
                     [&](const QString& message) {
                         streamError = message;
                         finished = true;
                         loop.quit();
                     });
    source->start();
    if (!finished)
        loop.exec();
    if (!streamError.isEmpty()) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: %1\n").arg(streamError));
        return 1;
    }
    if (samples.empty()) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: the source produced no frames.\n"));
        return 1;
    }

    // --- record -----------------------------------------------------------------
    MocapRecorder::FaceRecordOptions options;
    options.clipName = clipName;
    options.head = head;
    const MocapRecorder::FaceRecordReport report =
        MocapRecorder::recordFace(entity, samples, mapping, options);
    if (!report.ok()) {
        CLIPipeline::writeCliError(
            QStringLiteral("Error: %1\n").arg(report.error));
        return 1;
    }

    // --- export -------------------------------------------------------------------
    if (!outputPath.isEmpty()) {
        Ogre::SceneNode* node = entity->getParentSceneNode();
        const QString fmt = CLIPipeline::formatForExtension(outputPath);
        if (!node
            || MeshImporterExporter::exporter(
                   node, QFileInfo(outputPath).absoluteFilePath(), fmt) != 0) {
            CLIPipeline::writeCliError(
                QStringLiteral("Error: export to %1 failed.\n").arg(outputPath));
            return 1;
        }
    }

    if (jsonOutput) {
        CLIPipeline::writeOutput(QString::fromUtf8(
            QJsonDocument(reportToJson(report)).toJson(QJsonDocument::Indented)));
    } else {
        CLIPipeline::writeOutput(
            QStringLiteral("Recorded '%1': %2 frames (%3 without a face), "
                           "%4 weight keys on %5 channels, %6 head keys (%7), "
                           "clip length %8s%9\n")
                .arg(report.clipName)
                .arg(report.framesProcessed)
                .arg(report.framesNoFace)
                .arg(report.keyframesWritten)
                .arg(report.matchedChannels.size())
                .arg(report.headKeyframesWritten)
                .arg(report.headTarget)
                .arg(report.clipLength, 0, 'f', 2)
                .arg(outputPath.isEmpty()
                         ? QString()
                         : QStringLiteral(" -> %1").arg(outputPath)));
    }

    GamificationManager::noteOperation(
        QStringLiteral("mocap_face"),
        {{QStringLiteral("frames"),
          static_cast<qint64>(report.framesProcessed)},
         {QStringLiteral("keyframes"),
          static_cast<qint64>(report.keyframesWritten
                              + report.headKeyframesWritten)}},
        GamificationManager::Surface::Cli);
    return 0;
}

}  // namespace MocapCLI

#endif  // ENABLE_MOCAP
