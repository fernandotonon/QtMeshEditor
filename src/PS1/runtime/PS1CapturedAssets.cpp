#include "PS1CapturedAssets.h"

#include "MeshReconstructor.h"

#include <QHash>

PS1CapturedAssets *PS1CapturedAssets::s_instance = nullptr;

PS1CapturedAssets::PS1CapturedAssets(QObject *parent)
    : QObject(parent)
{
}

PS1CapturedAssets *PS1CapturedAssets::getSingleton()
{
    if (!s_instance)
        s_instance = new PS1CapturedAssets();
    return s_instance;
}

PS1CapturedAssets *PS1CapturedAssets::getSingletonPtr()
{
    return s_instance;
}

void PS1CapturedAssets::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

void PS1CapturedAssets::clear()
{
    if (m_set.captureId.isEmpty() && m_set.rows.isEmpty())
        return;
    m_set = CapturedAssetSet{};
    emit captureSetChanged();
}

void PS1CapturedAssets::setCaptureSet(CapturedAssetSet set)
{
    m_set = std::move(set);
    emit captureSetChanged();
}

bool PS1CapturedAssets::setRowHidden(int rowIndex, bool hidden)
{
    if (rowIndex < 1 || rowIndex > m_set.rows.size())
        return false;
    CapturedAssetRow &row = m_set.rows[rowIndex - 1];
    if (row.hidden == hidden)
        return false;
    row.hidden = hidden;
    emit rowChanged(rowIndex);
    return true;
}

bool PS1CapturedAssets::setRowDiscarded(int rowIndex, bool discarded)
{
    if (rowIndex < 1 || rowIndex > m_set.rows.size())
        return false;
    CapturedAssetRow &row = m_set.rows[rowIndex - 1];
    if (row.discarded == discarded)
        return false;
    row.discarded = discarded;
    emit rowChanged(rowIndex);
    return true;
}

namespace {

int trianglesForKind(PrimKind kind)
{
    switch (kind) {
    case PrimKind::MonoTri:
    case PrimKind::ShadedTri:
    case PrimKind::TexturedTri:
        return 1;
    case PrimKind::MonoQuad:
    case PrimKind::ShadedQuad:
    case PrimKind::TexturedQuad:
    case PrimKind::Sprite:
        return 2;
    }
    return 1;
}

bool isTexturedKind(PrimKind kind)
{
    return kind == PrimKind::TexturedTri || kind == PrimKind::TexturedQuad
           || kind == PrimKind::Sprite;
}

bool isColoredKind(PrimKind kind)
{
    return kind == PrimKind::ShadedTri || kind == PrimKind::ShadedQuad
           || kind == PrimKind::MonoTri || kind == PrimKind::MonoQuad;
}

/** Build a stable identity for a texture page so we can dedupe textures
 *  for the count header. Matches `MeshReconstructor::textureMaterialName`
 *  for textured prims; mono / shaded prims contribute no texture. */
QString textureIdentityForPrim(const PrimRecord &prim)
{
    if (!isTexturedKind(prim.kind))
        return {};
    return MeshReconstructor::textureMaterialName(prim.tpage, prim.clut, prim.semiTrans,
                                                  prim.drawModeBits);
}

} // namespace

CapturedAssetSet PS1CapturedAssets::buildFromCapture(const QString &captureId,
                                                    const CaptureSnapshot &snapshot,
                                                    const ReconstructedCaptureSet &reconstructed,
                                                    const QHash<QString, QImage> &textureImages)
{
    CapturedAssetSet set;
    set.captureId = captureId;
    set.uniqueMeshes = reconstructed.uniqueMeshes;
    set.instances = reconstructed.instances;
    set.textureImages = textureImages;

    // Per-prim provenance from the reconstructor — the authoritative source
    // for "which (uniqueMesh, submesh, instance) did this draw call land in".
    // The previous material-name-first-match heuristic collapsed every
    // solid-color row onto a single mesh / instance and broke inspector
    // actions for any capture with > 1 matrix group sharing a material
    // (#679 review feedback). When the reconstructor didn't emit provenance
    // (e.g. unit tests that hand-build a `ReconstructedCaptureSet`), we
    // fall back to a material-name lookup so legacy callers keep working.
    const QVector<PrimProvenance> &provenance = reconstructed.primProvenance;
    const bool hasProvenance = provenance.size() == snapshot.prims.size();

    QHash<QString, int> meshByMaterialFallback;
    QHash<int, int> firstInstanceByMeshFallback;
    if (!hasProvenance) {
        for (int meshIdx = 0; meshIdx < reconstructed.uniqueMeshes.size(); ++meshIdx) {
            const ReconstructedMesh &mesh = reconstructed.uniqueMeshes[meshIdx];
            for (const ReconstructedSubMesh &sub : mesh.subMeshes) {
                if (!meshByMaterialFallback.contains(sub.materialName))
                    meshByMaterialFallback.insert(sub.materialName, meshIdx);
            }
        }
        for (int i = 0; i < reconstructed.instances.size(); ++i) {
            const int meshIndex = reconstructed.instances[i].uniqueMeshIndex;
            if (!firstInstanceByMeshFallback.contains(meshIndex))
                firstInstanceByMeshFallback.insert(meshIndex, i);
        }
    }

    set.rows.reserve(snapshot.prims.size());
    int rowOrdinal = 0;
    for (int primIdx = 0; primIdx < snapshot.prims.size(); ++primIdx) {
        const PrimRecord &prim = snapshot.prims[primIdx];
        ++rowOrdinal;
        CapturedAssetRow row;
        row.rowIndex = rowOrdinal;
        row.kind = prim.kind;
        row.vertexCount = prim.vertexCount;
        row.tpage = prim.tpage;
        row.clut = prim.clut;
        row.matrixId = prim.matrixId;
        row.triangleCount = trianglesForKind(prim.kind);
        row.textured = isTexturedKind(prim.kind);
        row.colored = isColoredKind(prim.kind);
        // `frameIndex` is not part of `PrimRecord` today — left at 0 so the
        // column renders consistently. When scene-capture frame tagging
        // lands (#683 follow-up), this is where to thread it through.
        row.frameIndex = 0;

        if (row.textured) {
            row.materialName = MeshReconstructor::textureMaterialName(
                prim.tpage, prim.clut, prim.semiTrans, prim.drawModeBits);
        } else {
            row.materialName = QStringLiteral("PS1Rip_color");
        }

        if (hasProvenance) {
            const PrimProvenance &p = provenance.at(primIdx);
            row.uniqueMeshIndex = p.uniqueMeshIndex;
            row.subMeshIndex = p.subMeshIndex;
            row.instanceIndex = p.instanceIndex;
        } else {
            const auto meshIt = meshByMaterialFallback.constFind(row.materialName);
            if (meshIt != meshByMaterialFallback.constEnd()) {
                row.uniqueMeshIndex = meshIt.value();
                const auto instIt = firstInstanceByMeshFallback.constFind(row.uniqueMeshIndex);
                if (instIt != firstInstanceByMeshFallback.constEnd())
                    row.instanceIndex = instIt.value();
                // Best-effort submesh lookup by material name.
                if (row.uniqueMeshIndex >= 0
                    && row.uniqueMeshIndex < reconstructed.uniqueMeshes.size()) {
                    const auto &subs =
                        reconstructed.uniqueMeshes.at(row.uniqueMeshIndex).subMeshes;
                    for (int s = 0; s < subs.size(); ++s) {
                        if (subs.at(s).materialName == row.materialName) {
                            row.subMeshIndex = s;
                            break;
                        }
                    }
                }
            }
        }

        set.totalPrims += 1;
        set.totalTris += row.triangleCount;
        if (row.textured) {
            const QString texId = textureIdentityForPrim(prim);
            if (!texId.isEmpty())
                set.uniqueTextureIds.insert(texId);
        }

        set.rows.push_back(row);
    }

    // Same naming scheme as `PS1RipMeshBuilder::attachCaptureSetToScene`.
    int ordinal = 0;
    for (int meshIndex = 0; meshIndex < reconstructed.uniqueMeshes.size(); ++meshIndex) {
        for (int instIdx = 0; instIdx < reconstructed.instances.size(); ++instIdx) {
            if (reconstructed.instances[instIdx].uniqueMeshIndex != meshIndex)
                continue;
            set.instanceNodeNames.insert(
                instIdx, QStringLiteral("PS1Capture_%1_inst%2").arg(captureId).arg(ordinal++));
        }
    }

    return set;
}
