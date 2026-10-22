#include "CharacterCLI.h"

#include "CharacterAssets.h"
#include "CharacterModel.h"
#include "CharacterScene.h"
#include "../CLIPipeline.h"
#include "../MeshImporterExporter.h"
#include "../SentryReporter.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <OgreSceneNode.h>

#include <cmath>
#include <cstdlib>

namespace CharacterGen {
namespace CharacterCLI {

namespace {

void usage()
{
    CLIPipeline::writeCliError(QStringLiteral(
        "Usage: qtmesh character -o <out.glb|fbx|...> [--seed N] [--variety S] [--body 0=1.2,3=-0.5] [--face 0=1,4=-0.8]\n"
        "                        [--skin <#rrggbb|pale|light|tan|brown|dark>] [--eye <#rrggbb|blue|green|brown|hazel|grey>]\n"
        "                        [--facing-z] [--bust [--bust-depth M]] [--ict-head] [--closed-eyes] [--assets DIR] [--json]\n"
        "       qtmesh character --download\n"
        "\n"
        "Builds a rigged character: an MHR body (127-joint skeleton, 45 body shape modes) with an ICT-FaceKit head attached as\n"
        "separate parts (100 face identity modes, the 51 ARKit expression shapes as morph targets). --seed picks a random body and\n"
        "face (--variety scales the spread, default 1.0); --body / --face set individual mode coefficients (index=value).\n"
        "--bust keeps only the head and shoulders (previews, thumbnails). Skin and eye colours tint the parts. The character faces -Z like the rest of the editor (--facing-z keeps +Z).\n"
        "Asset bundles are read from --assets, QTMESH_CHARACTER_DIR or <AppData>/ai_models/character (scripts/build-character-assets.py).\n"));
}

}  // namespace

QString assetsDir() { return CharacterAssets::dir(); }

int run(int argc, char* argv[])
{
    QString outPath, bodyArg, faceArg, skinArg, eyeArg, assets = assetsDir();
    bool openEyes = true, ictHead = false, havePlusZ = false, json = false, haveSeed = false, bust = false, download = false, rawRig = false, fullSkeleton = false, noFold = false, keepAxes = false, keepNames = false;
    uint32_t seed = 0;
    float variety = 1.0f, bustDepth = 0.30f;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        bool missing = false;
        // the value of an option; a missing value (end of the arguments, or the next option) is an error, not an empty string
        auto next = [&]() {
            if (i + 1 >= argc || QString::fromLocal8Bit(argv[i + 1]).startsWith(QLatin1String("-")) ) {
                const QString v = i + 1 < argc ? QString::fromLocal8Bit(argv[i + 1]) : QString();
                bool isNumber = false;
                v.toDouble(&isNumber);   // a negative number is a value, not an option
                if (!isNumber) { missing = true; return QString(); }
            }
            return QString::fromLocal8Bit(argv[++i]);
        };
        auto number = [&](bool* ok) {
            const QString v = next();
            double d = v.toDouble(ok);
            if (missing) *ok = false;
            return d;
        };
        if (a == "--cli" || a == "--verbose" || a == "--no-telemetry" || a == "character") continue;
        if (a == "-o" || a == "--output") outPath = next();
        else if (a == "--seed") {
            bool ok = false;
            const double d = number(&ok);
            if (!ok || d < 0 || d > 4294967295.0 || d != std::floor(d)) { CLIPipeline::writeCliError(QStringLiteral("Error: --seed expects an integer between 0 and 4294967295.\n")); return 2; }
            seed = uint32_t(d);
            haveSeed = true;
        }
        else if (a == "--variety") {
            bool ok = false;
            const double d = number(&ok);
            if (!ok) { CLIPipeline::writeCliError(QStringLiteral("Error: --variety expects a number.\n")); return 2; }
            variety = float(d);
        }
        else if (a == "--body") bodyArg = next();
        else if (a == "--face") faceArg = next();
        else if (a == "--ict-head") ictHead = true;
        else if (a == "--closed-eyes") openEyes = false;
        else if (a == "--open-eyes") openEyes = true;
        else if (a == "--skin") skinArg = next();
        else if (a == "--eye") eyeArg = next();
        else if (a == "--assets") assets = next();
        else if (a == "--facing-z") havePlusZ = true;
        else if (a == "--bust") bust = true;
        else if (a == "--raw-rig") rawRig = true;
        else if (a == "--full-skeleton") fullSkeleton = true;
        else if (a == "--no-fold") noFold = true;
        else if (a == "--keep-bone-axes") keepAxes = true;
        else if (a == "--keep-names") keepNames = true;
        else if (a == "--bust-depth") {
            bool ok = false;
            const double d = number(&ok);
            if (!ok || d <= 0) { CLIPipeline::writeCliError(QStringLiteral("Error: --bust-depth expects a positive number of metres.\n")); return 2; }
            bustDepth = float(d);
            bust = true;
        }
        else if (a == "--json") json = true;
        else if (a == "--download") download = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { CLIPipeline::writeCliError(QStringLiteral("Error: unknown option '%1'\n").arg(a)); usage(); return 2; }
        if (missing) { CLIPipeline::writeCliError(QStringLiteral("Error: option '%1' needs a value.\n").arg(a)); return 2; }
    }
    if (download) {
        QString dlErr;
        if (!CharacterAssets::ensureBlocking(&dlErr)) {
            CLIPipeline::writeCliError(QStringLiteral("Error: the character assets could not be fetched: %1\n").arg(dlErr));
            return 1;
        }
        CLIPipeline::writeOutput(QStringLiteral("Character assets are in %1\n").arg(CharacterAssets::dir()));
        if (outPath.isEmpty()) return 0;
    }
    if (outPath.isEmpty()) { usage(); return 2; }
    if (!(variety >= 0.0f && variety <= 5.0f)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: --variety must be between 0 and 5.\n"));
        return 2;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"), QStringLiteral("character cli"));

    if (assets == CharacterAssets::dir()) {   // fetch when missing, and verify the pinned digests of the default bundle every time
        QString dlErr;
        if (!CharacterAssets::ensureBlocking(&dlErr)) {
            CLIPipeline::writeCliError(QStringLiteral("Error: the character assets are not available (%1).\n").arg(dlErr));
            return 1;
        }
    }
    Assets data;
    QString err;
    if (!loadAssetsFromDir(assets, data, &err)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: the character assets are unavailable (%1): %2\n"
                                                  "Build them with scripts/build-character-assets.py and point --assets or "
                                                  "QTMESH_CHARACTER_DIR at the output.\n").arg(assets, err));
        return 1;
    }

    Params p;
    p.openEyes = openEyes;
    p.nativeHead = !ictHead && !data.mhrArkit.empty();   // the MHR head (no seam) whenever the bundle carries its expression data
    p.facePlusZ = havePlusZ;
    if (rawRig) { p.standardSkeleton = false; p.identityBones = false; p.standardNames = false; p.foldHelperJoints = false; }
    if (fullSkeleton) p.standardSkeleton = false;
    if (noFold) p.foldHelperJoints = false;
    if (keepAxes) p.identityBones = false;
    if (keepNames) p.standardNames = false;
    if (bust) p.bustDepth = std::max(0.005f, bustDepth);
    if (haveSeed) {
        p.bodyShape = randomCoefficients(seed * 2u + 1u, data.mhrShapes, variety, 16);
        p.faceIdentity = randomCoefficients(seed * 2u + 2u, data.ictIdentities, variety, 40);
    }
    if (!CharacterGen::parseCoefficients(bodyArg, p.bodyShape, data.mhrShapes, &err) || !CharacterGen::parseCoefficients(faceArg, p.faceIdentity, data.ictIdentities, &err)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(err));
        return 2;
    }
    if (!skinArg.isEmpty() && !parseColour(skinArg, ColourKind::Skin, &p.skin)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: unknown skin colour '%1' (names: %2).\n").arg(skinArg, colourNames(ColourKind::Skin).join(QLatin1Char(' '))));
        return 2;
    }
    if (!eyeArg.isEmpty() && !parseColour(eyeArg, ColourKind::Eye, &p.eye)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: unknown eye colour '%1' (names: %2).\n").arg(eyeArg, colourNames(ColourKind::Eye).join(QLatin1Char(' '))));
        return 2;
    }

    if (!CLIPipeline::initOgreHeadless()) return 1;
    SceneBuild sb;
    Ogre::SceneNode* node = createCharacterInScene(data, p, QStringLiteral("character"), &sb, &err);
    if (!node) {
        CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(err));
        return 1;
    }
    for (const QString& w : sb.build.warnings + sb.import.warnings)
        CLIPipeline::writeCliError(QStringLiteral("Warning: %1\n").arg(w));
    if (MeshImporterExporter::exporter(node, QFileInfo(outPath).absoluteFilePath(), CLIPipeline::formatForExtension(outPath)) != 0) {
        CLIPipeline::writeCliError(QStringLiteral("Error: export to %1 failed.\n").arg(outPath));
        return 1;
    }
    if (json) {
        QJsonObject o;
        o["output"] = outPath;
        o["bodyVertices"] = sb.build.bodyVertices;
        o["bodyTriangles"] = sb.build.bodyTriangles;
        o["headVertices"] = sb.build.headVertices;
        o["headTriangles"] = sb.build.headTriangles;
        o["joints"] = sb.build.joints;
        o["seamTriangles"] = sb.build.seamTriangles;
        o["morphTargets"] = sb.import.morphTargets;
        o["headScale"] = double(sb.build.headScale);
        o["neckCutY"] = double(sb.build.neckCutY);
        o["meshes"] = QJsonArray::fromStringList(sb.build.meshNames);
        CLIPipeline::writeOutput(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
    } else {
        CLIPipeline::writeOutput(QStringLiteral("Character → %1 (%2 joints, %3 body + %4 head triangles, %5 morph targets)\n")
                                  .arg(QFileInfo(outPath).fileName()).arg(sb.build.joints).arg(sb.build.bodyTriangles)
                                  .arg(sb.build.headTriangles).arg(sb.import.morphTargets));
    }
    return 0;
}

}  // namespace CharacterCLI
}  // namespace CharacterGen
