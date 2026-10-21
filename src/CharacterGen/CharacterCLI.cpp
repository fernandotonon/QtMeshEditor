#include "CharacterCLI.h"

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

#include <cstdlib>

namespace CharacterGen {
namespace CharacterCLI {

namespace {

void usage()
{
    CLIPipeline::writeCliError(QStringLiteral(
        "Usage: qtmesh character -o <out.glb|fbx|...> [--seed N] [--variety S] [--body 0=1.2,3=-0.5] [--face 0=1,4=-0.8]\n"
        "                        [--skin <#rrggbb|pale|light|tan|brown|dark>] [--eye <#rrggbb|blue|green|brown|hazel|grey>]\n"
        "                        [--facing-z] [--bust] [--assets DIR] [--json]\n"
        "\n"
        "Builds a rigged character: an MHR body (127-joint skeleton, 45 body shape modes) with an ICT-FaceKit head attached as\n"
        "separate parts (100 face identity modes, the 51 ARKit expression shapes as morph targets). --seed picks a random body and\n"
        "face (--variety scales the spread, default 1.0); --body / --face set individual mode coefficients (index=value).\n"
        "--bust keeps only the head and shoulders (previews, thumbnails). Skin and eye colours tint the parts. The character faces -Z like the rest of the editor (--facing-z keeps +Z).\n"
        "Asset bundles are read from --assets, QTMESH_CHARACTER_DIR or <AppData>/ai_models/character (scripts/build-character-assets.py).\n"));
}

bool parseCoefficients(const QString& text, std::vector<float>& out, int limit, QString* error)
{
    for (const QString& item : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList kv = item.split(QLatin1Char('='));
        bool ok1 = false, ok2 = false;
        const int idx = kv.size() == 2 ? kv[0].trimmed().toInt(&ok1) : -1;
        const float v = kv.size() == 2 ? kv[1].trimmed().toFloat(&ok2) : 0.0f;
        if (!ok1 || !ok2 || idx < 0 || idx >= limit || !std::isfinite(v)) {
            if (error) *error = QStringLiteral("bad coefficient '%1' (want index=value, index 0..%2)").arg(item).arg(limit - 1);
            return false;
        }
        if (out.size() < size_t(limit)) out.resize(size_t(limit), 0.0f);
        out[size_t(idx)] = v;
    }
    return true;
}

bool parseColour(const QString& text, const QHash<QString, QColor>& names, Rgb* out)
{
    QColor c = names.value(text.toLower());
    if (!c.isValid()) c = QColor(text);
    if (!c.isValid()) return false;
    *out = {float(c.redF()), float(c.greenF()), float(c.blueF())};
    return true;
}

}  // namespace

QString assetsDir()
{
    const QString env = qEnvironmentVariable("QTMESH_CHARACTER_DIR");
    if (!env.isEmpty()) return env;
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/ai_models/character");
}

int run(int argc, char* argv[])
{
    QString outPath, bodyArg, faceArg, skinArg, eyeArg, assets = assetsDir();
    bool havePlusZ = false, json = false, haveSeed = false, bust = false;
    uint32_t seed = 0;
    float variety = 1.0f;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        auto next = [&]() { return i + 1 < argc ? QString::fromLocal8Bit(argv[++i]) : QString(); };
        if (a == "--cli" || a == "--verbose" || a == "--no-telemetry" || a == "character") continue;
        if (a == "-o" || a == "--output") outPath = next();
        else if (a == "--seed") { seed = next().toUInt(); haveSeed = true; }
        else if (a == "--variety") variety = next().toFloat();
        else if (a == "--body") bodyArg = next();
        else if (a == "--face") faceArg = next();
        else if (a == "--skin") skinArg = next();
        else if (a == "--eye") eyeArg = next();
        else if (a == "--assets") assets = next();
        else if (a == "--facing-z") havePlusZ = true;
        else if (a == "--bust") bust = true;
        else if (a == "--json") json = true;
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { CLIPipeline::writeCliError(QStringLiteral("Error: unknown option '%1'\n").arg(a)); usage(); return 2; }
    }
    if (outPath.isEmpty()) { usage(); return 2; }
    if (!(variety >= 0.0f && variety <= 5.0f)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: --variety must be between 0 and 5.\n"));
        return 2;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"), QStringLiteral("character cli"));

    Assets data;
    QString err;
    if (!loadAssetsFromDir(assets, data, &err)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: the character assets are unavailable (%1): %2\n"
                                                  "Build them with scripts/build-character-assets.py and point --assets or "
                                                  "QTMESH_CHARACTER_DIR at the output.\n").arg(assets, err));
        return 1;
    }

    Params p;
    p.facePlusZ = havePlusZ;
    if (bust) p.bustDepth = 0.30f;
    if (haveSeed) {
        p.bodyShape = randomCoefficients(seed * 2u + 1u, data.mhrShapes, variety, 16);
        p.faceIdentity = randomCoefficients(seed * 2u + 2u, data.ictIdentities, variety, 40);
    }
    if (!parseCoefficients(bodyArg, p.bodyShape, data.mhrShapes, &err) || !parseCoefficients(faceArg, p.faceIdentity, data.ictIdentities, &err)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(err));
        return 2;
    }
    static const QHash<QString, QColor> skinNames = {{"pale", QColor("#f1cdb3")}, {"light", QColor("#e0ac8c")}, {"tan", QColor("#c68c63")},
                                                     {"brown", QColor("#8d5a3b")}, {"dark", QColor("#4f3224")}};
    static const QHash<QString, QColor> eyeNames = {{"blue", QColor("#3f6fa8")}, {"green", QColor("#4c8a5a")}, {"brown", QColor("#5a3a22")},
                                                    {"hazel", QColor("#8a6a2e")}, {"grey", QColor("#7d8a94")}};
    if (!skinArg.isEmpty() && !parseColour(skinArg, skinNames, &p.skin)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: unknown skin colour '%1'.\n").arg(skinArg));
        return 2;
    }
    if (!eyeArg.isEmpty() && !parseColour(eyeArg, eyeNames, &p.eye)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: unknown eye colour '%1'.\n").arg(eyeArg));
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
