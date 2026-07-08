#include "MeshReconstructor.h"

#include "GteInverse.h"
#include "MeshTopologyHash.h"
#include "PsxCaptureFilters.h"

#include <OgreColourValue.h>

#include <QHash>

#include <algorithm>
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

ReconstructedVertex vertexFromPsx(const PsxVertex &v, const MatrixRecord *matrix, bool textured,
                                  bool *usedGteInverseOut = nullptr)
{
    ReconstructedVertex out;
    out.diffuseArgb = packDiffuse(v.r, v.g, v.b);

    float mx = 0.0f;
    float my = 0.0f;
    float mz = 0.0f;
    bool usedGte = false;
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
            usedGte = true;
        } else {
            GteInverse::psxScreenToWorld(static_cast<float>(v.x), static_cast<float>(v.y),
                                         static_cast<float>(v.z), out.px, out.py, out.pz);
        }
    } else {
        GteInverse::psxScreenToWorld(static_cast<float>(v.x), static_cast<float>(v.y),
                                     static_cast<float>(v.z), out.px, out.py, out.pz);
    }
    if (usedGteInverseOut)
        *usedGteInverseOut = usedGte;

    if (textured) {
        out.u = static_cast<float>(v.u) / 256.0f;
        out.v = static_cast<float>(v.v) / 256.0f;
    }
    return out;
}

/** Expand `stats`' AABB to include `v`. First call initialises the bounds to the
 *  vertex (we can't anchor at 0 — meshes that live entirely on one side of the
 *  origin would lose their min or max). Shared by every code path that needs to
 *  fold a vertex into stats so the behavior stays in lock-step (#674 review). */
void expandBounds(MeshReconstructionStats &stats, const ReconstructedVertex &v)
{
    if (!stats.hasBounds()) {
        stats.boundsMinX = stats.boundsMaxX = v.px;
        stats.boundsMinY = stats.boundsMaxY = v.py;
        stats.boundsMinZ = stats.boundsMaxZ = v.pz;
        return;
    }
    stats.boundsMinX = std::min(stats.boundsMinX, v.px);
    stats.boundsMaxX = std::max(stats.boundsMaxX, v.px);
    stats.boundsMinY = std::min(stats.boundsMinY, v.py);
    stats.boundsMaxY = std::max(stats.boundsMaxY, v.py);
    stats.boundsMinZ = std::min(stats.boundsMinZ, v.pz);
    stats.boundsMaxZ = std::max(stats.boundsMaxZ, v.pz);
}

void accumulateVertexStats(const ReconstructedVertex &v, MeshReconstructionStats &stats, bool usedGte)
{
    ++stats.totalVertices;
    if (usedGte)
        ++stats.gteInverseVertices;
    else
        ++stats.screenFallbackVertices;
    expandBounds(stats, v);
}

PsxVertex midpointPsx(const PsxVertex &a, const PsxVertex &b)
{
    // Integer division on screen coords is fine: even at the coarsest PS1
    // resolution (640px-wide modes) a 1-pixel rounding error along an edge
    // is well below the perspective-correct UV subdivision's own tolerance
    // (1.3 default depth ratio). UV is i16/256 fixed-point so the same
    // integer halving preserves screen-space affine interpolation exactly.
    PsxVertex m;
    m.x = (a.x + b.x) / 2;
    m.y = (a.y + b.y) / 2;
    m.z = (a.z + b.z) / 2;
    m.u = static_cast<int16_t>((static_cast<int>(a.u) + static_cast<int>(b.u)) / 2);
    m.v = static_cast<int16_t>((static_cast<int>(a.v) + static_cast<int>(b.v)) / 2);
    m.r = static_cast<uint8_t>((static_cast<int>(a.r) + static_cast<int>(b.r)) / 2);
    m.g = static_cast<uint8_t>((static_cast<int>(a.g) + static_cast<int>(b.g)) / 2);
    m.b = static_cast<uint8_t>((static_cast<int>(a.b) + static_cast<int>(b.b)) / 2);
    return m;
}

/** True when the triangle's per-vertex screen-space depth (sz) varies enough
 *  that the perspective-vs-affine UV gap is visibly larger than `tolerance`.
 *  Falls back to "don't subdivide" when any sz is zero (GP0-only captures
 *  have no depth — see GteInverse::screenToModel sz==0 guard, #675). */
bool depthRatioExceedsTolerance(const PsxVertex &a, const PsxVertex &b, const PsxVertex &c,
                                float tolerance)
{
    const int zs[3] = { a.z, b.z, c.z };
    int zmin = zs[0], zmax = zs[0];
    for (int i = 1; i < 3; ++i) {
        zmin = std::min(zmin, zs[i]);
        zmax = std::max(zmax, zs[i]);
    }
    if (zmin <= 0)
        return false;
    return static_cast<float>(zmax) / static_cast<float>(zmin) > tolerance;
}

void emitTriDirect(const PsxVertex &a, const PsxVertex &b, const PsxVertex &c,
                   const MatrixRecord *matrix, bool textured, SubMeshAccumulator &acc,
                   MeshReconstructionStats *statsOut)
{
    auto vtx = [&](const PsxVertex &pv) {
        bool usedGte = false;
        ReconstructedVertex out = vertexFromPsx(pv, matrix, textured, &usedGte);
        if (statsOut)
            accumulateVertexStats(out, *statsOut, usedGte);
        return out;
    };
    acc.addTriangle(vtx(a), vtx(b), vtx(c));
}

/** Recursive midpoint subdivision: when the triangle's depth ratio exceeds
 *  `tolerance`, split into 4 sub-tris via edge midpoints and recurse.
 *  New midpoint vertices' UVs are computed via screen-space linear interp
 *  (the PS1 affine convention) so Ogre's perspective-correct rendering of
 *  the resulting fine mesh approximates what the original PS1 GPU showed.
 *  Bounded by `maxDepth` so a single very-warped prim can't blow up to
 *  thousands of tris (4^3 = 64 sub-tris at the default depth=3). */
void emitTriSubdivided(const PsxVertex &a, const PsxVertex &b, const PsxVertex &c,
                       const MatrixRecord *matrix, bool textured, SubMeshAccumulator &acc,
                       MeshReconstructionStats *statsOut, float tolerance, int remainingDepth)
{
    if (remainingDepth <= 0 || !depthRatioExceedsTolerance(a, b, c, tolerance)) {
        emitTriDirect(a, b, c, matrix, textured, acc, statsOut);
        return;
    }
    const PsxVertex ab = midpointPsx(a, b);
    const PsxVertex bc = midpointPsx(b, c);
    const PsxVertex ca = midpointPsx(c, a);
    const int next = remainingDepth - 1;
    emitTriSubdivided(a,  ab, ca, matrix, textured, acc, statsOut, tolerance, next);
    emitTriSubdivided(ab, b,  bc, matrix, textured, acc, statsOut, tolerance, next);
    emitTriSubdivided(ca, bc, c,  matrix, textured, acc, statsOut, tolerance, next);
    emitTriSubdivided(ab, bc, ca, matrix, textured, acc, statsOut, tolerance, next);
}

void emitTri(const PsxVertex &a, const PsxVertex &b, const PsxVertex &c,
             const MatrixRecord *matrix, bool textured, SubMeshAccumulator &acc,
             MeshReconstructionStats *statsOut, const Ps1NormalizerSettings &settings)
{
    // Perspective-correct subdivision exists to reproduce PS1 affine UV
    // sampling at fine grain. There's no UV channel on mono / shaded prims
    // (HUDs, flat-shaded geometry), so tessellating them would just inflate
    // triangle counts without changing the visual — skip them.
    if (textured && settings.perspectiveCorrectUVs && settings.perspectiveMaxDepth > 0) {
        emitTriSubdivided(a, b, c, matrix, textured, acc, statsOut,
                          settings.perspectiveTolerance, settings.perspectiveMaxDepth);
        return;
    }
    emitTriDirect(a, b, c, matrix, textured, acc, statsOut);
}

void emitPrimitive(const PrimRecord &prim, const MatrixRecord *matrix, SubMeshAccumulator &acc,
                   MeshReconstructionStats *statsOut, const Ps1NormalizerSettings &settings)
{
    const bool textured = prim.kind == PrimKind::TexturedTri || prim.kind == PrimKind::TexturedQuad
                          || prim.kind == PrimKind::Sprite;

    if (prim.kind == PrimKind::MonoTri || prim.kind == PrimKind::ShadedTri
        || prim.kind == PrimKind::TexturedTri) {
        if (prim.vertexCount >= 3)
            emitTri(prim.verts[0], prim.verts[1], prim.verts[2], matrix, textured, acc,
                    statsOut, settings);
        return;
    }

    if (prim.kind == PrimKind::MonoQuad || prim.kind == PrimKind::ShadedQuad
        || prim.kind == PrimKind::TexturedQuad) {
        if (prim.vertexCount >= 4) {
            emitTri(prim.verts[0], prim.verts[1], prim.verts[2], matrix, textured, acc,
                    statsOut, settings);
            emitTri(prim.verts[0], prim.verts[2], prim.verts[3], matrix, textured, acc,
                    statsOut, settings);
        }
        return;
    }

    if (prim.kind == PrimKind::Sprite && prim.vertexCount >= 2) {
        // Sprites are screen-aligned billboards (no depth variance across the
        // pair), so subdivision is a no-op for them — we keep the original
        // 2-tri expansion. The pinned 0.05f py offset stays because the
        // captured pair lacks the second pair of corners.
        auto vtx = [&](int i) {
            bool usedGte = false;
            ReconstructedVertex out = vertexFromPsx(prim.verts[i], matrix, textured, &usedGte);
            if (statsOut)
                accumulateVertexStats(out, *statsOut, usedGte);
            return out;
        };
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

QHash<uint32_t, QHash<quint64, SubMeshAccumulator>> buildMatrixGroups(const CaptureSnapshot &snapshot,
                                                                    MeshReconstructionStats *statsOut,
                                                                    const Ps1NormalizerSettings &settings)
{
    QHash<uint32_t, QHash<quint64, SubMeshAccumulator>> groupsByMatrix;

    if (statsOut) {
        statsOut->primsTotal = snapshot.prims.size();
        for (const PrimRecord &prim : snapshot.prims) {
            if (prim.matrixId < static_cast<uint32_t>(snapshot.matrices.size()))
                ++statsOut->primsWithMatrixId;
        }
    }

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
        emitPrimitive(prim, matrix, acc, statsOut, settings);
    }
    if (statsOut)
        statsOut->finalizeSlabMetric();
    return groupsByMatrix;
}

/** Build the mesh for a single matrix group, recording which texture key
 *  produced which submesh index in `texKeyToSubMesh` so the per-prim
 *  provenance pass can resolve `prim → (matrixId, texKey) → submeshIndex`
 *  (#679 review feedback). */
ReconstructedMesh meshFromMatrixGroup(uint32_t matrixId,
                                      const QHash<quint64, SubMeshAccumulator> &texGroups,
                                      QHash<quint64, int> *texKeyToSubMesh = nullptr)
{
    ReconstructedMesh result;
    result.meshName = QStringLiteral("ps1_part_%1").arg(matrixId);

    for (auto texIt = texGroups.constBegin(); texIt != texGroups.constEnd(); ++texIt) {
        const SubMeshAccumulator &acc = texIt.value();
        if (acc.vertices.isEmpty() || acc.indices.size() < 3)
            continue;

        const int subMeshIndex = result.subMeshes.size();
        if (texKeyToSubMesh)
            texKeyToSubMesh->insert(texIt.key(), subMeshIndex);

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

struct PartsBuildResult {
    QVector<ReconstructedMesh> parts;
    // partIndexByMatrixId[matrixId] = index into `parts` for this matrix.
    // Matrices whose group produced no surviving submesh (every accumulator
    // got skipped by the < 3 indices guard) are simply absent.
    QHash<uint32_t, int> partIndexByMatrixId;
    // subMeshIndexByTexKey[partIndex][texKey] = submeshIndex within parts[partIndex].
    // Empty for parts contributed by `snapshot.modelMeshes` (those bypass the
    // matrix-group path and don't carry a texKey-addressable identity).
    QVector<QHash<quint64, int>> subMeshIndexByTexKey;
};

PartsBuildResult buildParts(const CaptureSnapshot &snapshot,
                            MeshReconstructionStats *statsOut,
                            const Ps1NormalizerSettings &settings)
{
    PartsBuildResult out;
    const QHash<uint32_t, QHash<quint64, SubMeshAccumulator>> groupsByMatrix =
        buildMatrixGroups(snapshot, statsOut, settings);

    for (auto matIt = groupsByMatrix.constBegin(); matIt != groupsByMatrix.constEnd(); ++matIt) {
        QHash<quint64, int> texKeyToSubMesh;
        ReconstructedMesh part = meshFromMatrixGroup(matIt.key(), matIt.value(), &texKeyToSubMesh);
        if (part.isEmpty())
            continue;
        out.partIndexByMatrixId.insert(matIt.key(), out.parts.size());
        out.subMeshIndexByTexKey.append(texKeyToSubMesh);
        out.parts.append(part);
    }

    // #674 — Model-space meshes from PsxTmdRamScanner / PsxHmdRamScanner. These bypass the
    // screen-space inverse-projection path entirely and arrive in editor world units, so
    // they're appended as additional parts. The dedupe pass in `reconstructDeduped` then
    // collapses byte-identical copies via MeshTopologyHash, the same way it does for
    // matrix-grouped screen-space parts.
    for (const CapturedModelMesh &cap : snapshot.modelMeshes) {
        if (cap.mesh.isEmpty())
            continue;
        out.subMeshIndexByTexKey.append(QHash<quint64, int>{});
        out.parts.append(cap.mesh);
        if (statsOut) {
            int verts = 0;
            for (const ReconstructedSubMesh &sub : cap.mesh.subMeshes) {
                verts += sub.vertices.size();
                // Share the bounds-update helper with accumulateVertexStats so a
                // future tweak to the bounds anchoring stays in lock-step
                // (#674 review).
                for (const ReconstructedVertex &v : sub.vertices)
                    expandBounds(*statsOut, v);
            }
            statsOut->modelMeshVertices += verts;
            statsOut->totalVertices += verts;
        }
    }

    return out;
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

/** Walk `snapshot.prims` in order and resolve each one to the part + submesh
 *  it produced via the build-time `(matrixId, texKey) → (partIndex, subMeshIndex)`
 *  map. `instanceIndex == partIndex` because the dedupe pass emits exactly one
 *  instance per surviving part in the same order. Prims that don't survive
 *  the on-screen filter or whose matrix group was wholly skipped leave their
 *  default `-1` provenance — the inspector silently ignores those rows. */
QVector<PrimProvenance> resolvePrimProvenance(const CaptureSnapshot &snapshot,
                                              const PartsBuildResult &build,
                                              const QVector<int> &partIndexToUnique)
{
    QVector<PrimProvenance> provenance(snapshot.prims.size());
    for (int i = 0; i < snapshot.prims.size(); ++i) {
        const PrimRecord &prim = snapshot.prims[i];
        if (!PsxCaptureFilters::isOnScreenPrim(prim))
            continue;
        const bool survives =
            ((prim.kind == PrimKind::MonoTri || prim.kind == PrimKind::ShadedTri
              || prim.kind == PrimKind::TexturedTri) && prim.vertexCount >= 3)
            || ((prim.kind == PrimKind::MonoQuad || prim.kind == PrimKind::ShadedQuad
                 || prim.kind == PrimKind::TexturedQuad) && prim.vertexCount >= 4)
            || (prim.kind == PrimKind::Sprite && prim.vertexCount >= 2);
        if (!survives)
            continue;
        const auto partIt = build.partIndexByMatrixId.constFind(prim.matrixId);
        if (partIt == build.partIndexByMatrixId.constEnd())
            continue;
        const int partIndex = partIt.value();
        if (partIndex < 0 || partIndex >= build.subMeshIndexByTexKey.size())
            continue;
        const QHash<quint64, int> &texMap = build.subMeshIndexByTexKey.at(partIndex);
        const quint64 texKey = MeshReconstructor::textureGroupKey(
            prim.tpage, prim.clut, prim.semiTrans, prim.drawModeBits);
        const auto subIt = texMap.constFind(texKey);
        if (subIt == texMap.constEnd())
            continue;
        if (partIndex >= partIndexToUnique.size())
            continue;
        const int uniqueIndex = partIndexToUnique.at(partIndex);
        if (uniqueIndex < 0)
            continue;
        provenance[i].uniqueMeshIndex = uniqueIndex;
        provenance[i].subMeshIndex = subIt.value();
        // Instance order is part order: `reconstructDeduped` walks `build.parts`
        // and appends one instance per surviving part, so `partIndex` is also
        // the index into `ReconstructedCaptureSet::instances`.
        provenance[i].instanceIndex = partIndex;
    }
    return provenance;
}

} // namespace

// textureMaterialName / textureGroupKey live in MeshReconstructorTexKeys.cpp
// (Ogre-free TU shared with the libretro plugin, #674/#813).

ReconstructedMesh MeshReconstructor::reconstruct(const CaptureSnapshot &snapshot)
{
    return flattenParts(buildParts(snapshot, nullptr, Ps1NormalizerSettings{}).parts);
}

ReconstructedCaptureSet MeshReconstructor::reconstructDeduped(const CaptureSnapshot &snapshot,
                                                            MeshDedupeMode dedupeMode)
{
    return reconstructDeduped(snapshot, dedupeMode, Ps1NormalizerSettings{}, nullptr);
}

ReconstructedCaptureSet MeshReconstructor::reconstructDeduped(const CaptureSnapshot &snapshot,
                                                            MeshDedupeMode dedupeMode,
                                                            MeshReconstructionStats *statsOut)
{
    return reconstructDeduped(snapshot, dedupeMode, Ps1NormalizerSettings{}, statsOut);
}

ReconstructedCaptureSet MeshReconstructor::reconstructDeduped(const CaptureSnapshot &snapshot,
                                                            MeshDedupeMode dedupeMode,
                                                            const Ps1NormalizerSettings &normalize,
                                                            MeshReconstructionStats *statsOut)
{
    ReconstructedCaptureSet result;
    const PartsBuildResult build = buildParts(snapshot, statsOut, normalize);
    result.capturedPartCount = build.parts.size();
    if (build.parts.isEmpty()) {
        // Still emit a default-filled provenance vector so the inspector
        // can index it 1:1 with `snapshot.prims` without checking sizes.
        result.primProvenance.resize(snapshot.prims.size());
        return result;
    }

    QHash<quint64, int> hashToUnique;
    QVector<int> partIndexToUnique;
    partIndexToUnique.reserve(build.parts.size());

    for (const ReconstructedMesh &part : build.parts) {
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
        partIndexToUnique.append(uniqueIndex);
    }

    result.primProvenance = resolvePrimProvenance(snapshot, build, partIndexToUnique);
    return result;
}
