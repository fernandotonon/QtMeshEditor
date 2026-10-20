#include "BodyTextureTrellis.h"

#include "../ImageTo3D/Trellis2Predictor.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

namespace SamBody {

TexturedBodyResult textureBodyFromTrellis(const QImage& photo, const std::vector<float>& bodyVertices,
                                          const std::vector<int>& bodyTriangles, const UvLayout& uv,
                                          const TexturedBodyOptions& options)
{
    TexturedBodyResult out;
    if (!Trellis2Predictor::runtimeAvailable()) {
        out.error = QStringLiteral("texturing from TRELLIS.2 needs its runtime: %1").arg(Trellis2Predictor::runtimeDescription());
        return out;
    }
    Trellis2Predictor::Options to;
    to.preset = options.trellisPreset;
    to.bakeTexture = true;
    to.textureSize = std::max(1024, std::min(options.textureSize, 4096));
    to.bakeNormalMap = false;                 // tangent-space normals of the TRELLIS surface mean nothing on the MHR atlas
    const MeshGenPredictor::Result gen = Trellis2Predictor::predict(photo, to, options.progress);
    if (!gen.ok) {
        out.error = QStringLiteral("TRELLIS.2 failed: %1").arg(gen.error);
        return out;
    }
    if (gen.texture.isNull() || gen.uvs.empty()) {
        out.error = QStringLiteral("TRELLIS.2 produced no baked texture%1")
                        .arg(gen.warning.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(gen.warning));
        return out;
    }
    if (!gen.warning.isEmpty()) out.notes << gen.warning;

    TexturedSurface src;
    src.positions = gen.positions;
    src.indices = gen.indices;
    src.uvs = gen.uvs;
    src.albedo = gen.texture;
    if (options.bakePbr) { src.roughness = gen.roughnessMap; src.metallic = gen.metallicMap; }

    TransferOptions topt = options.transfer;
    topt.textureSize = options.textureSize;
    out.transfer = transferTexture(src, bodyVertices, bodyTriangles, uv, topt);
    if (!out.transfer.ok) { out.error = out.transfer.error; return out; }
    out.notes << out.transfer.notes;
    out.ok = true;
    return out;
}

QString writeTexturedBody(const QString& objPath, const std::vector<float>& vertices, const std::vector<int>& triangles,
                          const UvLayout& uv, const TransferResult& tex, bool facingMinusZ)
{
    if (objPath.endsWith(QLatin1String(".glb"), Qt::CaseInsensitive)) {
        const QByteArray glb = toTexturedGlb(vertices, triangles, uv, tex.albedo, tex.roughness, tex.metallic, facingMinusZ);
        if (glb.isEmpty()) return QStringLiteral("could not build the glb");
        QSaveFile f(objPath);
        if (!f.open(QIODevice::WriteOnly) || f.write(glb) != glb.size() || !f.commit()) return QStringLiteral("cannot write %1").arg(objPath);
        return {};
    }
    const QFileInfo fi(objPath);
    const QString stem = fi.completeBaseName();
    const QDir dir = fi.absoluteDir();
    const QString diffuse = stem + QStringLiteral("_diffuse.png");
    const QString rough = tex.roughness.isNull() ? QString() : stem + QStringLiteral("_roughness.png");
    const QString metal = tex.metallic.isNull() ? QString() : stem + QStringLiteral("_metallic.png");
    const QString mtl = stem + QStringLiteral(".mtl");

    if (!tex.albedo.save(dir.filePath(diffuse))) return QStringLiteral("cannot write %1").arg(dir.filePath(diffuse));
    if (!rough.isEmpty() && !tex.roughness.save(dir.filePath(rough))) return QStringLiteral("cannot write %1").arg(rough);
    if (!metal.isEmpty() && !tex.metallic.save(dir.filePath(metal))) return QStringLiteral("cannot write %1").arg(metal);

    auto writeFile = [&](const QString& name, const QByteArray& bytes) {
        QSaveFile f(dir.filePath(name));
        return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size() && f.commit();
    };
    if (!writeFile(mtl, toMtl(QStringLiteral("body"), diffuse, rough, metal))) return QStringLiteral("cannot write %1").arg(mtl);
    if (!writeFile(fi.fileName(), toTexturedObj(vertices, triangles, uv, mtl, facingMinusZ)))
        return QStringLiteral("cannot write %1").arg(objPath);
    return {};
}

} // namespace SamBody
