#include "BodyFromPhotoCLI.h"

#include "BodyTexture.h"
#include "BodyTextureTrellis.h"
#include "SamBodyPredictor.h"
#include "../CLIPipeline.h"
#include "../SentryReporter.h"

#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStringList>

namespace SamBody {
namespace BodyCLI {

namespace {

void usage()
{
    CLIPipeline::writeCliError(QStringLiteral(
        "Usage: qtmesh body <photo> -o <body.obj> [--pose neutral|posed] [--bbox x1,y1,x2,y2] [--no-detect]\n"
        "                  [--texture trellis2 [--texture-size N] [--trellis-preset fast|balanced|high]]\n"
        "                  [--facing-z] [--json]\n"
        "       qtmesh body --download\n"
        "\n"
        "Predicts a human body (the MHR / Momentum Human Rig body, 18439 vertices) from ONE photo with\n"
        "SAM 3D Body. --pose neutral (default) zeroes the pose and keeps the predicted shape and\n"
        "proportions (the input `qtmesh dna fit --register` wants); --pose posed keeps the photo's pose\n"
        "(pose correctives are not reproduced: posed vertices are within ~2 mm mean / 25 mm max of the\n"
        "full model). The person is found by background removal unless --bbox is given (pixels).\n"
        "The OBJ is in metres, Y up, facing -Z (the editor's convention); --facing-z keeps the model's own +Z.\n"
        "--texture trellis2 also runs TRELLIS.2 on the photo and bakes its appearance (including the back) onto the\n"
        "body's UV atlas: a .glb output is one self-contained file (open it in the editor); .obj writes\n"
        "<stem>.obj + .mtl + _diffuse.png (+ _roughness/_metallic when TRELLIS baked them; the editor does not bind an OBJ's MTL).\n"
        "Needs the TRELLIS.2 runtime (trellis.cpp or the Python sidecar).\n"
        "Models (~3.6 GB) download on first use; see THIRD_PARTY_AI_MODELS.md for the SAM License.\n"
        "Needs a build with -DENABLE_ONNX=ON.\n"));
}

} // namespace

int run(int argc, char* argv[])
{
    QString photoPath, outPath;
    QString pose = QStringLiteral("neutral");
    QRectF bbox;
    bool autoDetect = true, facingZ = false, json = false, download = false;
    QString textureMode;
    int textureSize = 2048;
    QString trellisPreset = QStringLiteral("balanced");

    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--cli" || a == "--verbose" || a == "--no-telemetry" || a == "body") continue;
        if (a == "-o" || a == "--output") {
            if (i + 1 >= argc) { CLIPipeline::writeCliError(QStringLiteral("Error: -o requires a path\n")); return 2; }
            outPath = QString::fromLocal8Bit(argv[++i]);
        } else if (a == "--pose") {
            if (i + 1 >= argc) { CLIPipeline::writeCliError(QStringLiteral("Error: --pose requires neutral|posed\n")); return 2; }
            pose = QString::fromLocal8Bit(argv[++i]).toLower();
            if (pose != "neutral" && pose != "posed") {
                CLIPipeline::writeCliError(QStringLiteral("Error: --pose must be neutral or posed\n")); return 2;
            }
        } else if (a == "--bbox") {
            if (i + 1 >= argc) { CLIPipeline::writeCliError(QStringLiteral("Error: --bbox requires x1,y1,x2,y2\n")); return 2; }
            const QStringList p = QString::fromLocal8Bit(argv[++i]).split(',');
            bool ok = p.size() == 4;
            double v[4] = {0, 0, 0, 0};
            for (int k = 0; ok && k < 4; ++k) v[k] = p[k].trimmed().toDouble(&ok);
            if (!ok || v[2] <= v[0] || v[3] <= v[1]) {
                CLIPipeline::writeCliError(QStringLiteral("Error: --bbox must be x1,y1,x2,y2 with x2>x1, y2>y1\n")); return 2;
            }
            bbox = QRectF(v[0], v[1], v[2] - v[0], v[3] - v[1]);
        } else if (a == "--texture") {
            if (i + 1 >= argc) { CLIPipeline::writeCliError(QStringLiteral("Error: --texture requires trellis2\n")); return 2; }
            textureMode = QString::fromLocal8Bit(argv[++i]).toLower();
            if (textureMode != "trellis2") { CLIPipeline::writeCliError(QStringLiteral("Error: --texture must be trellis2\n")); return 2; }
        } else if (a == "--texture-size") {
            bool ok = false;
            textureSize = i + 1 < argc ? QString::fromLocal8Bit(argv[++i]).toInt(&ok) : 0;
            if (!ok || textureSize < 256 || textureSize > 4096) { CLIPipeline::writeCliError(QStringLiteral("Error: --texture-size must be 256..4096\n")); return 2; }
        } else if (a == "--trellis-preset") {
            trellisPreset = i + 1 < argc ? QString::fromLocal8Bit(argv[++i]).toLower() : QString();
            if (trellisPreset != "fast" && trellisPreset != "balanced" && trellisPreset != "high") {
                CLIPipeline::writeCliError(QStringLiteral("Error: --trellis-preset must be fast, balanced or high\n")); return 2;
            }
        } else if (a == "--no-detect") autoDetect = false;
        else if (a == "--facing-z") facingZ = true;
        else if (a == "--json") json = true;
        else if (a == "--download") download = true;
        else if (a.startsWith('-')) { CLIPipeline::writeCliError(QStringLiteral("Error: unknown option %1\n").arg(a)); usage(); return 2; }
        else if (photoPath.isEmpty()) photoPath = a;
        else { CLIPipeline::writeCliError(QStringLiteral("Error: unexpected argument %1\n").arg(a)); usage(); return 2; }
    }

    if (!isAvailable()) {
        CLIPipeline::writeCliError(QStringLiteral("Error: photo to body needs an ONNX build (rebuild with -DENABLE_ONNX=ON)\n"));
        return 1;
    }
    if (download) {
        QString e;
        if (!ensureModelsBlocking(&e)) {
            CLIPipeline::writeCliError(QStringLiteral("Error: could not fetch the SAM 3D Body models: %1\n").arg(e));
            return 1;
        }
        CLIPipeline::writeOutput(QStringLiteral("SAM 3D Body models ready in %1\n").arg(modelDir()));
        return 0;
    }
    if (photoPath.isEmpty() || outPath.isEmpty()) { usage(); return 2; }

    // Honour EXIF orientation (a portrait phone JPEG would otherwise arrive sideways, and --bbox is in displayed pixels).
    QImageReader reader(photoPath);
    reader.setAutoTransform(true);
    QImage photo = reader.read();
    if (photo.isNull()) {
        CLIPipeline::writeCliError(QStringLiteral("Error: cannot read the image %1\n").arg(photoPath));
        return 1;
    }

    SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"), QStringLiteral("qtmesh body (photo to MHR body)"));
    QString fetchErr;
    if (!ensureModelsBlocking(&fetchErr)) {
        CLIPipeline::writeCliError(QStringLiteral("Error: SAM 3D Body models unavailable: %1\n").arg(fetchErr));
        return 1;
    }

    Options opts;
    opts.bbox = bbox;
    opts.autoDetect = autoDetect;
    const Result r = predict(photo, opts);
    if (!r.ok) {
        CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(r.error));
        return 1;
    }
    if (!r.warning.isEmpty()) CLIPipeline::writeCliError(QStringLiteral("Warning: %1\n").arg(r.warning));

    const auto& verts = pose == "posed" ? r.body.posedVertices : r.body.neutralVertices;
    bool textured = false;
    QString textureNote;
    if (textureMode == "trellis2") {
        const UvLayout uv = loadUvLayout(uvPath());
        if (!uv.valid()) {
            CLIPipeline::writeCliError(QStringLiteral("Error: the MHR UV layout (%1) is missing or damaged; run qtmesh body --download\n").arg(uvPath()));
            return 1;
        }
        TexturedBodyOptions to;
        to.textureSize = textureSize;
        to.trellisPreset = trellisPreset;
        const TexturedBodyResult tr = textureBodyFromTrellis(photo, verts, r.body.triangles, uv, to);
        if (!tr.ok) {
            CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(tr.error));
            return 1;
        }
        for (const QString& n : tr.notes) CLIPipeline::writeCliError(QStringLiteral("Note: %1\n").arg(n));
        const QString we = writeTexturedBody(outPath, verts, r.body.triangles, uv, tr.transfer, !facingZ);
        if (!we.isEmpty()) { CLIPipeline::writeCliError(QStringLiteral("Error: %1\n").arg(we)); return 1; }
        textured = true;
        textureNote = QStringLiteral("texture coverage %1%, wrap residual %2% of height")
                          .arg(tr.transfer.coverage * 100.0f, 0, 'f', 1).arg(tr.transfer.meanDistanceFraction * 100.0f, 0, 'f', 2);
    } else {
        const QByteArray obj = toObj(verts, r.body.triangles, !facingZ);
        QSaveFile f(outPath);
        if (!f.open(QIODevice::WriteOnly) || f.write(obj) != obj.size() || !f.commit()) {
            CLIPipeline::writeCliError(QStringLiteral("Error: cannot write %1\n").arg(outPath));
            return 1;
        }
    }

    float ymin = 1e30f, ymax = -1e30f;
    for (size_t i = 1; i < verts.size(); i += 3) { ymin = std::min(ymin, verts[i]); ymax = std::max(ymax, verts[i]); }
    if (json) {
        QJsonObject o;
        o["output"] = outPath;
        o["pose"] = pose;
        o["vertices"] = static_cast<int>(verts.size() / 3);
        o["triangles"] = static_cast<int>(r.body.triangles.size() / 3);
        o["heightMeters"] = ymax - ymin;
        o["bbox"] = QJsonArray{r.bboxUsed.left(), r.bboxUsed.top(), r.bboxUsed.right(), r.bboxUsed.bottom()};
        o["personFoundByMatte"] = r.usedMatte;
        o["textured"] = textured;
        if (textured) o["textureNote"] = textureNote;
        QJsonArray shape; for (float s : r.body.shape) shape.append(s);
        o["shape"] = shape;
        if (!r.warning.isEmpty()) o["warning"] = r.warning;
        CLIPipeline::writeOutput(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
    } else {
        CLIPipeline::writeOutput(QStringLiteral("Wrote %1 (%2 body, %3 vertices, %4 m tall%5)\n")
                                     .arg(outPath, pose).arg(verts.size() / 3).arg(ymax - ymin, 0, 'f', 2)
                                     .arg(textured ? QStringLiteral(", textured: ") + textureNote : QString()));
    }
    return 0;
}

} // namespace BodyCLI
} // namespace SamBody
