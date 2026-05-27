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

    // Walk the instance vector once so a row's `instanceIndex` is a direct
    // lookup later. `instanceByMesh[meshIndex]` is the index of the FIRST
    // instance using that mesh — many PS1 games re-issue the same matrix
    // for every quad of the same prop, so taking the first match is fine
    // for the inspector's "highlight one node" semantics.
    QHash<int, int> firstInstanceByMesh;
    for (int i = 0; i < reconstructed.instances.size(); ++i) {
        const int meshIndex = reconstructed.instances[i].uniqueMeshIndex;
        if (!firstInstanceByMesh.contains(meshIndex))
            firstInstanceByMesh.insert(meshIndex, i);
    }

    // Build a material-name → uniqueMeshIndex lookup from the reconstructed
    // submesh list. A material can appear in multiple unique meshes if the
    // capture had instances with different geometry but the same texture
    // page — in that case we just attribute the prim to the first mesh
    // that uses the material, which matches the dedupe order.
    QHash<QString, int> meshByMaterial;
    for (int meshIdx = 0; meshIdx < reconstructed.uniqueMeshes.size(); ++meshIdx) {
        const ReconstructedMesh &mesh = reconstructed.uniqueMeshes[meshIdx];
        for (const ReconstructedSubMesh &sub : mesh.subMeshes) {
            if (!meshByMaterial.contains(sub.materialName))
                meshByMaterial.insert(sub.materialName, meshIdx);
        }
    }

    set.rows.reserve(snapshot.prims.size());
    int rowOrdinal = 0;
    for (const PrimRecord &prim : snapshot.prims) {
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

        const auto meshIt = meshByMaterial.constFind(row.materialName);
        if (meshIt != meshByMaterial.constEnd()) {
            row.uniqueMeshIndex = meshIt.value();
            const auto instIt = firstInstanceByMesh.constFind(row.uniqueMeshIndex);
            if (instIt != firstInstanceByMesh.constEnd())
                row.instanceIndex = instIt.value();
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
