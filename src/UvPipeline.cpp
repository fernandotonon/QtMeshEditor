#include "UvPipeline.h"

#include "EditableMesh.h"
#include "UVEditorController.h"
#include "UvSeamData.h"

#include <OgreEntity.h>
#include <OgreHardwareBuffer.h>
#include <OgreSubMesh.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace {

struct TriBox {
    float xmin = 0.f;
    float xmax = 0.f;
    float ymin = 0.f;
    float ymax = 0.f;
};

double overlappingUvsRatioFromEditableMesh(const EditableMesh& mesh)
{
    std::vector<TriBox> boxes;
    bool sawUv = false;

    for (const auto& sub : mesh.subMeshes()) {
        for (const auto& tri : sub.triangles) {
            Ogre::Vector2 uvs[3];
            bool ok = true;
            for (int c = 0; c < 3; ++c) {
                const auto& v = sub.vertices[tri.indices[c]];
                if (!v.hasUV) {
                    ok = false;
                    break;
                }
                uvs[c] = v.uv;
            }
            if (!ok)
                continue;
            sawUv = true;
            TriBox box;
            box.xmin = std::min({uvs[0].x, uvs[1].x, uvs[2].x});
            box.xmax = std::max({uvs[0].x, uvs[1].x, uvs[2].x});
            box.ymin = std::min({uvs[0].y, uvs[1].y, uvs[2].y});
            box.ymax = std::max({uvs[0].y, uvs[1].y, uvs[2].y});
            boxes.push_back(box);
        }
    }

    if (!sawUv || boxes.empty())
        return -1.0;

    std::vector<size_t> order(boxes.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return boxes[a].xmin < boxes[b].xmin;
    });

    std::vector<bool> overlapped(boxes.size(), false);
    for (size_t oi = 0; oi < order.size(); ++oi) {
        const size_t i = order[oi];
        const TriBox& a = boxes[i];
        for (size_t oj = oi + 1; oj < order.size(); ++oj) {
            const size_t j = order[oj];
            if (boxes[j].xmin > a.xmax)
                break;
            const TriBox& b = boxes[j];
            if (a.xmax < b.xmin || a.ymax < b.ymin || b.ymax < a.ymin)
                continue;
            overlapped[i] = true;
            overlapped[j] = true;
        }
    }

    const int overlapCount =
        static_cast<int>(std::count(overlapped.begin(), overlapped.end(), true));

    return static_cast<double>(overlapCount) / static_cast<double>(boxes.size());
}

void applyUvChannelFromEntity(EditableMesh& mesh, Ogre::Entity* entity, int channel)
{
    if (!entity)
        return;
    for (size_t si = 0; si < mesh.subMeshes().size(); ++si) {
        auto& sub = mesh.subMeshes()[si];
        if (static_cast<unsigned short>(si) >= entity->getMesh()->getNumSubMeshes())
            continue;
        const Ogre::SubMesh* ogreSub = entity->getMesh()->getSubMesh(static_cast<unsigned short>(si));
        const Ogre::VertexData* vd =
            ogreSub->useSharedVertices ? entity->getMesh()->sharedVertexData : ogreSub->vertexData;
        if (!vd)
            continue;

        const Ogre::VertexElement* elem =
            vd->vertexDeclaration->findElementBySemantic(Ogre::VES_TEXTURE_COORDINATES,
                                                         static_cast<unsigned short>(channel));
        if (!elem)
            continue;
        const Ogre::VertexElementType type = elem->getType();
        if (type != Ogre::VET_FLOAT2 && type != Ogre::VET_FLOAT3 && type != Ogre::VET_FLOAT4)
            continue;

        const auto vbuf = vd->vertexBufferBinding->getBuffer(elem->getSource());
        const size_t stride = vbuf->getVertexSize();
        const size_t count = vd->vertexCount;
        std::vector<Ogre::Vector2> uvs(count);
        auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (size_t vi = 0; vi < count; ++vi) {
            float* p = nullptr;
            elem->baseVertexPointerToElement(base + vi * stride, &p);
            uvs[vi] = {p[0], p[1]};
        }
        vbuf->unlock();

        const size_t n = std::min(sub.vertices.size(), uvs.size());
        for (size_t vi = 0; vi < n; ++vi) {
            sub.vertices[vi].uv = uvs[vi];
            sub.vertices[vi].hasUV = true;
        }
    }
}

} // namespace

UvProject::Mode UvPipeline::parseProjectMode(const QString& name, bool* ok)
{
    const QString n = name.trimmed().toLower();
    if (n == QStringLiteral("box")) {
        if (ok) *ok = true;
        return UvProject::Mode::Box;
    }
    if (n == QStringLiteral("cylinder") || n == QStringLiteral("cyl")) {
        if (ok) *ok = true;
        return UvProject::Mode::Cylinder;
    }
    if (n == QStringLiteral("sphere") || n == QStringLiteral("sph")) {
        if (ok) *ok = true;
        return UvProject::Mode::Sphere;
    }
    if (n == QStringLiteral("reset") || n == QStringLiteral("reset_box")
        || n == QStringLiteral("resetbox")) {
        if (ok) *ok = true;
        return UvProject::Mode::ResetBox;
    }
    if (ok) *ok = false;
    return UvProject::Mode::Box;
}

UvPipeline::InfoReport UvPipeline::analyzeEntity(const Ogre::Entity* entity, int uvChannel)
{
    InfoReport report;
    if (!entity)
        return report;

    report.submeshes = UvUnwrap::infoForEntity(entity);

    EditableMesh mesh;
    if (!mesh.loadFromEntity(const_cast<Ogre::Entity*>(entity)))
        return report;

    if (uvChannel != 0) {
        for (auto& sub : mesh.subMeshes()) {
            for (auto& vert : sub.vertices) {
                vert.hasUV = false;
            }
        }
        applyUvChannelFromEntity(mesh, const_cast<Ogre::Entity*>(entity), uvChannel);
    }

    const auto islands = UVEditorController::computeIslandsFromEditableMesh(mesh);
    report.islandCount = islands.islandCount;
    report.overlappingUvsRatio = overlappingUvsRatioFromEditableMesh(mesh);
    return report;
}

QJsonObject UvPipeline::infoToJson(const QString& fileName, const InfoReport& report)
{
    QJsonObject root = UvUnwrap::infoToJson(fileName, report.submeshes);
    root[QStringLiteral("islandCount")] = report.islandCount;
    if (report.overlappingUvsRatio >= 0.0) {
        root[QStringLiteral("overlappingUvsRatio")] = report.overlappingUvsRatio;
        root[QStringLiteral("overlappingUvsPercent")] = report.overlappingUvsRatio * 100.0;
    }
    return root;
}

QString UvPipeline::infoToText(const QString& fileName, const InfoReport& report)
{
    QString text = UvUnwrap::infoToText(fileName, report.submeshes);
    text += QStringLiteral("Islands:       %1\n").arg(report.islandCount);
    if (report.overlappingUvsRatio >= 0.0) {
        text += QStringLiteral("UV overlap:    %1% (AABB upper bound)\n")
                    .arg(report.overlappingUvsRatio * 100.0, 0, 'f', 1);
    }
    return text;
}

UvProject::Report UvPipeline::projectEntity(Ogre::Entity* entity, UvProject::Mode mode,
                                            int uvChannel, const UvProject::Options& opts)
{
    UvProject::Report fail;
    if (!entity) {
        fail.error = QStringLiteral("Entity is null");
        return fail;
    }

    EditableMesh mesh;
    if (!mesh.loadFromEntity(entity)) {
        fail.error = QStringLiteral("Failed to load mesh from entity");
        return fail;
    }

    if (uvChannel != 0)
        applyUvChannelFromEntity(mesh, entity, uvChannel);

    UvProject::Options runOpts = opts;
    runOpts.mode = mode;
    const UvProject::Selection selection;
    const UvProject::Report report = UvProject::project(mesh, selection, runOpts);
    if (!report.applied)
        return report;

    if (!mesh.commitUvsToEntity(entity, uvChannel)) {
        fail.error = QStringLiteral("Failed to commit projected UVs");
        return fail;
    }

    return report;
}

bool UvPipeline::parseSeamEdgeList(const QString& spec, std::vector<SeamEdge>& out,
                                     QString* error)
{
    out.clear();
    const QString trimmed = spec.trimmed();
    if (trimmed.isEmpty()) {
        if (error)
            *error = QStringLiteral("Empty seam edge list");
        return false;
    }

    const QStringList tokens = trimmed.split(',', Qt::SkipEmptyParts);
    for (const QString& token : tokens) {
        const QString t = token.trimmed();
        int subMeshIndex = 0;
        QString edgePart = t;
        const int colon = t.indexOf(':');
        if (colon >= 0) {
            bool ok = false;
            subMeshIndex = t.left(colon).toInt(&ok);
            if (!ok || subMeshIndex < 0) {
                if (error)
                    *error = QStringLiteral("Invalid submesh in '%1'").arg(t);
                return false;
            }
            edgePart = t.mid(colon + 1);
        }

        const int dash = edgePart.indexOf('-');
        if (dash <= 0) {
            if (error)
                *error = QStringLiteral("Expected vertA-vertB in '%1'").arg(t);
            return false;
        }

        bool okA = false;
        bool okB = false;
        const unsigned int vertA = edgePart.left(dash).toUInt(&okA);
        const unsigned int vertB = edgePart.mid(dash + 1).toUInt(&okB);
        if (!okA || !okB) {
            if (error)
                *error = QStringLiteral("Invalid vertex indices in '%1'").arg(t);
            return false;
        }

        SeamEdge edge;
        edge.subMeshIndex = subMeshIndex;
        edge.vertA = vertA;
        edge.vertB = vertB;
        out.push_back(edge);
    }

    return !out.empty();
}

bool UvPipeline::setSeamsOnEntity(Ogre::Entity* entity, const std::vector<SeamEdge>& edges,
                                  QString* error)
{
    if (!entity) {
        if (error)
            *error = QStringLiteral("Entity is null");
        return false;
    }

    EditableMesh mesh;
    if (!mesh.loadFromEntity(entity)) {
        if (error)
            *error = QStringLiteral("Failed to load mesh from entity");
        return false;
    }

    for (const SeamEdge& edge : edges) {
        if (edge.subMeshIndex < 0
            || static_cast<size_t>(edge.subMeshIndex) >= mesh.subMeshes().size()) {
            if (error)
                *error = QStringLiteral("Submesh index out of range: %1").arg(edge.subMeshIndex);
            return false;
        }
        auto& sub = mesh.subMeshes()[static_cast<size_t>(edge.subMeshIndex)];
        const size_t maxVert = sub.vertices.size();
        if (edge.vertA >= maxVert || edge.vertB >= maxVert) {
            if (error)
                *error = QStringLiteral("Vertex index out of range on submesh %1")
                             .arg(edge.subMeshIndex);
            return false;
        }
        UvSeamData::setSeam(sub, edge.vertA, edge.vertB, true);
    }

    UvSeamData::writeBindingsToMesh(entity->getMesh().get(), mesh.subMeshes());
    return true;
}

UvUnwrapReport UvPipeline::unwrapEntity(Ogre::Entity* entity, const UvUnwrapOptions& opts)
{
    return UvUnwrap::unwrapEntity(entity, opts);
}

UvUnwrapReport UvPipeline::unwrapTriangles(Ogre::Entity* entity, int subMeshIndex,
                                           const std::vector<int>& triangleIndices,
                                           const UvUnwrapOptions& opts)
{
    UvUnwrapReport fail;
    if (!entity) {
        fail.error = QStringLiteral("Entity is null");
        return fail;
    }
    if (subMeshIndex < 0) {
        fail.error = QStringLiteral("Invalid submesh index");
        return fail;
    }
    const size_t subMeshCount = entity->getMesh()->getNumSubMeshes();
    if (static_cast<size_t>(subMeshIndex) >= subMeshCount) {
        fail.error = QStringLiteral("Invalid submesh index");
        return fail;
    }
    if (triangleIndices.empty()) {
        fail.error = QStringLiteral("No triangle indices provided");
        return fail;
    }

    UvUnwrapOptions runOpts = opts;
    runOpts.faceMasks.clear();
    runOpts.faceMasks.reserve(subMeshCount);

    for (size_t si = 0; si < subMeshCount; ++si) {
        const int triCount = static_cast<int>(
            entity->getMesh()->getSubMesh(static_cast<unsigned short>(si))->indexData->indexCount
            / 3);

        UvUnwrapOptions::FaceMask mask;
        mask.subMeshIndex = static_cast<unsigned>(si);
        mask.includeTriangle.assign(static_cast<size_t>(triCount), false);

        if (static_cast<int>(si) == subMeshIndex) {
            for (int ti : triangleIndices) {
                if (ti < 0 || ti >= triCount) {
                    fail.error = QStringLiteral("Triangle index out of range: %1").arg(ti);
                    return fail;
                }
                mask.includeTriangle[static_cast<size_t>(ti)] = true;
            }
        }

        runOpts.faceMasks.push_back(std::move(mask));
    }

    EditableMesh mesh;
    if (mesh.loadFromEntity(entity)) {
        runOpts.seamEdgeKeys.resize(mesh.subMeshes().size());
        for (size_t si = 0; si < mesh.subMeshes().size(); ++si) {
            runOpts.seamEdgeKeys[si].assign(mesh.subMeshes()[si].seamEdges.begin(),
                                            mesh.subMeshes()[si].seamEdges.end());
        }
    }

    return UvUnwrap::unwrapEntity(entity, runOpts);
}
