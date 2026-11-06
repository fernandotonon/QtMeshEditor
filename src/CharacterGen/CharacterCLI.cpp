#include "CharacterCLI.h"

#include <algorithm>

#include "CharacterAssets.h"
#include "CharacterModel.h"
#include "HairLibrary.h"
#include "CharacterScene.h"
#include "Clothing.h"
#include "HairTint.h"
#include "PhotoAppearance.h"
#include "../CLIPipeline.h"
#include "../MeshImporterExporter.h"
#include "../SentryReporter.h"
#include "../BodyFromPhoto/SamBodyPredictor.h"

#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QImageReader>
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
        "                        [--skin <#rrggbb|pale|light|tan|brown|dark>] [--eye <#rrggbb|blue|green|brown|hazel|grey|dark>]\n"
        "                        [--facing-z] [--bust [--bust-depth M]] [--ict-head] [--closed-eyes] [--no-skin-texture] [--body-hair 0..1] [--top STYLE] [--bottom STYLE] [--top-colour C] [--bottom-colour C] [--list-clothes] [--no-eyelashes] [--hair STYLE|proc:PRESET|lib:ID [--hair-colour NAME|#rrggbb] [--hair-param k=v] [--hair-fit k=v] [--hair-config F] | --list-hair | --list-hair-presets | --list-hair-library | --list-hair-colours | --import-hair PATH] [--assets DIR] [--json]\n"
        "       qtmesh character --download\n"
        "\n"
        "Builds a rigged character: an MHR body (127-joint skeleton, 45 body shape modes) with an ICT-FaceKit head attached as\n"
        "separate parts (100 face identity modes, the 51 ARKit expression shapes as morph targets). --seed picks a random body and\n"
        "face (--variety scales the spread, default 1.0); --body / --face set individual mode coefficients (index=value).\n"
        "--bust keeps only the head and shoulders (previews, thumbnails). Skin and eye colours tint the parts. The character faces -Z like the rest of the editor (--facing-z keeps +Z).\n"
        "Hair Studio: --hair proc:<preset> (procedural hair cards: --list-hair-presets) or lib:<id> (imported library hairstyle: --list-hair-library),\n"
        "tuned with --hair-param key=value (length, density, volume, curliness, clumping, frizz, parting, layers, taper, ... seed) and --hair-fit key=value\n"
        "(tx ty tz rx ry rz scale snug clearance flip); --hair-config / --save-hair-config read and write the settings as JSON;\n"
        "--import-hair <file|folder> [--hair-author A --hair-license L --hair-attribution T --hair-source URL] registers hairstyles.\n"
        "Asset bundles are read from --assets, QTMESH_CHARACTER_DIR or <AppData>/ai_models/character (scripts/build-character-assets.py).\n"));
}

}  // namespace

QString assetsDir() { return CharacterAssets::dir(); }

int run(int argc, char* argv[])
{
    QString photoArg, outPath, bodyArg, faceArg, skinArg, eyeArg, assets = assetsDir();
    QString hairArg;
    QString topArg;
    QString bottomArg;
    QString topColourArg;
    QString bottomColourArg;
    bool listClothes = false;
    bool eyelashes = true;
    QString hairColourArg;
    bool listHairColours = false;
    bool listHair = false;
    // Hair Studio
    QStringList hairParams, hairFitArgs;
    QString hairConfigArg, saveHairConfigArg, importHairArg, hairAuthor, hairLicense, hairAttribution, hairSource;
    bool listHairPresets = false, listHairLibrary = false;
    float bodyHair = 0.5f;
    bool skinTexture = true, openEyes = true, ictHead = false, havePlusZ = false, json = false, haveSeed = false, bust = false, download = false, rawRig = false, fullSkeleton = false, noFold = false, keepAxes = false, keepNames = false;
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
        else if (a == "--photo") photoArg = next();
        else if (a == "--body") bodyArg = next();
        else if (a == "--face") faceArg = next();
        else if (a == "--ict-head") ictHead = true;
        else if (a == "--no-skin-texture") skinTexture = false;
        else if (a == "--hair") hairArg = next();
        else if (a == "--list-hair") listHair = true;
        else if (a == "--hair-param") hairParams << next();
        else if (a == "--hair-fit") hairFitArgs << next();
        else if (a == "--hair-config") hairConfigArg = next();
        else if (a == "--save-hair-config") saveHairConfigArg = next();
        else if (a == "--import-hair") importHairArg = next();
        else if (a == "--hair-author") hairAuthor = next();
        else if (a == "--hair-license") hairLicense = next();
        else if (a == "--hair-attribution") hairAttribution = next();
        else if (a == "--hair-source") hairSource = next();
        else if (a == "--list-hair-presets") listHairPresets = true;
        else if (a == "--list-hair-library") listHairLibrary = true;
        else if (a == "--no-eyelashes") eyelashes = false;
        else if (a == "--top") topArg = next();
        else if (a == "--bottom") bottomArg = next();
        else if (a == "--top-colour" || a == "--top-color") topColourArg = next();
        else if (a == "--bottom-colour" || a == "--bottom-color") bottomColourArg = next();
        else if (a == "--list-clothes") listClothes = true;
        else if (a == "--hair-colour" || a == "--hair-color") hairColourArg = next();
        else if (a == "--list-hair-colours" || a == "--list-hair-colors") listHairColours = true;
        else if (a == "--body-hair" && i + 1 < argc) bodyHair = std::clamp(QString::fromLocal8Bit(argv[++i]).toFloat(), 0.0f, 1.0f);
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
    if (listClothes) {
        CLIPipeline::writeOutput(QStringLiteral("tops:    %1\nbottoms: %2\ncolours: %3 (or #rrggbb)\n")
                                     .arg(topStyleIds().join(QStringLiteral(", ")), bottomStyleIds().join(QStringLiteral(", ")), clothColourNames().join(QStringLiteral(", "))));
        return 0;
    }
    if (listHairColours) {
        QStringList lines;
        QJsonArray arr;
        for (const HairColourPreset& pr : hairColourPresets()) {
            lines << QStringLiteral("%1  %2").arg(pr.name, -16).arg(hairColourHex(pr.colour));
            arr.append(QJsonObject{{"name", pr.name}, {"label", pr.label}, {"colour", hairColourHex(pr.colour)}});
        }
        CLIPipeline::writeOutput(json ? QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) + QLatin1Char('\n')
                                      : lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
        return 0;
    }
    if (listHairPresets) {
        QStringList lines;
        QJsonArray arr;
        for (const QString& id : Hair::presetIds()) {
            lines << QStringLiteral("proc:%1  %2").arg(id, -24).arg(Hair::presetLabel(id));
            arr.append(QJsonObject{{"key", QStringLiteral("proc:") + id}, {"kind", QStringLiteral("builtin")}, {"label", Hair::presetLabel(id)}});
        }
        for (const QString& n : Hair::Library::presetNames()) {
            lines << QStringLiteral("saved: %1").arg(n);
            arr.append(QJsonObject{{"key", QStringLiteral("proc:") + n}, {"kind", QStringLiteral("saved")}, {"label", n}});
        }
        CLIPipeline::writeOutput(json ? QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) + QLatin1Char('\n') : lines.join(QLatin1Char('\n')) + QLatin1Char('\n'));
        return 0;
    }
    if (listHairLibrary) {
        QStringList lines;
        QJsonArray arr;
        for (const Hair::Library::Meta& m : Hair::Library::list()) {
            lines << QStringLiteral("lib:%1  %2  [%3 %4 %5]  %6 / %7").arg(m.id, -34).arg(m.name, m.length, m.texture, m.gender, m.license.isEmpty() ? QStringLiteral("licence unknown") : m.license, m.author);
            arr.append(m.toJson());
        }
        CLIPipeline::writeOutput(json ? QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)) + QLatin1Char('\n')
                                      : (lines.isEmpty() ? QStringLiteral("The hair library is empty (--import-hair <file|folder>).\n") : lines.join(QLatin1Char('\n')) + QLatin1Char('\n')));
        return 0;
    }
    if (!importHairArg.isEmpty()) {
        Hair::Library::ImportOptions io;
        io.author = hairAuthor; io.license = hairLicense; io.attribution = hairAttribution; io.source = hairSource;
        const bool isDir = QFileInfo(importHairArg).isDir();
        const Hair::Library::ImportResult r = isDir ? Hair::Library::importFolder(importHairArg, io) : Hair::Library::importFile(importHairArg, io);
        if (hairLicense.isEmpty()) CLIPipeline::writeCliError(QStringLiteral("Note: no --hair-license given: check the source's licence before redistributing characters that use these styles.\n"));
        CLIPipeline::writeOutput(QStringLiteral("found %1, added %2, duplicates %3, failed %4\n%5").arg(r.found).arg(r.added).arg(r.duplicates).arg(r.failed).arg(r.ids.isEmpty() ? QString() : r.ids.join(QLatin1Char('\n')) + QLatin1Char('\n')));
        for (const QString& e : r.errors) CLIPipeline::writeCliError(e + QLatin1Char('\n'));
        return r.added + r.duplicates > 0 ? 0 : 1;
    }
    if (listHair) {
        const QStringList names = CharacterAssets::hairStyleNames(assets);
        if (json) {
            CLIPipeline::writeOutput(QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(names)).toJson(QJsonDocument::Compact)) + QLatin1Char('\n'));
            return 0;
        }
        CLIPipeline::writeOutput(names.isEmpty() ? QStringLiteral("No hairstyles (no hair_character.qmcb in %1)\n").arg(assets)
                                                  : names.join(QLatin1Char('\n')) + QLatin1Char('\n'));
        return 0;
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
    p.skinTexture = skinTexture;
    p.bodyHair = bodyHair;
    p.eyelashes = eyelashes;
    if (!topArg.isEmpty() && !parseTopStyle(topArg, nullptr)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: unknown top '%1' (%2).\n").arg(topArg, topStyleIds().join(QStringLiteral(", "))));
        return 2;
    }
    if (!bottomArg.isEmpty() && !parseBottomStyle(bottomArg, nullptr)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: unknown bottom '%1' (%2).\n").arg(bottomArg, bottomStyleIds().join(QStringLiteral(", "))));
        return 2;
    }
    if ((!topColourArg.isEmpty() && !parseClothColour(topColourArg, &p.topColour)) || (!bottomColourArg.isEmpty() && !parseClothColour(bottomColourArg, &p.bottomColour))) {
        CLIPipeline::writeCliError(QStringLiteral("Error: unknown clothes colour (a name from --list-clothes, or #rrggbb).\n"));
        return 2;
    }
    if (!topArg.isEmpty()) p.topStyle = topArg.toLower();
    if (!bottomArg.isEmpty()) p.bottomStyle = bottomArg.toLower();
    p.nativeHead = !ictHead && !data.mhrArkit.empty();   // the MHR head (no seam) whenever the bundle carries its expression data
    Hair::Library::StudioConfig studio;
    if (!hairConfigArg.isEmpty()) {
        if (!Hair::Library::loadConfigFile(hairConfigArg, &studio)) {
            CLIPipeline::writeCliError(QStringLiteral("Error: cannot read the hair config '%1'.\n").arg(hairConfigArg));
            return 2;
        }
        if (hairArg.isEmpty()) hairArg = studio.style;
        p.hairProcedural = studio.procedural;
        p.hairFit = studio.fit;
        if (hairColourArg.isEmpty()) hairColourArg = studio.colour;
    }
    if (hairArg.startsWith(QLatin1String("proc:")) || hairArg.startsWith(QLatin1String("lib:"))) {
        if (!p.nativeHead) {
            CLIPipeline::writeCliError(QStringLiteral("Error: hair needs the MHR head (not --ict-head, and a bundle with its expression data).\n"));
            return 2;
        }
        if (hairArg.startsWith(QLatin1String("proc:"))) {
            const QString id = hairArg.mid(5);
            Hair::Params saved;
            if (Hair::isPreset(id)) { if (hairConfigArg.isEmpty()) p.hairProcedural = Hair::presetParams(id); }
            else if (Hair::Library::loadPreset(id, &saved)) { if (hairConfigArg.isEmpty()) p.hairProcedural = saved; }
            else if (hairConfigArg.isEmpty()) {
                CLIPipeline::writeCliError(QStringLiteral("Error: unknown procedural hairstyle '%1' (see --list-hair-presets).\n").arg(id));
                return 2;
            }
        } else {
            Hair::ImportedHair probe;
            if (!Hair::Library::load(hairArg.mid(4), &probe)) {
                CLIPipeline::writeCliError(QStringLiteral("Error: '%1' is not in the hair library (see --list-hair-library).\n").arg(hairArg.mid(4)));
                return 2;
            }
        }
        for (const QString& kv : hairParams) {
            const int eq = kv.indexOf(QLatin1Char('='));
            bool ok = false;
            const double v = kv.mid(eq + 1).toDouble(&ok);
            if (eq <= 0 || !ok || !Hair::setParam(p.hairProcedural, kv.left(eq), v)) {
                CLIPipeline::writeCliError(QStringLiteral("Error: bad --hair-param '%1' (key=number; keys: length density volume curliness clumping frizz parting layers taper thickness fringe tipCurl gravity hairlineOffset ponytail tieHeight whorl quality seed).\n").arg(kv));
                return 2;
            }
        }
        if (hairArg.startsWith(QLatin1String("proc:")) && !hairFitArgs.isEmpty()) {   // the fit moves IMPORTED hair onto the head; procedural hair is grown on it
            CLIPipeline::writeCliError(QStringLiteral("Error: --hair-fit applies to imported (lib:) hairstyles only; tune procedural hair with --hair-param.\n"));
            return 2;
        }
        QJsonObject fitObj = Hair::toJson(p.hairFit);
        for (const QString& kv : hairFitArgs) {
            const int eq = kv.indexOf(QLatin1Char('='));
            static const QStringList keys{"tx", "ty", "tz", "rx", "ry", "rz", "scale", "snug", "clearance"};
            bool ok = false;
            const double v = kv.mid(eq + 1).toDouble(&ok);
            if (eq > 0 && ok && kv.left(eq) == QLatin1String("flip")) fitObj["flipFront"] = v != 0.0;
            else if (eq > 0 && ok && keys.contains(kv.left(eq))) fitObj[kv.left(eq)] = v;
            else {
                CLIPipeline::writeCliError(QStringLiteral("Error: bad --hair-fit '%1' (keys: tx ty tz rx ry rz scale snug clearance flip).\n").arg(kv));
                return 2;
            }
        }
        p.hairFit = Hair::fitAdjustFromJson(fitObj);
        p.hairStyle = hairArg;
        studio.style = hairArg; studio.procedural = p.hairProcedural; studio.fit = p.hairFit;
        if (!hairColourArg.isEmpty()) {   // the colour is part of the saved config too (--hair-colour, or the one a loaded config carried)
            Rgb resolved;
            if (!parseHairColour(hairColourArg, &resolved)) {
                CLIPipeline::writeCliError(QStringLiteral("Error: unknown hair colour '%1' (a preset: see --list-hair-colours, or #rrggbb).\n").arg(hairColourArg));
                return 2;
            }
            studio.colour = hairColourHex(resolved);
        }
        if (!saveHairConfigArg.isEmpty() && !Hair::Library::saveConfigFile(saveHairConfigArg, studio))
            CLIPipeline::writeCliError(QStringLiteral("Warning: could not write %1.\n").arg(saveHairConfigArg));
    } else if (!hairArg.isEmpty()) {
        if (!p.nativeHead) {
            CLIPipeline::writeCliError(QStringLiteral("Error: hair needs the MHR head (not --ict-head, and a bundle with its expression data).\n"));
            return 2;
        }
        if (!data.hairStyle(hairArg)) {
            CLIPipeline::writeCliError(QStringLiteral("Error: unknown hairstyle '%1' (available: %2).\n").arg(hairArg, data.hairStyleNames().join(QStringLiteral(", "))));
            return 2;
        }
        p.hairStyle = hairArg;
    }
    if (!hairColourArg.isEmpty() && !parseHairColour(hairColourArg, &p.hairColour)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: unknown hair colour '%1' (a preset: see --list-hair-colours, or #rrggbb).\n").arg(hairColourArg));
        return 2;
    }
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
    if (!photoArg.isEmpty()) {
        // Photo -> MHR identity (SAM 3D Body): its 45 shape coefficients are this generator's body-shape modes. --body still overrides.
        QImageReader reader(photoArg);
        reader.setAutoTransform(true);
        const QImage photo = reader.read();
        if (photo.isNull()) { CLIPipeline::writeCliError(QStringLiteral("Error: cannot read the image %1\n").arg(photoArg)); return 1; }
        QString fetchErr;
        if (!SamBody::ensureModelsBlocking(&fetchErr)) { CLIPipeline::writeCliError(QStringLiteral("Error: SAM 3D Body models unavailable: %1\n").arg(fetchErr)); return 1; }
        SamBody::Options so;   // the neutral mesh + topology are needed for the clothing regions
        const SamBody::Result sr = SamBody::predict(photo, so);
        if (!sr.ok) { CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(sr.error)); return 1; }
        if (!sr.warning.isEmpty()) CLIPipeline::writeCliError(QStringLiteral("Warning: %1\n").arg(sr.warning));
        // Appearance from the same photo (skin tone, hair colour, iris colour); explicit --skin/--eye/--hair-colour still win.
        if (qEnvironmentVariableIsSet("QTMESH_PHOTO_DEBUG_DIR")) {
            float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f, mnz = 1e9f, mxz = -1e9f;
            for (size_t i = 0; i + 2 < sr.body.posedVertices.size(); i += 3) {
                mnx = std::min(mnx, sr.body.posedVertices[i]); mxx = std::max(mxx, sr.body.posedVertices[i]);
                mny = std::min(mny, sr.body.posedVertices[i + 1]); mxy = std::max(mxy, sr.body.posedVertices[i + 1]);
                mnz = std::min(mnz, sr.body.posedVertices[i + 2]); mxz = std::max(mxz, sr.body.posedVertices[i + 2]);
            }
            fprintf(stderr, "sam: camT %.3f %.3f %.3f focal %.1f photo %dx%d verts x[%.2f %.2f] y[%.2f %.2f] z[%.2f %.2f]\n", double(sr.body.camT[0]), double(sr.body.camT[1]), double(sr.body.camT[2]),
                    double(sr.body.focalPx), photo.width(), photo.height(), double(mnx), double(mxx), double(mny), double(mxy), double(mnz), double(mxz));
            QImage dbg = photo.convertToFormat(QImage::Format_RGB888);
            QPainter pt(&dbg);
            const QRectF hb = PhotoAppearance::headBoxFromBody(sr.body.posedVertices, sr.body.camT, sr.body.focalPx, photo.width(), photo.height());
            pt.setPen(QPen(Qt::red, 4)); pt.drawRect(hb);
            pt.setPen(QPen(Qt::green, 4)); pt.drawRect(sr.bboxUsed);
            pt.end();
            dbg.save(QDir(qEnvironmentVariable("QTMESH_PHOTO_DEBUG_DIR")).filePath(QFileInfo(photoArg).baseName() + QStringLiteral("_head.png")));
        }
        QImage matte;
        const PhotoAppearance::FaceAppearance fa = PhotoAppearance::estimate(photo, sr.bboxUsed,
            PhotoAppearance::headBoxFromBody(sr.body.posedVertices, sr.body.camT, sr.body.focalPx, photo.width(), photo.height()), &matte);
        auto hex = [](const PhotoAppearance::ColourEstimate& c) { return QColor::fromRgbF(double(c.colour[0]), double(c.colour[1]), double(c.colour[2])).name(); };
        if (fa.skin.ok) { p.skin = fa.skin.colour; }
        if (fa.hair.ok) {
            p.hairColour = fa.hair.colour;
            if (hairArg.isEmpty() && p.nativeHead) {
                const char* const styles[] = {"", "short02", "bob01", "long01"};
                if (data.hairStyle(QString::fromLatin1(styles[fa.hairLength]))) { hairArg = QString::fromLatin1(styles[fa.hairLength]); p.hairStyle = hairArg; }
            }
        }
        if (fa.iris.ok) { p.eye = fa.iris.colour; }
        CLIPipeline::writeCliError(QStringLiteral("Photo appearance: face %1, skin %2 (%3 px), hair %4 (%5 px, length %6, side %7), iris %8 %9\n")
            .arg(fa.faceFound ? QStringLiteral("found") : QStringLiteral("NOT found"), fa.skin.ok ? hex(fa.skin) : QStringLiteral("-")).arg(fa.skin.samples)
            .arg(fa.hair.ok ? hex(fa.hair) : QStringLiteral("-")).arg(fa.hair.samples).arg(fa.hairLength).arg(double(fa.sideHairFraction), 0, 'f', 2)
            .arg(fa.iris.ok ? hex(fa.iris) : QStringLiteral("-"), fa.note));
        if (topArg.isEmpty() && bottomArg.isEmpty() && fa.skin.ok) {
            const PhotoAppearance::ClothingEstimate ce = PhotoAppearance::estimateClothing(photo, matte, p.skin, sr.body.neutralVertices, sr.body.posedVertices,
                                                                                               sr.body.triangles, sr.body.camT, sr.body.focalPx);
            if (ce.ok) {
                p.topStyle = ce.top; p.bottomStyle = ce.bottom;
                if (ce.topColour.ok) p.topColour = ce.topColour.colour;
                if (ce.bottomColour.ok) p.bottomColour = ce.bottomColour.colour;
                CLIPipeline::writeCliError(QStringLiteral("Photo clothing: top %1 %2, bottom %3 %4\n").arg(ce.top, ce.topColour.ok ? hex(ce.topColour) : QStringLiteral("-"), ce.bottom, ce.bottomColour.ok ? hex(ce.bottomColour) : QStringLiteral("-")));
            }
        }
        p.bodyShape = sr.body.shape;
        p.bodyShape.resize(size_t(data.mhrShapes), 0.0f);
        p.faceIdentity.clear();
        if (!ictHead) p.nativeHead = !data.mhrArkit.empty();
        CLIPipeline::writeCliError(QStringLiteral("Photo shape coefficients: %1\n").arg([&] { QStringList l; for (float v : p.bodyShape) l << QString::number(double(v), 'f', 3); return l.join(QLatin1Char(',')); }()));
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
    if (p.hairStyle.startsWith(QLatin1String("proc:")) || p.hairStyle.startsWith(QLatin1String("lib:"))) {
        // the Hair Studio settings travel with the file, so the character's hair can be restored and edited later
        Hair::Library::StudioConfig cfg;
        cfg.style = p.hairStyle; cfg.procedural = p.hairProcedural; cfg.fit = p.hairFit; cfg.colour = hairColourHex(p.hairColour);
        const QString sidecar = QFileInfo(outPath).absoluteFilePath() + QStringLiteral(".hair.json");
        if (!Hair::Library::saveConfigFile(sidecar, cfg))
            CLIPipeline::writeCliError(QStringLiteral("Warning: could not write %1.\n").arg(sidecar));
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
