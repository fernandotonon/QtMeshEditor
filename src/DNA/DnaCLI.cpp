#include "DnaCLI.h"
#include <OgreMatrix4.h>
#include <OgreBone.h>
#include "../SelectionSet.h"
#include <functional>
#include <map>
#include <cstring>
#include <set>
#include "DnaReader.h"

#include "../CLIPipeline.h"
#include "../Manager.h"
#include "../MeshImporterExporter.h"
#include "DnaCorrectiveManager.h"
#include "../MorphAnimationManager.h"
#include "DnaArkitMap.h"
#include "DnaFaceBake.h"
#include "DnaPsdBake.h"
#include "DnaRigLogic.h"
#include "DnaScene.h"
#include "../ModelTurntableRenderer.h"

#include <QImage>
#include <QSaveFile>
#include <QPainter>

#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMesh.h>
#include <OgreSubEntity.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>
#include <OgreSceneNode.h>

#include <algorithm>
#include <cmath>
#include <QFile>
#include <QFileInfo>
#include <QVariantMap>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace Dna {
namespace DnaCLI {

namespace {
void usage()
{
    CLIPipeline::writeOutput(QStringLiteral(
        "Usage: qtmesh dna info <file.dna> [--lod N] [--json] [--keep-units]\n"
        "       qtmesh dna pose <body.dna> --set <joint>=<axis>:<deg>|<rx,ry,rz> [--set ...]\n"
        "                       [--compare [--vs-rig]] [--no-correctives] [-o posed.stl|.obj|.glb] [--json]\n"
        "\n"
        "       qtmesh dna psd <body.dna> [--bake correctives.json] [--validate [--poses N]] [--json]\n"
        "       qtmesh dna face <head.dna> [-o head.glb|sheet.png] [--all-controls] [--map arkit.json]\n"
        "                       [--preview label:shape=w,shape=w ...] [--validate] [--json]\n"
        "\n"
        "face bakes the face rig into ARKit-named morph targets (see below).\n"
        "psd bakes the rig's correctives into generic pose-space RBF sets (per-vertex offsets that live on the\n"
        "mesh, independent of RigLogic); --validate compares plain skinning and plain+sets against the full rig\n"
        "over random multi-joint poses (millimetres).\n"
        "pose bends driver joints (rotation relative to neutral, degrees, local axes) and\n"
        "evaluates the body correctives. --compare reports how far the corrected\n"
        "deformation moves the skinned vertices versus plain skinning (millimetres).\n"
        "Example: qtmesh dna pose body.dna --set calf_l=z:-90 --set thigh_l=x:60 --compare\n"
        "\n"
        "Reports a MetaHuman DNA file: descriptor, joints, meshes, blend-shape\n"
        "channels and the rig-logic data it carries (controls, PSD, RBF, ...).\n"
        "Convert a DNA to a regular mesh with: qtmesh convert head.dna -o head.glb\n"
        "Requires a build with -DENABLE_OPENRIGLOGIC=ON.\n"));
}
}

namespace {
/// Largest vertex displacement (millimetres) between the rig's neutral positions and `truth`, over the given meshes.
double maxMovedMm(const Rig& rig, const std::vector<size_t>& meshes, const std::vector<std::vector<float>>& truth)
{
    double moved = 0;
    for (size_t mi : meshes) {
        const std::vector<float>& neutral = rig.meshes[mi].positions;
        for (size_t i = 0; i < truth[mi].size(); ++i) moved = std::max(moved, double(std::abs(truth[mi][i] - neutral[i])) * 1000.0);
    }
    return moved;
}
}  // namespace

static int runPose(int argc, char* argv[]);
static int runFace(int argc, char* argv[]);
static int runPsd(int argc, char* argv[]);

int run(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("pose"))
            return runPose(argc, argv);
    for (int i = 1; i < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("face"))
            return runFace(argc, argv);
    for (int i = 1; i < argc; ++i)
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("psd"))
            return runPsd(argc, argv);
    QString path;
    bool json = false;
    ReadOptions opt;
    opt.loadBehavior = true;
    bool sawInfo = false;
    bool sawDna = false;
    // CLIPipeline::run forwards the full argv and accepts global flags before
    // the subcommand (`--cli dna info x.dna`), so scan from 1 and skip them.
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--cli" || a == "--verbose" || a == "--no-telemetry") continue;
        if (!sawDna && a == "dna") { sawDna = true; continue; }
        if (a == "info" && !sawInfo) sawInfo = true;
        else if (a == "--json") json = true;
        else if (a == "--keep-units") opt.units = Units::Preserve;
        else if (a == "--lod" && i + 1 < argc) {
            bool ok = false;
            opt.lod = QString::fromLocal8Bit(argv[++i]).toInt(&ok);
            if (!ok || opt.lod < 0) {
                CLIPipeline::writeCliError(QStringLiteral("Error: --lod expects a non-negative integer\n"));
                return 2;
            }
        }
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.startsWith('-') && path.isEmpty()) path = a;
        else { CLIPipeline::writeCliError(QStringLiteral("Error: unknown argument %1\n").arg(a)); return 2; }
    }
    if (!sawInfo || path.isEmpty()) { usage(); return 2; }

    const ReadResult r = read(path, opt);
    if (!r.ok) {
        CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(r.error));
        return 1;
    }
    const Rig& g = r.rig;
    int verts = 0, tris = 0, targets = 0, skinned = 0;
    for (const Mesh& m : g.meshes) {
        verts += int(m.positions.size() / 3);
        const ExpandedMesh ex = expand(m, g.flipV);
        tris += int(ex.indices.size() / 3);
        targets += int(m.blendShapeTargets.size());
        if (!m.skin.empty()) ++skinned;
    }
    if (json) {
        QJsonObject o;
        o["name"] = g.descriptor.name;
        o["archetype"] = g.descriptor.archetype;
        o["gender"] = g.descriptor.gender;
        o["translationUnit"] = g.descriptor.translationUnit;
        o["coordinateSystem"] = g.descriptor.coordinateSystem;
        o["rotationSequence"] = g.descriptor.rotationSequence;
        o["lodCount"] = g.descriptor.lodCount;
        o["lod"] = g.lod;
        o["joints"] = int(g.joints.size());
        o["lodJoints"] = int(g.lodJointIndices.size());
        o["meshes"] = int(g.meshes.size());
        o["vertices"] = verts;
        o["triangles"] = tris;
        o["skinnedMeshes"] = skinned;
        o["blendShapeChannels"] = g.blendShapeChannels.size();
        o["blendShapeTargets"] = targets;
        o["guiControls"] = g.behavior.guiControls;
        o["rawControls"] = g.behavior.rawControls;
        o["psd"] = g.behavior.psdCount;
        o["jointGroups"] = g.behavior.jointGroups;
        o["rbfPoses"] = g.behavior.rbfPoses;
        o["neuralNetworks"] = g.behavior.neuralNetworks;
        o["twists"] = g.behavior.twists;
        o["swings"] = g.behavior.swings;
        o["animatedMaps"] = g.behavior.animatedMaps;
        QJsonArray w;
        for (const QString& s : g.warnings) w.append(s);
        o["warnings"] = w;
        CLIPipeline::writeOutput(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
    } else {
        CLIPipeline::writeOutput(QStringLiteral(
            "DNA: %1 (%2, %3)\n"
            "  units: %4  coords: %5  rotation: %6\n"
            "  LODs: %7 (showing LOD %8)\n"
            "  joints: %9 (%10 at this LOD)\n"
            "  meshes: %11  vertices: %12  triangles: %13  skinned meshes: %14\n"
            "  blend-shape channels: %15  targets: %16\n"
            "  rig logic: %17 GUI / %18 raw controls, %19 PSD, %20 joint groups, %21 RBF poses, "
            "%22 neural nets, %23 twists, %24 swings\n")
            .arg(g.descriptor.name, g.descriptor.archetype, g.descriptor.gender)
            .arg(g.descriptor.translationUnit, g.descriptor.coordinateSystem, g.descriptor.rotationSequence)
            .arg(g.descriptor.lodCount).arg(g.lod)
            .arg(g.joints.size()).arg(g.lodJointIndices.size())
            .arg(g.meshes.size()).arg(verts).arg(tris).arg(skinned)
            .arg(g.blendShapeChannels.size()).arg(targets)
            .arg(g.behavior.guiControls).arg(g.behavior.rawControls).arg(g.behavior.psdCount)
            .arg(g.behavior.jointGroups).arg(g.behavior.rbfPoses).arg(g.behavior.neuralNetworks)
            .arg(g.behavior.twists).arg(g.behavior.swings));
        for (const QString& s : g.warnings)
            CLIPipeline::writeOutput(QStringLiteral("  warning: %1\n").arg(s));
    }
    return 0;
}

namespace {

// "z:-90" (one axis) or "10,20,30" (xyz Euler) -> quaternion (w,x,y,z).
bool parseRotation(const QString& spec, std::array<double, 4>* q)
{
    std::array<double, 3> a{0, 0, 0};
    if (spec.contains(QLatin1Char(':'))) {
        const QStringList p = spec.split(QLatin1Char(':'));
        if (p.size() != 2) return false;   // "x:10:20" is not "x:10"
        const QString ax = p[0].toLower();
        bool ok = false;
        const double deg = p.value(1).toDouble(&ok);
        // QString::toDouble accepts "nan" / "inf": a non-finite angle would put NaN
        // into the rig controls, so refuse it here.
        if (!ok || !std::isfinite(deg) || ax.size() != 1 || !QStringLiteral("xyz").contains(ax)) return false;
        a[size_t(ax[0].unicode() - 'x')] = deg;
    } else {
        const QStringList p = spec.split(QLatin1Char(','));
        if (p.size() != 3) return false;
        for (int i = 0; i < 3; ++i) {
            bool ok = false;
            a[size_t(i)] = p[i].toDouble(&ok);
            if (!ok || !std::isfinite(a[size_t(i)])) return false;
        }
    }
    *q = eulerToQuaternion(a, QStringLiteral("xyz"), true);
    return true;
}

}  // namespace

static int runPose(int argc, char* argv[])
{
    QString path, out;
    QList<QPair<QString, QString>> sets;
    bool json = false, compare = false, noCorrectives = false, vsRig = false;
    bool sawDna = false, sawPose = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--cli" || a == "--verbose" || a == "--no-telemetry") continue;
        if (!sawDna && a == "dna") { sawDna = true; continue; }
        if (!sawPose && a == "pose") { sawPose = true; continue; }
        if (a == "--json") json = true;
        else if (a == "--compare") compare = true;
        else if (a == "--no-correctives") noCorrectives = true;
        else if (a == "--vs-rig") vsRig = true;
        else if ((a == "-o" || a == "--output") && i + 1 < argc) out = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--set" && i + 1 < argc) {
            const QString kv = QString::fromLocal8Bit(argv[++i]);
            const int eq = kv.indexOf(QLatin1Char('='));
            if (eq <= 0) { CLIPipeline::writeCliError(QStringLiteral("Error: --set expects joint=axis:deg\n")); return 2; }
            sets.append({kv.left(eq), kv.mid(eq + 1)});
        }
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.startsWith('-') && path.isEmpty()) path = a;
        else { CLIPipeline::writeCliError(QStringLiteral("Error: unknown argument %1\n").arg(a)); return 2; }
    }
    if (path.isEmpty() || sets.isEmpty()) { usage(); return 2; }
    if (vsRig && !compare) {
        CLIPipeline::writeCliError(QStringLiteral("Error: --vs-rig is a part of --compare (it adds the full-rig ground truth to that report); pass both\n"));
        return 2;
    }
    if (!available()) {
        CLIPipeline::writeCliError(QStringLiteral("Error: MetaHuman DNA support is not compiled into this build. "
                                                  "Rebuild QtMeshEditor with -DENABLE_OPENRIGLOGIC=ON.\n"));
        return 1;
    }
    if (!CLIPipeline::initOgreHeadless()) return 1;

    MeshImporterExporter::clearLastImportError();
    MeshImporterExporter::importer({QFileInfo(path).absoluteFilePath()});
    const QString entityName = QFileInfo(path).completeBaseName() + QStringLiteral("_dna_node");
    Ogre::Entity* entity = nullptr;
    for (auto* obj : Manager::getSingleton()->getEntities())
        if (obj && obj->getMovableType() == "Entity" && obj->getName() == entityName.toStdString())
            entity = static_cast<Ogre::Entity*>(obj);
    DnaCorrectiveManager* mgr = DnaCorrectiveManager::peek();
    if (!entity) {
        const QString why = MeshImporterExporter::lastImportError();
        CLIPipeline::writeCliError(QStringLiteral("Error: could not import %1%2\n")
                                       .arg(path, why.isEmpty() ? QString() : QStringLiteral(": ") + why));
        return 1;
    }
    if (!mgr || !mgr->isAttached(entityName)) {
        CLIPipeline::writeCliError(QStringLiteral(
            "Error: %1 has no body rig to evaluate (no quaternion driver joints). Body correctives need a "
            "MetaHuman BODY DNA; facial controls are a later phase.\n").arg(QFileInfo(path).fileName()));
        return 1;
    }
    std::vector<float> neutral;
    if (compare) {
        mgr->setEnabled(false);
        mgr->evaluateNow();
        neutral = DnaCorrectiveManager::skinnedPositions(entity);
        mgr->setEnabled(true);
    }
    for (const auto& kv : sets) {
        std::array<double, 4> q;
        if (!parseRotation(kv.second, &q)) {
            CLIPipeline::writeCliError(QStringLiteral("Error: bad rotation '%1' (use axis:deg or rx,ry,rz)\n").arg(kv.second));
            return 2;
        }
        if (!mgr->setPoseOverride(entityName, kv.first, q)) {
            CLIPipeline::writeCliError(QStringLiteral("Error: joint '%1' is not in the imported skeleton\n").arg(kv.first));
            return 1;
        }
    }

    QVariantMap report;
    if (compare) {
        mgr->setEnabled(false);
        const std::vector<float> plain = DnaCorrectiveManager::skinnedPositions(entity);
        mgr->setEnabled(true);
        mgr->evaluateNow();
        const std::vector<float> corrected = DnaCorrectiveManager::skinnedPositions(entity);
        // Leave the character in the state the caller asked for, because any
        // -o export below reads the live pose.
        if (noCorrectives) {
            mgr->setEnabled(false);
            mgr->evaluateNow();
        }
        std::vector<double> d;
        if (plain.size() == corrected.size()) {
            for (size_t i = 0; i + 2 < plain.size(); i += 3) {
                const double dx = corrected[i] - plain[i], dy = corrected[i + 1] - plain[i + 1],
                             dz = corrected[i + 2] - plain[i + 2];
                d.push_back(std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0);  // metres -> mm
            }
        }
        double bend = 0;   // how far the posed limb itself moved (plain skinning vs neutral)
        if (neutral.size() == plain.size())
            for (size_t i = 0; i < plain.size(); ++i) bend = std::max(bend, double(std::abs(plain[i] - neutral[i])) * 1000.0);
        report.insert("poseMaxAxisDisplacementMm", bend);
        std::sort(d.begin(), d.end());
        double sum = 0; int over1 = 0, over5 = 0;
        for (double v : d) { sum += v; over1 += v > 1.0; over5 += v > 5.0; }
        report.insert("vertices", int(d.size()));
        report.insert("maxDisplacementMm", d.empty() ? 0.0 : d.back());
        report.insert("meanDisplacementMm", d.empty() ? 0.0 : sum / double(d.size()));
        report.insert("p99DisplacementMm", d.empty() ? 0.0 : d[size_t(double(d.size() - 1) * 0.99)]);
        report.insert("verticesMovedOver1mm", over1);
        report.insert("verticesMovedOver5mm", over5);
        if (vsRig) {
            // Ground truth: the FULL skeleton (every joint, every influence, all correctives) evaluated
            // by RigLogic and skinned on the CPU — what the import trades away to fit Ogre's 256 bones
            // and 4 influences per vertex.
            ReadOptions gro;
            gro.loadBehavior = true;
            const ReadResult full = read(QFileInfo(path).absoluteFilePath(), gro);
            QString gerr;
            auto gev = RigEvaluator::create(QFileInfo(path).absoluteFilePath(), gro, &gerr);
            if (!full.ok || !gev) {
                CLIPipeline::writeCliError(QStringLiteral("Error: --vs-rig could not read the full rig: %1\n").arg(full.ok ? gerr : full.error));
                return 1;
            }
            QList<QPair<QString, std::array<double, 4>>> rots;
            for (const auto& kv : sets) {
                std::array<double, 4> q;
                parseRotation(kv.second, &q);
                rots << qMakePair(kv.first, q);
            }
            const auto truth = FaceBake::evaluatePositionsPosed(full.rig, *gev, rots);
            std::vector<size_t> imported;   // the meshes the importer keeps: those that expand to triangles
            for (size_t m = 0; m < full.rig.meshes.size(); ++m)
                if (!expand(full.rig.meshes[m], full.rig.flipV).indices.empty()) imported.push_back(m);
            report.insert("vsRigTruthMovedMm", maxMovedMm(full.rig, imported, truth));
            std::vector<double> err;
            size_t ev0 = 0;
            double worst = 0; size_t worstMesh = 0, worstPos = 0;
            for (size_t mi : imported) {
                const ExpandedMesh ex = expand(full.rig.meshes[mi], full.rig.flipV);
                for (size_t v = 0; v < ex.positionOf.size(); ++v, ++ev0) {
                    if (ev0 * 3 + 2 >= corrected.size()) break;
                    const size_t p = ex.positionOf[v];
                    const double dx = corrected[ev0 * 3] - truth[mi][p * 3], dy = corrected[ev0 * 3 + 1] - truth[mi][p * 3 + 1], dz = corrected[ev0 * 3 + 2] - truth[mi][p * 3 + 2];
                    const double e = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0;
                    err.push_back(e);
                    if (e > worst) { worst = e; worstMesh = mi; worstPos = p; }
                }
            }
            std::sort(err.begin(), err.end());
            double esum = 0; int e1 = 0, e5 = 0;
            for (double v : err) { esum += v; e1 += v > 1.0; e5 += v > 5.0; }
            report.insert("vsRigMaxMm", err.empty() ? 0.0 : err.back());
            report.insert("vsRigMeanMm", err.empty() ? 0.0 : esum / double(err.size()));
            report.insert("vsRigP99Mm", err.empty() ? 0.0 : err[size_t(double(err.size() - 1) * 0.99)]);
            report.insert("vsRigOver1mm", e1);
            report.insert("vsRigOver5mm", e5);
            if (!err.empty() && worstMesh < full.rig.meshes.size() && worstPos < full.rig.meshes[worstMesh].skin.size()) {
                float bw = 0; QString bn;
                for (const auto& inf : full.rig.meshes[worstMesh].skin[worstPos])
                    if (inf.weight > bw && inf.joint < full.rig.joints.size()) { bw = inf.weight; bn = full.rig.joints[inf.joint].name; }
                report.insert("vsRigWorstVertexDominantJoint", bn);
            }
        }
    } else {
        mgr->setEnabled(!noCorrectives);
        mgr->evaluateNow();
    }
    const QVariantMap st = mgr->statsMap(entityName);
    for (auto it = st.cbegin(); it != st.cend(); ++it) report.insert(it.key(), it.value());

    if (!out.isEmpty() && out.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        // A 4-view strip (front, side, back, side) of the posed character. The
        // renderer drives frames through the scene, so the corrective manager's
        // per-frame hook evaluates exactly as it does in the viewport.
        TurntableOptions to;
        to.width = 512; to.height = 768; to.frameCount = 4; to.elevationDegrees = 5.0f; to.studio = true;
        QList<QImage> frames;
        QString rerr;
        if (!ModelTurntableRenderer::renderToImages({entity}, to, &frames, &rerr) || frames.isEmpty()) {
            CLIPipeline::writeCliError(QStringLiteral("Error: render failed: %1\n").arg(rerr));
            return 1;
        }
        QImage strip(to.width * frames.size(), to.height, QImage::Format_ARGB32);
        strip.fill(Qt::black);
        QPainter pt(&strip);
        for (int i = 0; i < frames.size(); ++i) pt.drawImage(i * to.width, 0, frames[i]);
        pt.end();
        if (!strip.save(QFileInfo(out).absoluteFilePath())) {
            CLIPipeline::writeCliError(QStringLiteral("Error: could not write %1\n").arg(out));
            return 1;
        }
        report.insert("output", QFileInfo(out).absoluteFilePath());
    } else if (!out.isEmpty()) {
        if (MeshImporterExporter::exportCurrentPose(entity, QFileInfo(out).absoluteFilePath()) != 0) {
            CLIPipeline::writeCliError(QStringLiteral("Error: export failed: %1\n").arg(out));
            return 1;
        }
        report.insert("output", QFileInfo(out).absoluteFilePath());
    }

    if (json) {
        CLIPipeline::writeOutput(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(report)).toJson(QJsonDocument::Indented)));
    } else {
        QString t = QStringLiteral("Pose applied (%1).\n").arg(noCorrectives ? "plain skinning" : "with correctives");
        if (compare)
            t += QStringLiteral("Pose moved vertices by up to %1 mm (plain skinning vs neutral)\n")
                     .arg(report["poseMaxAxisDisplacementMm"].toDouble(), 0, 'f', 1);
        if (compare && vsRig)
            t += QStringLiteral("Imported (<=256 bones, 4 influences) vs the FULL DNA rig: max %1 mm, mean %2 mm, p99 %3 mm; %4 vertices >1 mm, %5 >5 mm (worst vertex: %6)\n")
                     .arg(report["vsRigMaxMm"].toDouble(), 0, 'f', 2).arg(report["vsRigMeanMm"].toDouble(), 0, 'f', 3)
                     .arg(report["vsRigP99Mm"].toDouble(), 0, 'f', 2).arg(report["vsRigOver1mm"].toInt()).arg(report["vsRigOver5mm"].toInt())
                     .arg(report["vsRigWorstVertexDominantJoint"].toString());
        if (compare)
            t += QStringLiteral("Corrected vs plain skinning: max %1 mm, mean %2 mm, p99 %3 mm; %4 of %5 vertices move >1 mm, %6 >5 mm\n")
                     .arg(report["maxDisplacementMm"].toDouble(), 0, 'f', 2).arg(report["meanDisplacementMm"].toDouble(), 0, 'f', 3)
                     .arg(report["p99DisplacementMm"].toDouble(), 0, 'f', 2).arg(report["verticesMovedOver1mm"].toInt())
                     .arg(report["vertices"].toInt()).arg(report["verticesMovedOver5mm"].toInt());
        t += QStringLiteral("%1 driver bones, %2 corrective bones evaluated (%3 not in skeleton), RigLogic %4 ms\n")
                 .arg(report["driverBones"].toInt()).arg(report["correctiveBones"].toInt())
                 .arg(report["correctiveJointsMissing"].toInt()).arg(report["rigLogicMs"].toDouble(), 0, 'f', 2);
        if (!out.isEmpty()) t += QStringLiteral("Wrote %1\n").arg(report["output"].toString());
        CLIPipeline::writeOutput(t);
    }
    return 0;
}

static int runFace(int argc, char* argv[])
{
    QString path, out, mapPath;
    QStringList previews;
    bool json = false, all = false, validate = false;
    bool sawDna = false, sawFace = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--cli" || a == "--verbose" || a == "--no-telemetry") continue;
        if (!sawDna && a == "dna") { sawDna = true; continue; }
        if (!sawFace && a == "face") { sawFace = true; continue; }
        if (a == "--json") json = true;
        else if (a == "--all-controls") all = true;
        else if (a == "--validate") validate = true;
        else if (a == "--map" && i + 1 < argc) mapPath = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--preview" && i + 1 < argc) previews << QString::fromLocal8Bit(argv[++i]);
        else if ((a == "-o" || a == "--output") && i + 1 < argc) out = QString::fromLocal8Bit(argv[++i]);
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.startsWith('-') && path.isEmpty()) path = a;
        else { CLIPipeline::writeCliError(QStringLiteral("Error: unknown argument %1\n").arg(a)); return 2; }
    }
    if (path.isEmpty()) { usage(); return 2; }
    if (!available()) {
        CLIPipeline::writeCliError(QStringLiteral("Error: MetaHuman DNA support is not compiled into this build. "
                                                  "Rebuild QtMeshEditor with -DENABLE_OPENRIGLOGIC=ON.\n"));
        return 1;
    }
    if (!CLIPipeline::initOgreHeadless()) return 1;

    ImportOptions io;
    io.allFaceControls = all;
    io.arkitMapPath = mapPath;
    ImportReport rep;
    QString err;
    Ogre::SceneNode* node = importToScene(QFileInfo(path).absoluteFilePath(), ReadOptions(), io, &rep, &err);
    if (!node) {
        CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(err));
        return 1;
    }
    if (rep.faceShapes == 0) {
        CLIPipeline::writeCliError(QStringLiteral(
            "Error: %1 has no face expression controls to bake (a body DNA? use `qtmesh dna pose`).\n")
                                       .arg(QFileInfo(path).fileName()));
        return 1;
    }

    QVariantMap report;
    report.insert("morphTargets", rep.morphTargets);
    report.insert("faceShapes", rep.faceShapes);
    report.insert("approximateArkitChannels", rep.faceShapesApproximate);
    report.insert("bakeMs", rep.faceBakeMs);
    report.insert("maxVertexDisplacementMm", rep.faceMaxDeltaMm);
    report.insert("jointsInOgreSkeleton", rep.joints);
    report.insert("warnings", rep.warnings);

    if (validate) {
        // The nonlinearity a morph target cannot keep: the rig evaluated with
        // several controls at once vs the sum of their baked shapes.
        ReadOptions ro;
        const ReadResult rr = read(QFileInfo(path).absoluteFilePath(), ro);
        QString e2;
        auto ev = RigEvaluator::create(QFileInfo(path).absoluteFilePath(), ro, &e2);
        QVariantList rows;
        if (!rr.ok || !ev) {
            CLIPipeline::writeCliError(QStringLiteral("Error: --validate could not run: %1\n")
                                           .arg(!rr.ok ? rr.error : e2));
            return 1;
        }
        {
            QList<ArkitMap::Entry> map = ArkitMap::defaultMap();
            if (!mapPath.isEmpty()) {
                const ArkitMap::LoadResult lr = ArkitMap::loadOverride(mapPath);
                if (!lr.ok) {
                    CLIPipeline::writeCliError(QStringLiteral("Error: ARKit map: %1\n").arg(lr.error));
                    return 1;
                }
                map = lr.entries;
            }
            const QList<FaceBake::ShapeSpec> all52 = FaceBake::arkitSpecs(map);
            auto pick = [&](const QStringList& names) {
                QList<FaceBake::ShapeSpec> v;
                for (const QString& n : names)
                    for (const auto& s : all52) if (s.name == n) v << s;
                return v;
            };
            const QList<QStringList> combos = {
                {"jawOpen", "mouthSmileLeft", "mouthSmileRight"},
                {"eyeBlinkLeft", "browDownLeft"},
                {"mouthPucker", "jawOpen"},
                {"mouthFunnel", "jawOpen", "browInnerUp"},
            };
            for (const QStringList& c : combos) {
                const auto lr = FaceBake::linearityError(rr.rig, *ev, pick(c));
                QVariantMap row;
                row.insert("shapes", c.join(QStringLiteral(" + ")));
                row.insert("maxErrorMm", lr.maxMm);
                row.insert("meanErrorMm", lr.meanMm);
                row.insert("expressionMaxMm", lr.combinedMaxMm);
                rows << row;
            }
        }
        report.insert("linearity", rows);
    }

    if (!out.isEmpty() && out.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)) {
        // Contact sheet: one front view per --preview (a neutral frame first).
        Ogre::Entity* face = nullptr;
        for (auto* o : node->getAttachedObjects())
            if (o && o->getMovableType() == "Entity") { face = static_cast<Ogre::Entity*>(o); break; }
        if (!face) { CLIPipeline::writeCliError(QStringLiteral("Error: no entity to render\n")); return 1; }
        QStringList sheets = previews;
        sheets.prepend(QStringLiteral("neutral:"));
        auto* morphs = MorphAnimationManager::instance();
        QList<QImage> images;
        for (const QString& p : sheets) {
            for (const QString& t : morphs->morphTargetsFor(face)) morphs->setWeight(face, t, 0.0f);
            const int colon = p.indexOf(QLatin1Char(':'));
            const QString spec = colon >= 0 ? p.mid(colon + 1) : p;
            for (const QString& kv : spec.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const QStringList nv = kv.split(QLatin1Char('='));
                bool ok = false;
                const double w = nv.value(1, QStringLiteral("1")).toDouble(&ok);
                if (!ok || !std::isfinite(w) || !morphs->setWeight(face, nv[0], float(w))) {
                    CLIPipeline::writeCliError(QStringLiteral("Error: cannot set '%1' (unknown target or bad weight)\n").arg(kv));
                    return 1;
                }
            }
            TurntableOptions to;
            to.width = 480; to.height = 540; to.frameCount = 1; to.elevationDegrees = 0.0f; to.studio = true;
            QList<QImage> frames;
            QString rerr;
            if (!ModelTurntableRenderer::renderToImages({face}, to, &frames, &rerr) || frames.isEmpty()) {
                CLIPipeline::writeCliError(QStringLiteral("Error: render failed: %1\n").arg(rerr));
                return 1;
            }
            images << frames.first();
        }
        QImage strip(480 * images.size(), 540, QImage::Format_ARGB32);
        strip.fill(Qt::black);
        QPainter pt(&strip);
        for (int i = 0; i < images.size(); ++i) pt.drawImage(i * 480, 0, images[i]);
        pt.end();
        if (!strip.save(QFileInfo(out).absoluteFilePath())) {
            CLIPipeline::writeCliError(QStringLiteral("Error: could not write %1\n").arg(out));
            return 1;
        }
        report.insert("output", QFileInfo(out).absoluteFilePath());
    } else if (!out.isEmpty()) {
        if (MeshImporterExporter::exporter(node, QFileInfo(out).absoluteFilePath(),
                                           CLIPipeline::formatForExtension(out)) != 0) {
            CLIPipeline::writeCliError(QStringLiteral("Error: export failed: %1\n").arg(out));
            return 1;
        }
        report.insert("output", QFileInfo(out).absoluteFilePath());
    }

    if (json) {
        CLIPipeline::writeOutput(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(report)).toJson(QJsonDocument::Indented)));
    } else {
        QString t = QStringLiteral("Baked %1 face morph targets in %2 ms (largest vertex displacement %3 mm; %4 ARKit channels are close composites, not exact).\n")
                        .arg(rep.faceShapes).arg(rep.faceBakeMs, 0, 'f', 0).arg(rep.faceMaxDeltaMm, 0, 'f', 1)
                        .arg(rep.faceShapesApproximate);
        for (const QVariant& v : report.value("linearity").toList()) {
            const QVariantMap m = v.toMap();
            t += QStringLiteral("  %1: summed shapes differ from the joint rig by max %2 mm / mean %3 mm (expression moves up to %4 mm)\n")
                     .arg(m["shapes"].toString()).arg(m["maxErrorMm"].toDouble(), 0, 'f', 2)
                     .arg(m["meanErrorMm"].toDouble(), 0, 'f', 3).arg(m["expressionMaxMm"].toDouble(), 0, 'f', 1);
        }
        for (const QString& w : rep.warnings) t += QStringLiteral("  note: %1\n").arg(w);
        if (!out.isEmpty()) t += QStringLiteral("Wrote %1\n").arg(report["output"].toString());
        CLIPipeline::writeOutput(t);
    }
    return 0;
}

static int runPsd(int argc, char* argv[])
{
    QString path, bakeOut;
    bool json = false, validate = false;
    int poses = 24;
    bool sawDna = false, sawPsd = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--cli" || a == "--verbose" || a == "--no-telemetry") continue;
        if (!sawDna && a == "dna") { sawDna = true; continue; }
        if (!sawPsd && a == "psd") { sawPsd = true; continue; }
        if (a == "--json") json = true;
        else if (a == "--validate") validate = true;
        else if (a == "--poses" && i + 1 < argc) {
            bool ok = false;
            poses = QString::fromLocal8Bit(argv[++i]).toInt(&ok);
            if (!ok || poses < 1 || poses > 500) { CLIPipeline::writeCliError(QStringLiteral("Error: --poses expects 1..500\n")); return 2; }
        }
        else if (a == "--bake" && i + 1 < argc) bakeOut = QString::fromLocal8Bit(argv[++i]);
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.startsWith('-') && path.isEmpty()) path = a;
        else { CLIPipeline::writeCliError(QStringLiteral("Error: unknown argument %1\n").arg(a)); return 2; }
    }
    if (path.isEmpty() || (!validate && bakeOut.isEmpty())) { usage(); return 2; }
    if (!available()) {
        CLIPipeline::writeCliError(QStringLiteral("Error: MetaHuman DNA support is not compiled into this build. "
                                                  "Rebuild QtMeshEditor with -DENABLE_OPENRIGLOGIC=ON.\n"));
        return 1;
    }
    ReadOptions ro;
    ro.loadBehavior = true;
    const ReadResult full = read(QFileInfo(path).absoluteFilePath(), ro);
    QString err;
    auto ev = RigEvaluator::create(QFileInfo(path).absoluteFilePath(), ro, &err);
    if (!full.ok || !ev) {
        CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(full.ok ? err : full.error));
        return 1;
    }
    const Psd::BakeResult b = Psd::bake(full.rig, *ev);
    QVariantMap report;
    report.insert("drivers", int(ev->driverJoints().size()));
    report.insert("setsBaked", b.driversBaked);
    report.insert("driversWithoutEffect", b.driversWithoutEffect);
    int samples = 0;
    long long entries = 0;
    for (const auto& s : b.sets) { samples += int(s.samples.size()); for (const auto& sm : s.samples) entries += qlonglong(sm.vertices.size()); }
    report.insert("samples", samples);
    report.insert("sparseVertexEntries", entries);
    if (!b.warnings.isEmpty()) report.insert("warnings", b.warnings);
    if (!bakeOut.isEmpty()) {
        QJsonArray arr;
        for (const auto& s : b.sets) arr.append(s.toJson());
        QJsonObject doc;
        doc.insert(QStringLiteral("schema"), QStringLiteral("qtmesh-pose-correctives-collection-v1"));
        doc.insert(QStringLiteral("sets"), arr);
        QSaveFile f(bakeOut);
        if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(doc).toJson(QJsonDocument::Compact)) < 0 || !f.commit()) {
            CLIPipeline::writeCliError(QStringLiteral("Error: cannot write %1\n").arg(bakeOut));
            return 1;
        }
        report.insert("written", bakeOut);
    }
    if (validate) {
        const Psd::Validation v = Psd::validate(full.rig, *ev, b.sets, poses);
        report.insert("validationPoses", v.poses);
        report.insert("plainMeanMm", v.plainMeanMm);
        report.insert("plainP95Mm", v.plainP95Mm);
        report.insert("plainMaxMm", v.plainMaxMm);
        report.insert("psdMeanMm", v.psdMeanMm);
        report.insert("psdP95Mm", v.psdP95Mm);
        report.insert("psdMaxMm", v.psdMaxMm);
    }
    if (json) CLIPipeline::writeOutput(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(report)).toJson(QJsonDocument::Indented)));
    else for (auto it = report.begin(); it != report.end(); ++it) {
        // a QStringList only converts to text when it has exactly one element
        const QString v = it.value().typeId() == QMetaType::QStringList ? it.value().toStringList().join(QStringLiteral("; "))
                                                                        : it.value().toString();
        CLIPipeline::writeOutput(QStringLiteral("%1: %2\n").arg(it.key(), v));
    }
    return 0;
}

}  // namespace DnaCLI
}  // namespace Dna
