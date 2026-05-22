#include "MeshReconstructor.h"

#include "GteInverse.h"
#include "MeshTopologyHash.h"
#include "PsxCaptureFilters.h"

#include <OgreColourValue.h>

#include <QHash>

#include <cmath>

namespace {

constexpr float kMaxVertexRadius = 64.0f;

uint32_t packDiffuse(uint8_t r, uint8_t g, uint8_t b)
{
    const Ogre::ColourValue cv(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
    return cv.getAsBYTE();
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
        float wx = 0.0f;
        float wy = 0.0f;
        float wz = 0.0f;
        GteInverse::modelToEditor(mx, my, mz, wx, wy, wz);
        const float radius = std::sqrt(wx * wx + wy * wy + wz * wz);
        if (std::isfinite(radius) && radius <= kMaxVertexRadius) {
            out.px = wx;
            out.py = wy;
            out.pz = wz;
        } else {
            GteInverse::psxScreenToWorld(static_cast<float>(v.x), static_cast<float>(v.y),
                                         static_cast<float>(v.z), out.px, out.py, out.pz);
        }
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

QHash<uint32_t, QHash<quint64, SubMeshAccumulator>> buildMatrixGroups(const CaptureSnapshot &snapshot)
{
    QHash<uint32_t, QHash<quint64, SubMeshAccumulator>> groupsByMatrix;

    for (const PrimRecord &prim : snapshot.prims) {
        if (!PsxCaptureFilters::isOnScreenPrim(prim))
            continue;

        const MatrixRecord *matrix = nullptr;
        if (prim.matrixId < static_cast<uint32_t>(snapshot.matrices.size()))
            matrix = &snapshot.matrices[static_cast<int>(prim.matrixId)];

        const quint64 texKey = MeshReconstructor::textureGroupKey(
            prim.tpage, prim.clut, prim.semiTrans, prim.drawModeBits);
        SubMeshAccumulator &acc = groupsByMatrix[prim.matrixId][texKey];
        if (acc.materialName.isEmpty())
            acc.materialName = MeshReconstructor::textureMaterialName(
                prim.tpage, prim.clut, prim.semiTrans, prim.drawModeBits);
        emitPrimitive(prim, matrix, acc);
    }
    return groupsByMatrix;
}

ReconstructedMesh meshFromMatrixGroup(uint32_t matrixId,
                                      const QHash<quint64, SubMeshAccumulator> &texGroups)
{
    ReconstructedMesh result;
    result.meshName = QStringLiteral("ps1_part_%1").arg(matrixId);

    for (auto texIt = texGroups.constBegin(); texIt != texGroups.constEnd(); ++texIt) {
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
    }
    return result;
}

QVector<ReconstructedMesh> buildParts(const CaptureSnapshot &snapshot)
{
    QVector<ReconstructedMesh> parts;
    const QHash<uint32_t, QHash<quint64, SubMeshAccumulator>> groupsByMatrix =
        buildMatrixGroups(snapshot);

    for (auto matIt = groupsByMatrix.constBegin(); matIt != groupsByMatrix.constEnd(); ++matIt) {
        ReconstructedMesh part = meshFromMatrixGroup(matIt.key(), matIt.value());
        if (!part.isEmpty())
            parts.append(part);
    }
    return parts;
}

ReconstructedMesh flattenParts(const QVector<ReconstructedMesh> &parts)
{
    ReconstructedMesh merged;
    merged.meshName = QStringLiteral("ps1_capture");
    for (const ReconstructedMesh &part : parts) {
        for (const ReconstructedSubMesh &sub : part.subMeshes)
            merged.subMeshes.append(sub);
        merged.vertexCount += part.vertexCount;
        merged.triangleCount += part.triangleCount;
    }
    return merged;
}

} // namespace

QString MeshReconstructor::textureMaterialName(uint16_t tpage, uint16_t clut, uint8_t semiTrans,
                                                 uint32_t drawModeBits)
{
    return QStringLiteral("PS1Rip_tpage_%1_clut_%2_st%3_dm%4")
        .arg(tpage, 4, 16, QChar('0'))
        .arg(clut, 4, 16, QChar('0'))
        .arg(semiTrans & 3)
        .arg((drawModeBits >> 11) & 1u);
}

quint64 MeshReconstructor::textureGroupKey(uint16_t tpage, uint16_t clut, uint8_t semiTrans,
                                           uint32_t drawModeBits)
{
    return (static_cast<quint64>(tpage) << 32) | clut
           | (static_cast<quint64>(semiTrans & 3) << 48)
           | (static_cast<quint64>((drawModeBits >> 11) & 1u) << 52);
}

ReconstructedMesh MeshReconstructor::reconstruct(const CaptureSnapshot &snapshot)
{
    return flattenParts(buildParts(snapshot));
}

ReconstructedCaptureSet MeshReconstructor::reconstructDeduped(const CaptureSnapshot &snapshot,
                                                            MeshDedupeMode dedupeMode)
{
    ReconstructedCaptureSet result;
    const QVector<ReconstructedMesh> parts = buildParts(snapshot);
    result.capturedPartCount = parts.size();
    if (parts.isEmpty())
        return result;

    QHash<quint64, int> hashToUnique;

    for (const ReconstructedMesh &part : parts) {
        float cx = 0.0f;
        float cy = 0.0f;
        float cz = 0.0f;
        const ReconstructedMesh local = MeshTopologyHash::centered(part, cx, cy, cz);
        const quint64 h = MeshTopologyHash::hashMesh(local, dedupeMode);

        int uniqueIndex = hashToUnique.value(h, -1);
        if (uniqueIndex < 0) {
            uniqueIndex = result.uniqueMeshes.size();
            hashToUnique.insert(h, uniqueIndex);
            ReconstructedMesh unique = local;
            unique.meshName = QStringLiteral("ps1_unique_%1").arg(uniqueIndex);
            result.uniqueMeshes.append(unique);
        }

        ReconstructedInstance inst;
        inst.uniqueMeshIndex = uniqueIndex;
        inst.px = cx;
        inst.py = cy;
        inst.pz = cz;
        result.instances.append(inst);
    }

    return result;
}
