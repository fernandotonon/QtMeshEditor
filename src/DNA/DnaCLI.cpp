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
#include "DnaFit.h"
#include "DnaPsdBake.h"
#include "DnaRegister.h"
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
        "       qtmesh dna fit <body.dna> --target <edited mesh | other body.dna> [--poses] [--lods]\n"
        "                      [--register-joints [--no-align-limbs] [--refine-surface] | --register [--register-method smooth|nricp]]\n"
        "                      [--reference same_topology_mesh]\n"
        "                      [--uniform-scale S] [--write-dna fitted.dna] [--bake-correctives sets.json]\n"
        "                      [-o fitted.glb] [--json]\n"
        "       qtmesh dna psd <body.dna> [--bake correctives.json] [--validate [--poses N]] [--json]\n"
        "       qtmesh dna face <head.dna> [-o head.glb|sheet.png] [--all-controls] [--map arkit.json]\n"
        "                       [--preview label:shape=w,shape=w ...] [--validate] [--json]\n"
        "\n"
        "fit moves the body skeleton to follow an EDITED body mesh of the SAME topology (a sculpt /\n"
        "lattice edit of the imported character, or another MetaHuman body of the same LOD); arbitrary\n"
        "topology is refused. --poses reports how the fitted rig deforms in reference poses against the\n"
        "original; --write-dna serialises the result (behaviour data is NOT recalibrated).\n"
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

static int runPose(int argc, char* argv[]);
static int runFace(int argc, char* argv[]);
static int runFit(int argc, char* argv[]);
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
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("fit"))
            return runFit(argc, argv);
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
            const std::vector<size_t> imported = Fit::importedMeshIndices(full.rig, full.rig.flipV);
            { double tm = 0; for (size_t mi : imported) for (size_t i = 0; i < truth[mi].size(); ++i) tm = std::max(tm, double(std::abs(truth[mi][i] - full.rig.meshes[mi].positions[i])) * 1000.0); report.insert("vsRigTruthMovedMm", tm); }
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

namespace {

// Bind-pose (undeformed) vertex positions of every submesh, in submesh order.

// Merge every submesh of an entity into one surface (positions + triangle indices).
bool surfaceOf(Ogre::Entity* e, std::vector<float>& pos, std::vector<int>& tris, std::vector<int>* vertexGroup = nullptr)
{
    const Ogre::MeshPtr mesh = e->getMesh();
    // body part of each bone of the target skeleton, for the part-restricted surface matching
    std::vector<int> boneGroup;
    if (vertexGroup && e->hasSkeleton()) {
        Ogre::SkeletonInstance* sk = e->getSkeleton();
        std::vector<QString> names(sk->getNumBones());
        std::vector<int> parent(sk->getNumBones(), -1);
        for (unsigned short b = 0; b < sk->getNumBones(); ++b) {
            Ogre::Bone* bone = sk->getBone(b);
            names[bone->getHandle() < names.size() ? bone->getHandle() : b] = QString::fromStdString(bone->getName());
            if (auto* par = dynamic_cast<Ogre::Bone*>(bone->getParent()))
                if (bone->getHandle() < parent.size()) parent[bone->getHandle()] = int(par->getHandle());
        }
        boneGroup = Fit::targetBoneGroups(names, parent);
    }
    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        Ogre::SubMesh* sm = mesh->getSubMesh(i);
        Ogre::VertexData* vd = sm->useSharedVertices ? mesh->sharedVertexData : sm->vertexData;
        if (!vd || !sm->indexData || sm->operationType != Ogre::RenderOperation::OT_TRIANGLE_LIST) continue;
        const Ogre::VertexElement* pe = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        if (!pe) continue;
        const int base = int(pos.size() / 3);
        const size_t groupBase = vertexGroup ? vertexGroup->size() : 0;
        if (vertexGroup) vertexGroup->resize(groupBase + vd->vertexCount, -1);
        if (vertexGroup && !boneGroup.empty()) {
            const auto& assignments = sm->useSharedVertices ? mesh->getBoneAssignments() : sm->getBoneAssignments();
            std::vector<float> bestW(vd->vertexCount, -1.f);
            for (const auto& kv : assignments)
                if (kv.first < vd->vertexCount && kv.second.weight > bestW[kv.first] && kv.second.boneIndex < boneGroup.size()) {
                    bestW[kv.first] = kv.second.weight;
                    (*vertexGroup)[groupBase + kv.first] = boneGroup[kv.second.boneIndex];
                }
        }
        Ogre::HardwareVertexBufferSharedPtr vb = vd->vertexBufferBinding->getBuffer(pe->getSource());
        auto* vbase = static_cast<unsigned char*>(vb->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        // A VertexData / IndexData may select a window inside a shared buffer.
        for (size_t v = 0; v < vd->vertexCount; ++v) {
            float* p = nullptr;
            pe->baseVertexPointerToElement(vbase + (vd->vertexStart + v) * vb->getVertexSize(), &p);
            pos.push_back(p[0]); pos.push_back(p[1]); pos.push_back(p[2]);
        }
        vb->unlock();
        Ogre::HardwareIndexBufferSharedPtr ib = sm->indexData->indexBuffer;
        const size_t n = sm->indexData->indexCount, i0 = sm->indexData->indexStart;
        void* ip = ib->lock(Ogre::HardwareBuffer::HBL_READ_ONLY);
        const bool wide = ib->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;
        for (size_t k = 0; k + 2 < n; k += 3)
            for (int c = 0; c < 3; ++c)
                tris.push_back(base + int(wide ? static_cast<uint32_t*>(ip)[i0 + k + size_t(c)] : static_cast<uint16_t*>(ip)[i0 + k + size_t(c)]));
        ib->unlock();
    }
    return !tris.empty();
}

std::vector<Fit::ImportedSubmesh> bindPositionsOf(Ogre::Entity* e)
{
    std::vector<Fit::ImportedSubmesh> out;
    const Ogre::MeshPtr mesh = e->getMesh();
    for (unsigned short i = 0; i < mesh->getNumSubMeshes(); ++i) {
        Ogre::SubMesh* sm = mesh->getSubMesh(i);
        Ogre::VertexData* vd = sm->useSharedVertices ? mesh->sharedVertexData : sm->vertexData;
        std::vector<float> pos, uvs;
        if (vd) {
            const Ogre::VertexElement* ue = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES, 0);
            if (ue && ue->getType() == Ogre::VET_FLOAT2) {
                Ogre::HardwareVertexBufferSharedPtr ub = vd->vertexBufferBinding->getBuffer(ue->getSource());
                const size_t ustride = ub->getVertexSize();
                auto* ubase = static_cast<unsigned char*>(ub->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                for (size_t v = 0; v < vd->vertexCount; ++v) {
                    float* p = nullptr;
                    ue->baseVertexPointerToElement(ubase + (vd->vertexStart + v) * ustride, &p);
                    uvs.push_back(p[0]); uvs.push_back(p[1]);
                }
                ub->unlock();
            }
            const Ogre::VertexElement* pe = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
            if (pe) {
                Ogre::HardwareVertexBufferSharedPtr vb = vd->vertexBufferBinding->getBuffer(pe->getSource());
                const size_t stride = vb->getVertexSize();
                auto* base = static_cast<unsigned char*>(vb->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
                for (size_t v = 0; v < vd->vertexCount; ++v) {
                    float* p = nullptr;
                    pe->baseVertexPointerToElement(base + (vd->vertexStart + v) * stride, &p);
                    pos.push_back(p[0]); pos.push_back(p[1]); pos.push_back(p[2]);
                }
                vb->unlock();
            }
        }
        out.push_back({std::move(pos), std::move(uvs)});
    }
    return out;
}

QVariantMap poseRows(const Fit::PoseReport& p)
{
    QVariantMap m;
    m.insert("pose", p.name);
    m.insert("jointsPresent", p.joints);
    m.insert("movesMeshByMm", p.maxMovementMm);
    m.insert("stretchDeltaMax", p.stretchMaxDelta);
    m.insert("stretchDeltaP99", p.stretchP99Delta);
    if (p.similarityMaxMm >= 0) {
        m.insert("similarityErrorMaxMm", p.similarityMaxMm);
        m.insert("similarityErrorMeanMm", p.similarityMeanMm);
    }
    return m;
}

}  // namespace

struct FitArgs {
    QString path, targetPath, writeDna, out, referencePath, bakeCorrectives;
    std::vector<double> stiffness;   ///< NRICP schedule override (empty = default)
    QString registerMethod;          ///< surface registration: "" / smooth | nricp
    bool refineSurface = false;      ///< with registerJoints: refine the shape onto the target surface afterwards
    bool poses = false, lods = false, doRegister = false, registerJoints = false, noAlignLimbs = false;
    Ogre::Entity* targetEntity = nullptr;   ///< fit to this live entity instead of importing targetPath
    std::shared_ptr<std::vector<PoseSpace::Set>>* poseSpaceOut = nullptr;   ///< in-editor fits: receives the transferred pose-space sets (fittedRigOut's rig carries their channels)
    QString checkPose;              ///< "joint=axis:deg[,...]": import the fitted body WITH its pose-space correctives, bend it, report what they move
    Rig* fittedRigOut = nullptr;            ///< receives the fitted rig (the limb-aligned base when the fit re-posed it)
    bool headless = true;     ///< CLI process (may init Ogre, nothing to clean up) vs inside the editor (MCP)
    double uniformScale = 0.0;
};

// The fit itself, shared by `qtmesh dna fit` and MCP `fit_dna_body`. Returns 0 on success (report filled);
// otherwise an exit code with the message already passed to `err`.
static int fitCore(const FitArgs& a, QVariantMap& report, const std::function<void(const QString&)>& err)
{
    const QString& path = a.path;
    const QString& targetPath = a.targetPath;
    const QString& writeDna = a.writeDna;
    const QString& out = a.out;
    const bool poses = a.poses, lods = a.lods, doRegister = a.doRegister, registerJoints = a.registerJoints, noAlignLimbs = a.noAlignLimbs;
    const double uniformScale = a.uniformScale;
    if (!available()) {
        err(QStringLiteral("Error: MetaHuman DNA support is not compiled into this build. "
                                                  "Rebuild QtMeshEditor with -DENABLE_OPENRIGLOGIC=ON.\n"));
        return 1;
    }
    const bool needScene = !targetPath.endsWith(QStringLiteral(".dna"), Qt::CaseInsensitive) || !out.isEmpty() || !a.checkPose.isEmpty();
    if (needScene && a.headless && !CLIPipeline::initOgreHeadless()) return 1;
    // Nodes the fit creates (the imported target, the exported fitted character) are removed again when
    // running inside the editor (MCP): the CLI process exits, the editor must not keep them.
    struct NodeCleanup {
        bool active;
        std::set<QString> before;            // scene-node names that existed before
        std::vector<QString> selected;       // the user's selection, restored afterwards (addSceneNode selects)
        explicit NodeCleanup(bool on) : active(on)
        {
            if (!on) return;
            for (auto* n : Manager::getSingleton()->getSceneNodes()) before.insert(QString::fromStdString(n->getName()));
            for (auto* n : SelectionSet::getSingleton()->getNodesSelectionList()) selected.push_back(QString::fromStdString(n->getName()));
        }
        ~NodeCleanup()
        {
            if (!active) return;
            QList<Ogre::SceneNode*> add;
            for (auto* n : Manager::getSingleton()->getSceneNodes()) if (!before.count(QString::fromStdString(n->getName()))) add << n;
            auto* sel = SelectionSet::getSingleton();
            sel->clearList();   // the temporary nodes are about to be destroyed: no dangling selection
            for (auto* n : add) Manager::getSingleton()->destroySceneNode(n);
            sel->clear();
            for (const QString& name : selected)
                for (auto* n : Manager::getSingleton()->getSceneNodes())
                    if (QString::fromStdString(n->getName()) == name) { sel->append(n); break; }
        }
    } cleanup(!a.headless && needScene);

    ReadOptions ro;
    const ReadResult src = read(QFileInfo(path).absoluteFilePath(), ro);
    if (!src.ok) { err(QStringLiteral("Error: %1\n").arg(src.error)); return 1; }

    // ---- the target, as one position array per rig mesh ----
    bool rigChanged = false;
    Rig baseRig = src.rig;   // replaced by the limb-aligned rig when --register-joints re-poses it
    std::vector<std::vector<float>> target;
    double seamMm = 0, matchedFraction = 1.0;
    bool matchedByUv = false, flippedV = false;
    int ambiguous = 0;
    QVariantMap registration;
    std::vector<Fit::JointAnchor> anchorTargets;
    if (targetPath.endsWith(QStringLiteral(".dna"), Qt::CaseInsensitive)) {
        const ReadResult tr = read(QFileInfo(targetPath).absoluteFilePath(), ro);
        if (!tr.ok) { err(QStringLiteral("Error: target: %1\n").arg(tr.error)); return 1; }
        if (tr.rig.meshes.size() != src.rig.meshes.size()) {
            err(QStringLiteral("Error: the target DNA has %1 meshes, the source %2 (a different "
                                                      "topology is not supported)\n").arg(tr.rig.meshes.size()).arg(src.rig.meshes.size()));
            return 1;
        }
        for (const Mesh& m : tr.rig.meshes) target.push_back(m.positions);
    } else {
        Ogre::Entity* te = a.targetEntity;
        if (!te) {
            MeshImporterExporter::clearLastImportError();
            // The target is the entity this import ADDS: inside the editor the scene already holds the
            // user's meshes, and the traversal order is not the import order.
            std::set<Ogre::Entity*> entitiesBefore;
            for (auto* o : Manager::getSingleton()->getEntities())
                if (o && o->getMovableType() == "Entity") entitiesBefore.insert(static_cast<Ogre::Entity*>(o));
            MeshImporterExporter::importer({QFileInfo(targetPath).absoluteFilePath()});
                for (auto* o : Manager::getSingleton()->getEntities())
                if (o && o->getMovableType() == "Entity" && !entitiesBefore.count(static_cast<Ogre::Entity*>(o))) te = static_cast<Ogre::Entity*>(o);
        }
        if (!te) {
            const QString why = MeshImporterExporter::lastImportError();
            err(QStringLiteral("Error: could not import the target %1%2\n")
                                           .arg(targetPath, why.isEmpty() ? QString() : QStringLiteral(": ") + why));
            return 1;
        }
        if (registerJoints) {
            // pair the target's skeleton with the MetaHuman joints and warp the body onto its proportions
            std::vector<std::pair<QString, std::array<double, 3>>> bones;
            if (te->hasSkeleton()) {
                // The BIND pose, read without touching the entity: setBindingPose() would record whatever the
                // character is doing right now (an animation, an edited bone) as its new rest pose — fine for a
                // throw-away imported copy, destructive for a live one. Compose each bone's initial transform
                // down the hierarchy instead.
                Ogre::SkeletonInstance* sk = te->getSkeleton();
                std::map<unsigned short, Ogre::Matrix4> world;
                std::function<const Ogre::Matrix4&(Ogre::Bone*)> bindWorld = [&](Ogre::Bone* bone) -> const Ogre::Matrix4& {
                    auto it = world.find(bone->getHandle());
                    if (it != world.end()) return it->second;
                    Ogre::Matrix4 local;
                    local.makeTransform(bone->getInitialPosition(), bone->getInitialScale(), bone->getInitialOrientation());
                    auto* parent = dynamic_cast<Ogre::Bone*>(bone->getParent());
                    return world[bone->getHandle()] = parent ? Ogre::Matrix4(bindWorld(parent) * local) : local;
                };
                for (unsigned short b = 0; b < sk->getNumBones(); ++b) {
                    Ogre::Bone* bone = sk->getBone(b);
                    const Ogre::Vector3 p = bindWorld(bone).getTrans();
                    bones.push_back({QString::fromStdString(bone->getName()), {double(p.x), double(p.y), double(p.z)}});
                }
            }
            if (bones.empty()) { err(QStringLiteral("Error: --register-joints needs a target with a skeleton\n")); return 1; }
            QStringList unmatched;
            const auto anchors = Fit::matchJointAnchors(bones, &unmatched);
            Fit::JointRegisterResult jr;
            if (a.refineSurface) {
                std::vector<float> tpos;
                std::vector<int> ttri, tgroup;
                if (!surfaceOf(te, tpos, ttri, &tgroup)) { err(QStringLiteral("Error: the target has no triangle surface to refine onto\n")); return 1; }
                jr = Fit::registerJointsThenSurface(src.rig, anchors, tpos, ttri, !noAlignLimbs, 8, 6, &tgroup);
            } else {
                jr = Fit::registerByJoints(src.rig, anchors, !noAlignLimbs);
            }
            if (!jr.ok) { err(QStringLiteral("Error: joint registration: %1\n").arg(jr.error)); return 1; }
            target = jr.positions;
            baseRig = jr.alignedRig;
            rigChanged = jr.limbChainsAligned > 0 || jr.yawDegrees != 0.0;
            registration.insert("yawDegrees", jr.yawDegrees);
            registration.insert("limbChainsAligned", jr.limbChainsAligned);
            registration.insert("limbAngles", jr.limbAngles);
            registration.insert("mode", jr.surfaceRefined ? "joints+surface" : "joints");
            anchorTargets = anchors;
            registration.insert("anchorsUsed", jr.anchorsUsed);
            registration.insert("anchorResidualMm", jr.anchorResidualMm);
            registration.insert("unmatchedJoints", unmatched);
            registration.insert("warnings", jr.warnings);
        } else if (doRegister) {
            std::vector<float> tpos;
            std::vector<int> ttri;
            if (!surfaceOf(te, tpos, ttri)) { err(QStringLiteral("Error: the target has no triangle surface\n")); return 1; }
            Fit::RegisterOptions ropt;
            ropt.stiffness = a.stiffness;
            if (a.registerMethod == QLatin1String("nricp")) ropt.method = Fit::RegisterOptions::Method::Nricp;
            const Fit::RegisterResult rr = Fit::registerToSurface(src.rig, tpos, ttri, ropt);
            if (!rr.ok) { err(QStringLiteral("Error: registration: %1\n").arg(rr.error)); return 1; }
            target = rr.positions;
            registration.insert("alignScale", rr.alignScale);
            registration.insert("templateVertices", rr.templateVertices);
            registration.insert("meanResidualPct", rr.meanResidualPct);
            registration.insert("p95ResidualPct", rr.p95ResidualPct);
            registration.insert("maxResidualPct", rr.maxResidualPct);
            registration.insert("warnings", rr.warnings);
            matchedFraction = 1.0;
        } else {
        const Fit::Correspondence c = Fit::matchImported(src.rig, bindPositionsOf(te));
        if (!c.ok) { err(QStringLiteral("Error: %1\n").arg(c.error)); return 1; }
        target = c.positions;
        seamMm = c.maxSeamSplitMm;
        matchedByUv = c.matchedByUv;
        matchedFraction = c.matchedFraction;
        ambiguous = c.ambiguousVertices;
        flippedV = c.flippedV;
        }
    }

    if (rigChanged && (lods || !writeDna.isEmpty())) {
        err(QStringLiteral("Error: --lods and --write-dna are not supported together with limb pose alignment "
                                                  "(the DNA keeps the original neutral orientations and facing); add --no-align-limbs and use a target that already faces the same way\n"));
        return 2;
    }
    std::vector<std::vector<float>> referencePositions;
    if (!a.referencePath.isEmpty()) {
        // Ground truth for the registration: a mesh that keeps the body's topology/UVs (the same edit before it was
        // re-meshed). How far each body vertex's REGISTERED position is from where the reference puts it, in mm.
        std::set<Ogre::Entity*> before;
        for (auto* o : Manager::getSingleton()->getEntities())
            if (o && o->getMovableType() == "Entity") before.insert(static_cast<Ogre::Entity*>(o));
        MeshImporterExporter::importer({QFileInfo(a.referencePath).absoluteFilePath()});
        Ogre::Entity* re = nullptr;
        for (auto* o : Manager::getSingleton()->getEntities())
            if (o && o->getMovableType() == "Entity" && !before.count(static_cast<Ogre::Entity*>(o))) re = static_cast<Ogre::Entity*>(o);
        if (!re) { err(QStringLiteral("Error: could not import the reference %1\n").arg(a.referencePath)); return 1; }
        const Fit::Correspondence rc = Fit::matchImported(src.rig, bindPositionsOf(re));
        if (!rc.ok) { err(QStringLiteral("Error: reference: %1\n").arg(rc.error)); return 1; }
        auto stats = [&](const std::vector<std::vector<float>>& pos, const char* key) {
            std::vector<double> d;
            for (size_t m = 0; m < pos.size() && m < rc.positions.size(); ++m) {
                if (pos[m].empty() || pos[m].size() != rc.positions[m].size()) continue;
                for (size_t i = 0; i + 2 < pos[m].size(); i += 3) {
                    const double dx = pos[m][i] - rc.positions[m][i], dy = pos[m][i + 1] - rc.positions[m][i + 1], dz = pos[m][i + 2] - rc.positions[m][i + 2];
                    d.push_back(std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0);
                }
            }
            if (d.empty()) return;
            if (std::strcmp(key, "errorVsReference") == 0) {
                // where the error sits: mean per dominant skin joint, worst few
                std::map<QString, std::pair<double, int>> byJoint;
                for (size_t m = 0; m < pos.size() && m < rc.positions.size(); ++m) {
                    if (pos[m].empty() || pos[m].size() != rc.positions[m].size() || m >= src.rig.meshes.size()) continue;
                    const Mesh& mesh = src.rig.meshes[m];
                    for (size_t i = 0; i + 2 < pos[m].size(); i += 3) {
                        const size_t v = i / 3;
                        if (v >= mesh.skin.size() || mesh.skin[v].empty()) continue;
                        uint16_t bj = mesh.skin[v][0].joint; float bw = -1;
                        for (const auto& inf : mesh.skin[v]) if (inf.weight > bw) { bw = inf.weight; bj = inf.joint; }
                        const double dx = pos[m][i] - rc.positions[m][i], dy = pos[m][i + 1] - rc.positions[m][i + 1], dz = pos[m][i + 2] - rc.positions[m][i + 2];
                        auto& e = byJoint[bj < src.rig.joints.size() ? src.rig.joints[bj].name : QString()];
                        e.first += std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0; ++e.second;
                    }
                }
                std::vector<std::pair<double, QString>> rows;
                for (const auto& kv : byJoint) if (kv.second.second >= 50) rows.push_back({kv.second.first / kv.second.second, kv.first});
                std::sort(rows.rbegin(), rows.rend());
                QVariantList worst;
                for (size_t i = 0; i < rows.size() && i < 8; ++i) worst << QVariantMap{{"joint", rows[i].second}, {"meanMm", rows[i].first}};
                registration.insert("worstRegions", worst);
            }
            std::sort(d.begin(), d.end());
            double sum = 0; for (double v : d) sum += v;
            QVariantMap e;
            e.insert("meanMm", sum / double(d.size()));
            e.insert("p95Mm", d[size_t(double(d.size() - 1) * 0.95)]);
            e.insert("maxMm", d.back());
            e.insert("vertices", int(d.size()));
            registration.insert(QString::fromLatin1(key), e);
        };
        std::vector<std::vector<float>> unfitted;
        for (const Mesh& m : src.rig.meshes) unfitted.push_back(m.positions);
        referencePositions = rc.positions;
        stats(target, "errorVsReference");
        stats(unfitted, "unfittedErrorVsReference");
    }
    Fit::Result fr = Fit::fit(baseRig, target);
    if (!fr.ok) { err(QStringLiteral("Error: %1\n").arg(fr.error)); return 1; }
    QVariantMap psdReport;
    if (!a.bakeCorrectives.isEmpty() || a.poseSpaceOut || !a.checkPose.isEmpty()) {
        // Generic pose-space correctives for the FITTED body: bake the original rig's corrective behaviour as
        // per-vertex offsets (cached per DNA), carry them through the limb alignment / facing yaw (per-vertex
        // linear part) and the fit warp (its local linear part), see Psd::transferThroughWarp.
        // an in-editor fit keeps going without correctives when they cannot be built (the fit itself is fine);
        // an explicit --bake-correctives request fails instead
        const bool soft = a.bakeCorrectives.isEmpty();
        auto problem = [&](const QString& m) {
            if (!soft) { err(QStringLiteral("Error: pose-space correctives: %1\n").arg(m)); return 1; }
            fr.warnings << QStringLiteral("Pose-space correctives were not built (%1); the body keeps plain skinning.").arg(m);
            return 0;
        };
        QString evErr;
        auto bev = RigEvaluator::create(QFileInfo(path).absoluteFilePath(), ro, &evErr);
        std::vector<PoseSpace::Set> sets;
        QStringList psdNotes;
        if (!bev) { if (problem(evErr)) return 1; }
        else {
            sets = Psd::cachedBake(QFileInfo(path).absoluteFilePath(), src.rig, *bev, &psdNotes);
            if (sets.empty() && problem(QStringLiteral("this DNA has no correctives to bake"))) return 1;
        }
        if (!sets.empty()) {
        if (rigChanged) Psd::applyVertexLinear(sets, Fit::blendedAlignmentLinear(src.rig, baseRig));
        Psd::transferThroughWarp(sets, baseRig, fr.warp);
        for (auto& st : sets) { QString serr; if (!st.solve(&serr)) { err(QStringLiteral("Error: transferred correctives are degenerate: %1\n").arg(serr)); return 1; } }
        QVariantMap cv;
        cv.insert("sets", int(sets.size()));
        cv.insert("notes", psdNotes);
        if (!a.bakeCorrectives.isEmpty()) {
            QSaveFile f(a.bakeCorrectives);
            bool wrote = false;
            if (f.open(QIODevice::WriteOnly)) {
                if (a.bakeCorrectives.endsWith(QLatin1String(".qpsd"), Qt::CaseInsensitive)) {
                    wrote = PoseSpace::writeBinary(&f, sets);
                } else {
                    QJsonArray arr;
                    for (const auto& st : sets) arr.append(st.toJson());
                    QJsonObject doc;
                    doc.insert(QStringLiteral("schema"), QStringLiteral("qtmesh-pose-correctives-collection-v1"));
                    doc.insert(QStringLiteral("sets"), arr);
                    wrote = f.write(QJsonDocument(doc).toJson(QJsonDocument::Compact)) >= 0;
                }
                wrote = wrote && f.commit();
            }
            if (!wrote) { err(QStringLiteral("Error: cannot write %1\n").arg(a.bakeCorrectives)); return 1; }
            cv.insert("file", QFileInfo(a.bakeCorrectives).absoluteFilePath());
        }
        if (!a.checkPose.isEmpty()) {
            // The editor's commit path, headless: import the fitted body with the transferred sets (poses attached to
            // the mesh, weights driven by DnaCorrectiveManager), bend it, and measure what the correctives move.
            Rig withChannels = fr.fitted;
            Psd::appendAsBlendShapes(withChannels, sets);
            QString ierr;
            auto shared = std::make_shared<std::vector<PoseSpace::Set>>(sets);
            Ogre::SceneNode* node = importRigToScene(withChannels, QStringLiteral("psdcheck"), nullptr, &ierr, nullptr, shared);
            DnaCorrectiveManager* mgr = DnaCorrectiveManager::peek();
            Ogre::Entity* ent = nullptr;
            if (node && mgr)
                for (auto* obj : Manager::getSingleton()->getEntities())
                    if (obj && obj->getMovableType() == "Entity" && obj->getName() == node->getName()) ent = static_cast<Ogre::Entity*>(obj);
            if (!ent || !mgr->isAttached(QString::fromStdString(node->getName()))) {
                err(QStringLiteral("Error: --check-pose: could not build the fitted body with its correctives%1\n").arg(ierr.isEmpty() ? QString() : QStringLiteral(": ") + ierr));
                return 1;
            }
            const QString en = QString::fromStdString(node->getName());
            for (const QString& kv : a.checkPose.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                const int eq = kv.indexOf(QLatin1Char('='));
                std::array<double, 4> q;
                if (eq <= 0 || !parseRotation(kv.mid(eq + 1), &q) || !mgr->setPoseOverride(en, kv.left(eq), q)) {
                    err(QStringLiteral("Error: --check-pose: bad entry '%1' (joint=axis:deg)\n").arg(kv));
                    return 1;
                }
            }
            mgr->setEnabled(false);
            mgr->evaluateNow();
            const std::vector<float> plain = DnaCorrectiveManager::skinnedPositions(ent);
            mgr->setEnabled(true);
            mgr->evaluateNow();
            const std::vector<float> corrected = DnaCorrectiveManager::skinnedPositions(ent);
            double mx = 0, sum = 0; size_t n = 0;
            for (size_t i = 0; i + 2 < plain.size() && i + 2 < corrected.size(); i += 3, ++n) {
                const double d = std::sqrt(std::pow(double(corrected[i]) - plain[i], 2) + std::pow(double(corrected[i + 1]) - plain[i + 1], 2) + std::pow(double(corrected[i + 2]) - plain[i + 2], 2)) * 1000.0;
                mx = std::max(mx, d); sum += d;
            }
            cv.insert("checkPoseMaxDisplacementMm", mx);
            cv.insert("checkPoseMeanDisplacementMm", n ? sum / double(n) : 0.0);
            cv.insert("checkPoseActiveSamples", mgr->statsMap(en).value("poseSpaceActiveSamples"));
        }
        psdReport = cv;
        if (a.poseSpaceOut) {
            // the fitted rig carries the samples as blend-shape channels (the importer turns them into poses on the mesh)
            Psd::appendAsBlendShapes(fr.fitted, sets);
            *a.poseSpaceOut = std::make_shared<std::vector<PoseSpace::Set>>(std::move(sets));
        }
        }
    }
    if (a.fittedRigOut) *a.fittedRigOut = fr.fitted;
    if (!anchorTargets.empty()) {
        // After the whole pipeline, where do the fitted joints sit relative to the target's own bones?
        const auto jp = Fit::jointWorldPositions(fr.fitted);
        QVariantList rows;
        double sum = 0, worst = 0;
        for (const Fit::JointAnchor& an : anchorTargets)
            for (size_t j = 0; j < fr.fitted.joints.size(); ++j)
                if (fr.fitted.joints[j].name == an.bodyJoint) {
                    const double dx = jp[j][0] - an.target[0], dy = jp[j][1] - an.target[1], dz = jp[j][2] - an.target[2];
                    const double d = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0;
                    sum += d; worst = std::max(worst, d);
                    rows << QVariantMap{{"joint", an.bodyJoint}, {"errMm", d}, {"dxMm", dx * 1000.0}, {"dyMm", dy * 1000.0}, {"dzMm", dz * 1000.0}};
                }
        // Alignment of the fitted body against the target's skeleton, in degrees: the yaw (about the vertical axis) and
        // roll (in the frontal plane) of the shoulder line and the hip line, and the tilt of the spine. A tilted body
        // shows up here as numbers instead of as an impression from a screenshot.
        {
            auto pos = [&](const QString& n, std::array<double, 3>& p) { for (size_t j = 0; j < fr.fitted.joints.size(); ++j) if (fr.fitted.joints[j].name == n) { p = jp[j]; return true; } return false; };
            auto tgt = [&](const QString& n, std::array<double, 3>& p) { for (const Fit::JointAnchor& an : anchorTargets) if (an.bodyJoint == n) { p = an.target; return true; } return false; };
            auto angle2 = [](double ax, double ay, double bx, double by) {   // signed angle a -> b in degrees
                return std::atan2(ax * by - ay * bx, ax * bx + ay * by) * 180.0 / 3.14159265358979;
            };
            auto lineAngles = [&](const char* a, const char* b, const char* key) {
                std::array<double, 3> fa, fb, ta, tb;
                if (!pos(a, fa) || !pos(b, fb) || !tgt(a, ta) || !tgt(b, tb)) return;
                const double fx = fb[0] - fa[0], fy = fb[1] - fa[1], fz = fb[2] - fa[2], tx = tb[0] - ta[0], ty = tb[1] - ta[1], tz = tb[2] - ta[2];
                registration.insert(QString::fromLatin1(key) + "YawDeg", angle2(tx, tz, fx, fz));
                registration.insert(QString::fromLatin1(key) + "RollDeg", angle2(tx, ty, fx, fy));
            };
            lineAngles("upperarm_l", "upperarm_r", "shoulders");
            lineAngles("thigh_l", "thigh_r", "hips");
            std::array<double, 3> fp, fn, tp, tn2;
            if (pos("pelvis", fp) && pos("neck_01", fn) && tgt("pelvis", tp) && tgt("neck_01", tn2)) {
                const double fx = fn[0] - fp[0], fy = fn[1] - fp[1], fz = fn[2] - fp[2], tx = tn2[0] - tp[0], ty = tn2[1] - tp[1], tz = tn2[2] - tp[2];
                const double dot = fx * tx + fy * ty + fz * tz, nf = std::sqrt(fx * fx + fy * fy + fz * fz), nt = std::sqrt(tx * tx + ty * ty + tz * tz);
                if (nf > 1e-9 && nt > 1e-9) registration.insert("spineTiltDeg", std::acos(std::max(-1.0, std::min(1.0, dot / (nf * nt)))) * 180.0 / 3.14159265358979);
            }
        }
        registration.insert("finalJointVsTargetBones", rows);
        registration.insert("finalJointVsTargetBonesMeanMm", rows.isEmpty() ? 0.0 : sum / double(rows.size()));
        registration.insert("finalJointVsTargetBonesMaxMm", worst);
    }
    if (!referencePositions.empty() && !rigChanged) {
        // The fit the reference itself would give (same-topology, exact) vs the fit from the registered wrap:
        // how far each JOINT lands from where the true edit puts it.
        const Fit::Result truth = Fit::fit(baseRig, referencePositions);
        if (truth.ok) {
            const auto jp = Fit::jointWorldPositions(fr.fitted), jt = Fit::jointWorldPositions(truth.fitted);
            std::vector<double> d;
            for (size_t j = 0; j < jp.size() && j < jt.size(); ++j) {
                const double dx = jp[j][0] - jt[j][0], dy = jp[j][1] - jt[j][1], dz = jp[j][2] - jt[j][2];
                d.push_back(std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0);
            }
            std::sort(d.begin(), d.end());
            double sum = 0; for (double v : d) sum += v;
            QVariantMap e;
            e.insert("meanMm", sum / double(d.size()));
            e.insert("p95Mm", d[size_t(double(d.size() - 1) * 0.95)]);
            e.insert("maxMm", d.back());
            registration.insert("jointErrorVsReference", e);
            const auto j0 = Fit::jointWorldPositions(baseRig);
            double s0 = 0, m0 = 0;
            for (size_t j = 0; j < j0.size() && j < jt.size(); ++j) {
                const double dx = j0[j][0] - jt[j][0], dy = j0[j][1] - jt[j][1], dz = j0[j][2] - jt[j][2];
                const double e0 = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0;
                s0 += e0; m0 = std::max(m0, e0);
            }
            registration.insert("unfittedJointErrorMeanMm", s0 / double(j0.size()));
            registration.insert("unfittedJointErrorMaxMm", m0);
        }
    }

    report.clear();
    if (!psdReport.isEmpty()) report.insert("bakedCorrectives", psdReport);
    report.insert("verticesMatched", fr.verticesMatched);
    report.insert("controlPoints", fr.controlPoints);
    report.insert("meanVertexShiftMm", fr.meanVertexShiftMm);
    report.insert("maxVertexShiftMm", fr.maxVertexShiftMm);
    report.insert("meanJointShiftMm", fr.meanJointShiftMm);
    report.insert("maxJointShiftMm", fr.maxJointShiftMm);
    report.insert("warpResidualMm", fr.warpResidualMm);
    report.insert("seamSplitMaxMm", seamMm);
    report.insert("matchedByUv", matchedByUv);
    report.insert("matchedFraction", matchedFraction);
    report.insert("ambiguousVertices", ambiguous);
    report.insert("targetVFlipped", flippedV);
    if (doRegister || registerJoints) report.insert("registration", registration);
    report.insert("joints", int(fr.fitted.joints.size()));
    report.insert("warnings", fr.warnings);

    if (poses) {
        QVariantList rows;
        for (const Fit::PoseReport& p : Fit::evaluatePoses(baseRig, fr.fitted, Fit::referencePoses(), uniformScale))
            rows << poseRows(p);
        report.insert("poses", rows);
    }
    if (lods) {
        QVariantList rows;
        for (int l = 1; l < src.rig.descriptor.lodCount; ++l) {
            ReadOptions lo = ro;
            lo.lod = l;
            const ReadResult lr = read(QFileInfo(path).absoluteFilePath(), lo);
            if (!lr.ok) continue;
            const Fit::LodPropagation lp = Fit::evaluateLodPropagation(src.rig, fr.fitted, lr.rig, fr.warp);
            QVariantMap row;
            row.insert("lod", l);
            row.insert("vertices", lp.vertices);
            row.insert("meanGapMm", lp.meanGapMm);
            row.insert("maxGapMm", lp.maxGapMm);
            row.insert("baselineMeanGapMm", lp.baselineMeanGapMm);
            rows << row;
        }
        report.insert("lodPropagation", rows);
    }

    if (!writeDna.isEmpty()) {
        const Fit::WriteResult wr = Fit::writeFittedDna(QFileInfo(path).absoluteFilePath(), src.rig, fr.fitted, fr.warp,
                                                        QFileInfo(writeDna).absoluteFilePath());
        if (!wr.ok) { err(QStringLiteral("Error: %1\n").arg(wr.error)); return 1; }
        // Read it back: serialisation is only trustworthy if it round-trips.
        const ReadResult rb = read(QFileInfo(writeDna).absoluteFilePath(), ro);
        QVariantMap v;
        v.insert("file", QFileInfo(writeDna).absoluteFilePath());
        v.insert("meshesWritten", wr.meshesWritten);
        v.insert("lodMeshesWarped", wr.lodMeshesWarped);
        v.insert("readsBack", rb.ok);
        if (rb.ok && rb.rig.meshes.size() == fr.fitted.meshes.size()) {
            double pm = 0, jm = 0;
            for (size_t m = 0; m < rb.rig.meshes.size(); ++m)
                for (size_t i = 0; i < rb.rig.meshes[m].positions.size() && i < fr.fitted.meshes[m].positions.size(); ++i)
                    pm = std::max(pm, double(std::abs(rb.rig.meshes[m].positions[i] - fr.fitted.meshes[m].positions[i])));
            for (size_t j = 0; j < rb.rig.joints.size(); ++j)
                for (int k = 0; k < 3; ++k)
                    jm = std::max(jm, std::abs(rb.rig.joints[j].translation[size_t(k)] - fr.fitted.joints[j].translation[size_t(k)]));
            v.insert("roundTripMaxPositionMm", pm * 1000.0);
            v.insert("roundTripMaxJointMm", jm * 1000.0);
        }
        QString evErr;
        auto ev = RigEvaluator::create(QFileInfo(writeDna).absoluteFilePath(), ro, &evErr);
        v.insert("rigLogicLoads", ev != nullptr);
        if (!ev) v.insert("rigLogicError", evErr);
        v.insert("notes", wr.notes);
        report.insert("writtenDna", v);
        if (!rb.ok || !ev) {
            // an output that cannot be read back / evaluated is not a result: do not leave it behind
            QFile::remove(QFileInfo(writeDna).absoluteFilePath());
            err(QStringLiteral("Error: the written DNA failed validation (%1) and was removed\n")
                                           .arg(!rb.ok ? QStringLiteral("it does not read back") : QStringLiteral("RigLogic cannot load it: %1").arg(evErr)));
            return 1;
        }
    }

    if (!out.isEmpty()) {
        QString ierr;
        Ogre::SceneNode* node = importRigToScene(fr.fitted, QFileInfo(out).completeBaseName() + QStringLiteral("_fitted"), nullptr, &ierr);
        if (!node || MeshImporterExporter::exporter(node, QFileInfo(out).absoluteFilePath(), CLIPipeline::formatForExtension(out)) != 0) {
            err(QStringLiteral("Error: could not export the fitted character%1\n")
                                           .arg(ierr.isEmpty() ? QString() : QStringLiteral(": ") + ierr));
            return 1;
        }
        report.insert("output", QFileInfo(out).absoluteFilePath());
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

static int runFit(int argc, char* argv[])
{
    QString path, targetPath, writeDna, out, referencePath, bakeCorrectives, checkPose;
    std::vector<double> stiffness;
    QString registerMethod;   // "" / smooth (default) | nricp
    bool refineSurface = false;
    bool json = false, poses = false, lods = false, doRegister = false, registerJoints = false, noAlignLimbs = false;
    double uniformScale = 0.0;
    bool sawDna = false, sawFit = false;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--cli" || a == "--verbose" || a == "--no-telemetry") continue;
        if (!sawDna && a == "dna") { sawDna = true; continue; }
        if (!sawFit && a == "fit") { sawFit = true; continue; }
        if (a == "--json") json = true;
        else if (a == "--poses") poses = true;
        else if (a == "--lods") lods = true;
        else if (a == "--register") doRegister = true;
        else if (a == "--refine-surface") refineSurface = true;
        else if (a == "--register-method" && i + 1 < argc) {
            registerMethod = QString::fromLocal8Bit(argv[++i]).toLower();
            if (registerMethod != QLatin1String("smooth") && registerMethod != QLatin1String("nricp")) {
                CLIPipeline::writeCliError(QStringLiteral("Error: --register-method expects smooth or nricp\n"));
                return 2;
            }
        }
        else if (a == "--stiffness" && i + 1 < argc) {
            for (const QString& t : QString::fromLocal8Bit(argv[++i]).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                bool ok = false;
                const double v = t.toDouble(&ok);
                if (!ok || !std::isfinite(v) || v <= 0) {
                    CLIPipeline::writeCliError(QStringLiteral("Error: --stiffness expects positive numbers, e.g. 50,20,8 (got '%1')\n").arg(t));
                    return 2;
                }
                stiffness.push_back(v);
            }
        }
        else if (a == "--reference" && i + 1 < argc) referencePath = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--register-joints") registerJoints = true;
        else if (a == "--no-align-limbs") noAlignLimbs = true;
        else if (a == "--target" && i + 1 < argc) targetPath = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--write-dna" && i + 1 < argc) writeDna = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--bake-correctives" && i + 1 < argc) bakeCorrectives = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--check-pose" && i + 1 < argc) checkPose = QString::fromLocal8Bit(argv[++i]);
        else if ((a == "-o" || a == "--output") && i + 1 < argc) out = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--uniform-scale" && i + 1 < argc) {
            bool ok = false;
            uniformScale = QString::fromLocal8Bit(argv[++i]).toDouble(&ok);
            if (!ok || !std::isfinite(uniformScale) || uniformScale <= 0) {
                CLIPipeline::writeCliError(QStringLiteral("Error: --uniform-scale expects a positive number\n"));
                return 2;
            }
        }
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.startsWith('-') && path.isEmpty()) path = a;
        else { CLIPipeline::writeCliError(QStringLiteral("Error: unknown argument %1\n").arg(a)); return 2; }
    }
    if (path.isEmpty() || targetPath.isEmpty()) { usage(); return 2; }
    if (doRegister && registerJoints) { CLIPipeline::writeCliError(QStringLiteral("Error: --register and --register-joints are alternatives; pass one\n")); return 2; }
    if ((doRegister || registerJoints || noAlignLimbs) && targetPath.endsWith(QStringLiteral(".dna"), Qt::CaseInsensitive)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: --register / --register-joints / --no-align-limbs apply to a mesh target with its own skeleton; a .dna target has the same topology and needs none\n"));
        return 2;
    }
    if (!stiffness.empty() && !(doRegister && registerMethod == QLatin1String("nricp"))) {
        CLIPipeline::writeCliError(QStringLiteral("Error: --stiffness is the NRICP schedule: use it with --register --register-method nricp\n"));
        return 2;
    }
    if (!registerMethod.isEmpty() && !doRegister) {
        CLIPipeline::writeCliError(QStringLiteral("Error: --register-method only applies with --register\n"));
        return 2;
    }
    if (refineSurface && !registerJoints) { CLIPipeline::writeCliError(QStringLiteral("Error: --refine-surface only applies with --register-joints\n")); return 2; }
    if (noAlignLimbs && !registerJoints) { CLIPipeline::writeCliError(QStringLiteral("Error: --no-align-limbs only applies with --register-joints\n")); return 2; }
    FitArgs fa;
    fa.path = path; fa.targetPath = targetPath; fa.writeDna = writeDna; fa.out = out;
    fa.poses = poses; fa.lods = lods; fa.doRegister = doRegister; fa.registerJoints = registerJoints;
    fa.bakeCorrectives = bakeCorrectives;
    fa.checkPose = checkPose;
    fa.noAlignLimbs = noAlignLimbs; fa.uniformScale = uniformScale; fa.headless = true; fa.referencePath = referencePath; fa.stiffness = stiffness; fa.registerMethod = registerMethod; fa.refineSurface = refineSurface;
    QVariantMap report;
    const int rc = fitCore(fa, report, [](const QString& m) { CLIPipeline::writeCliError(m); });
    if (rc != 0) return rc;
    if (json) {
        CLIPipeline::writeOutput(QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(report)).toJson(QJsonDocument::Indented)));
    } else {
        QString t = QStringLiteral("Fitted %1 joints to %2 matched vertices (%3 RBF control points).\n"
                                   "  mesh moved by mean %4 mm / max %5 mm; joints moved by mean %6 mm / max %7 mm; "
                                   "warp misses a vertex by at most %8 mm\n")
                        .arg(report["joints"].toInt()).arg(report["verticesMatched"].toInt()).arg(report["controlPoints"].toInt())
                        .arg(report["meanVertexShiftMm"].toDouble(), 0, 'f', 1).arg(report["maxVertexShiftMm"].toDouble(), 0, 'f', 1)
                        .arg(report["meanJointShiftMm"].toDouble(), 0, 'f', 1).arg(report["maxJointShiftMm"].toDouble(), 0, 'f', 1).arg(report["warpResidualMm"].toDouble(), 0, 'f', 2);
        for (const QVariant& v : report.value("poses").toList()) {
            const QVariantMap m = v.toMap();
            t += QStringLiteral("  pose %1: stretch delta max %2 (p99 %3)").arg(m["pose"].toString(), -24)
                     .arg(m["stretchDeltaMax"].toDouble(), 0, 'f', 3).arg(m["stretchDeltaP99"].toDouble(), 0, 'f', 3);
            if (m.contains("similarityErrorMaxMm"))
                t += QStringLiteral("; vs ideal similarity: max %1 mm").arg(m["similarityErrorMaxMm"].toDouble(), 0, 'f', 3);
            if (!m["jointsPresent"].toBool()) t += QStringLiteral(" (a joint of this pose is missing)");
            t += QLatin1Char('\n');
        }
        for (const QVariant& v : report.value("lodPropagation").toList()) {
            const QVariantMap m = v.toMap();
            t += QStringLiteral("  LOD%1: warped vertices sit %2 mm (max %3) from the fitted LOD0 surface; unwarped baseline %4 mm\n")
                     .arg(m["lod"].toInt()).arg(m["meanGapMm"].toDouble(), 0, 'f', 2).arg(m["maxGapMm"].toDouble(), 0, 'f', 2)
                     .arg(m["baselineMeanGapMm"].toDouble(), 0, 'f', 2);
        }
        if (report.contains("writtenDna")) {
            const QVariantMap w = report["writtenDna"].toMap();
            t += QStringLiteral("  wrote %1 (%2 meshes, %3 LOD meshes warped); reads back: %4; round trip max %5 mm / joints %6 mm; "
                                "RigLogic loads: %7\n")
                     .arg(w["file"].toString()).arg(w["meshesWritten"].toInt()).arg(w["lodMeshesWarped"].toInt())
                     .arg(w["readsBack"].toBool() ? "yes" : "NO").arg(w["roundTripMaxPositionMm"].toDouble(), 0, 'f', 4)
                     .arg(w["roundTripMaxJointMm"].toDouble(), 0, 'f', 4).arg(w["rigLogicLoads"].toBool() ? "yes" : "NO");
            for (const QString& n : w["notes"].toStringList()) t += QStringLiteral("  note: %1\n").arg(n);
        }
        for (const QString& w : report.value("warnings").toStringList()) t += QStringLiteral("  warning: %1\n").arg(w);
        if (report.contains("output")) t += QStringLiteral("Wrote %1\n").arg(report["output"].toString());
        CLIPipeline::writeOutput(t);
    }
    return 0;
}

bool fitEntityReport(const QString& dnaPath, Ogre::Entity* target, const FitOptions& options, Rig* fittedRig,
                     QVariantMap* report, QString* error, std::shared_ptr<std::vector<PoseSpace::Set>>* poseSpace)
{
    if (!target) { if (error) *error = QStringLiteral("no target entity"); return false; }
    FitArgs a;
    a.path = dnaPath;
    a.targetEntity = target;
    a.fittedRigOut = fittedRig;
    a.poseSpaceOut = options.poseSpace ? poseSpace : nullptr;
    a.poses = options.poses;
    a.registerJoints = options.mode == QLatin1String("joints") || options.mode == QLatin1String("joints+surface");
    a.refineSurface = options.mode == QLatin1String("joints+surface");
    a.doRegister = options.mode == QLatin1String("surface");
    a.noAlignLimbs = !options.alignLimbs;
    if (!options.alignLimbs && !a.registerJoints) { if (error) *error = QStringLiteral("align_limbs=false only applies with the skeleton modes"); return false; }
    a.headless = false;
    QString msg;
    QVariantMap local;
    const int rc = fitCore(a, local, [&](const QString& m) { msg += m; });
    if (rc != 0) { if (error) *error = msg.trimmed().isEmpty() ? QStringLiteral("the fit failed") : msg.trimmed(); return false; }
    if (report) *report = local;
    return true;
}

bool fitReport(const QString& dnaPath, const QString& targetPath, const FitOptions& options, QVariantMap* report, QString* error)
{
    FitArgs a;
    a.path = dnaPath;
    a.targetPath = targetPath;
    a.writeDna = options.outputDna;
    a.out = options.outputMesh;
    a.bakeCorrectives = options.outputCorrectives;
    a.poses = options.poses;
    a.lods = options.lods;
    a.registerJoints = options.mode == QLatin1String("joints") || options.mode == QLatin1String("joints+surface");
    a.refineSurface = options.mode == QLatin1String("joints+surface");
    a.doRegister = options.mode == QLatin1String("surface");
    a.noAlignLimbs = !options.alignLimbs;
    // the same combinations the CLI refuses
    if ((a.registerJoints || a.doRegister) && targetPath.endsWith(QStringLiteral(".dna"), Qt::CaseInsensitive)) {
        if (error) *error = QStringLiteral("mode '%1' applies to a mesh target with its own skeleton; a .dna target has the same topology — use mode topology").arg(options.mode);
        return false;
    }
    if (!options.alignLimbs && !a.registerJoints) {
        if (error) *error = QStringLiteral("align_limbs=false only applies with mode joints");
        return false;
    }
    a.headless = false;   // inside the editor: Ogre is up, and created nodes are cleaned afterwards
    QString msg;
    QVariantMap local;
    const int rc = fitCore(a, local, [&](const QString& m) { msg += m; });
    if (rc != 0) {
        if (error) *error = msg.trimmed().isEmpty() ? QStringLiteral("the fit failed") : msg.trimmed();
        return false;
    }
    if (report) *report = local;
    return true;
}

}  // namespace DnaCLI
}  // namespace Dna
