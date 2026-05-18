#include "MeshReconstructor.h"

#include "GteInverse.h"

#include <OgreColourValue.h>

#include <QHash>

namespace {

uint32_t packDiffuse(uint8_t r, uint8_t g, uint8_t b)
{
    const Ogre::ColourValue cv(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    return cv.getAsBYTE();
}

QString textureMaterialName(uint16_t tpage, uint16_t clut)
{
    return QStringLiteral("PS1Rip/tpage_%1_clut_%2")
        .arg(tpage, 4, 16, QChar('0'))
        .arg(clut, 4, 16, QChar('0'));
}

quint64 textureGroupKey(uint16_t tpage, uint16_t clut)
{
    return (static_cast<quint64>(tpage) << 32) | clut;
}

struct SubMeshAccumulator {
    QString materialName;
    QVector<ReconstructedVertex> vertices;
    QVector<uint32_t> indices;

    void addTriangle(const ReconstructedVertex &a, const ReconstructedVertex &b, const ReconstructedVertex &c)
    {
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        vertices.append(a);
        vertices.append(b);
        vertices.append(c);
        indices.append(base);
        indices.append(base + 1);
        indices.append(base + 2);
    }
};

ReconstructedVertex vertexFromPsx(const PsxVertex &v, const MatrixRecord *matrix, bool textured)
{
    ReconstructedVertex out;
    out.diffuseArgb = packDiffuse(v.r, v.g, v.b);

    float mx = 0.0f;
    float my = 0.0f;
    float mz = 0.0f;
    if (matrix && GteInverse::screenToModel(*matrix, v.x, v.y, v.z, mx, my, mz)) {
        GteInverse::psxScreenToWorld(mx, my, mz, out.px, out.py, out.pz);
    } else {
        GteInverse::psxScreenToWorld(static_cast<float>(v.x), static_cast<float>(v.y),
                                     static_cast<float>(v.z), out.px, out.py, out.pz);
    }

    if (textured) {
        out.u = static_cast<float>(v.u) / 256.0f;
        out.v = static_cast<float>(v.v) / 256.0f;
    }
    return out;
}

void emitPrimitive(const PrimRecord &prim, const MatrixRecord *matrix, SubMeshAccumulator &acc)
{
    const bool textured = prim.kind == PrimKind::TexturedTri || prim.kind == PrimKind::TexturedQuad
                          || prim.kind == PrimKind::Sprite;

    auto vtx = [&](int i) { return vertexFromPsx(prim.verts[i], matrix, textured); };

    if (prim.kind == PrimKind::MonoTri || prim.kind == PrimKind::ShadedTri
        || prim.kind == PrimKind::TexturedTri) {
        if (prim.vertexCount >= 3)
            acc.addTriangle(vtx(0), vtx(1), vtx(2));
        return;
    }

    if (prim.kind == PrimKind::MonoQuad || prim.kind == PrimKind::ShadedQuad
        || prim.kind == PrimKind::TexturedQuad) {
        if (prim.vertexCount >= 4) {
            acc.addTriangle(vtx(0), vtx(1), vtx(2));
            acc.addTriangle(vtx(0), vtx(2), vtx(3));
        }
        return;
    }

    if (prim.kind == PrimKind::Sprite && prim.vertexCount >= 2) {
        ReconstructedVertex a = vtx(0);
        ReconstructedVertex b = vtx(1);
        ReconstructedVertex c = b;
        ReconstructedVertex d = a;
        c.py = b.py + 0.05f;
        d.py = a.py + 0.05f;
        acc.addTriangle(a, b, c);
        acc.addTriangle(a, c, d);
    }
}

} // namespace

ReconstructedMesh MeshReconstructor::reconstruct(const CaptureSnapshot &snapshot)
{
    ReconstructedMesh result;
    if (snapshot.prims.isEmpty())
        return result;

    QHash<uint32_t, QHash<quint64, SubMeshAccumulator>> groupsByMatrix;

    for (const PrimRecord &prim : snapshot.prims) {
        const MatrixRecord *matrix = nullptr;
        if (prim.matrixId < static_cast<uint32_t>(snapshot.matrices.size()))
            matrix = &snapshot.matrices[static_cast<int>(prim.matrixId)];

        const quint64 texKey = textureGroupKey(prim.tpage, prim.clut);
        SubMeshAccumulator &acc = groupsByMatrix[prim.matrixId][texKey];
        if (acc.materialName.isEmpty())
            acc.materialName = textureMaterialName(prim.tpage, prim.clut);
        emitPrimitive(prim, matrix, acc);
    }

    int subIndex = 0;
    for (auto matIt = groupsByMatrix.constBegin(); matIt != groupsByMatrix.constEnd(); ++matIt) {
        for (auto texIt = matIt.value().constBegin(); texIt != matIt.value().constEnd(); ++texIt) {
            const SubMeshAccumulator &acc = texIt.value();
            if (acc.vertices.isEmpty() || acc.indices.size() < 3)
                continue;

            ReconstructedSubMesh sub;
            sub.materialName = acc.materialName;
            sub.vertices = acc.vertices;
            sub.indices = acc.indices;
            result.subMeshes.append(sub);
            result.vertexCount += acc.vertices.size();
            result.triangleCount += acc.indices.size() / 3;
            ++subIndex;
        }
    }

    result.meshName = QStringLiteral("ps1_capture");
    return result;
}
