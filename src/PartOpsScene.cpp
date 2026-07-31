#include "PartOpsScene.h"

#include "PartOpsMesh.h"
#include "EditableMesh.h"

#include <OgreEntity.h>
#include <OgreSubMesh.h>
#include <OgreSceneNode.h>

#include <vector>

namespace {

// Centroid of a single editable submesh's vertices (local space).
Ogre::Vector3 subMeshCentroid(const EditableSubMesh& sub)
{
    if (sub.vertices.empty())
        return Ogre::Vector3::ZERO;
    Ogre::Vector3 acc = Ogre::Vector3::ZERO;
    for (const auto& v : sub.vertices)
        acc += v.position;
    return acc / static_cast<float>(sub.vertices.size());
}

} // namespace

PartOpsScene::ExplodeResult
PartOpsScene::explodeEntity(Ogre::Entity* entity, float distance,
                           const std::string& baseName)
{
    ExplodeResult out;
    if (!entity || !entity->getMesh()) {
        out.error = QStringLiteral("no entity to explode");
        return out;
    }

    std::vector<EditableSubMesh> subs;
    if (!PartOpsMesh::readSubMeshes(entity, subs)) {
        out.error = QStringLiteral("could not read mesh geometry from entity");
        return out;
    }
    if (subs.size() < 2) {
        out.error = QStringLiteral("mesh has a single part — split it first");
        return out;
    }

    QString skelName;
    if (entity->getMesh()->hasSkeleton())
        skelName = QString::fromStdString(entity->getMesh()->getSkeletonName());

    // Explode offsets are computed from each part's centroid vs the assembly
    // centroid, so an isolated part is pushed radially outward. Bounds come
    // from every vertex across every submesh (the assembly AABB).
    std::vector<Ogre::Vector3> centroids;
    centroids.reserve(subs.size());
    Ogre::AxisAlignedBox bounds;
    for (const auto& s : subs) {
        centroids.push_back(subMeshCentroid(s));
        for (const auto& v : s.vertices)
            bounds.merge(v.position);
    }
    const std::vector<Ogre::Vector3> offsets =
        SubMeshOps::explodeOffsets(centroids, bounds, distance);

    // Prefer the registered per-submesh name (a prior split names them
    // head/torso/…); fall back to a positional label.
    const auto& nameMap = entity->getMesh()->getSubMeshNameMap();
    auto partName = [&](unsigned short i) -> QString {
        for (const auto& kv : nameMap)
            if (kv.second == i)
                return QString::fromStdString(kv.first);
        return QStringLiteral("part%1").arg(i);
    };

    out.parts.reserve(subs.size());
    for (size_t i = 0; i < subs.size(); ++i) {
        const QString name = partName(static_cast<unsigned short>(i));
        // Build a one-submesh mesh from just this part. buildMesh takes a
        // vector, so hand it a single-element slice; the part name is carried
        // so nameSubMesh(0) stamps it (and it round-trips through export).
        std::vector<EditableSubMesh> one{ subs[i] };
        Ogre::MeshPtr mesh = PartOpsMesh::buildMesh(
            one, baseName + "_" + name.toStdString(), skelName, { name });
        if (!mesh) {
            out.error = QStringLiteral("failed to build part mesh for '%1'").arg(name);
            out.parts.clear();
            return out;
        }
        ExplodePart p;
        p.mesh = mesh;
        p.name = name;
        p.offset = offsets[i];
        out.parts.push_back(std::move(p));
    }

    out.ok = true;
    return out;
}

PartOpsScene::JoinResult
PartOpsScene::joinEntities(const std::vector<Ogre::Entity*>& entities, const std::string& baseName)
{
    JoinResult out;
    if (entities.size() < 2) {
        out.error = QStringLiteral("select at least two parts to join");
        return out;
    }

    std::vector<SubMeshOps::JoinPart> parts;
    parts.reserve(entities.size());
    for (Ogre::Entity* e : entities) {
        if (!e || !e->getMesh()) {
            out.error = QStringLiteral("a selected object is not a mesh");
            return out;
        }
        SubMeshOps::JoinPart jp;
        if (!PartOpsMesh::readSubMeshes(e, jp.subMeshes)) {
            out.error = QStringLiteral("could not read geometry from '%1'")
                            .arg(QString::fromStdString(e->getName()));
            return out;
        }
        // Bake the part's FULL world transform (node hierarchy included) so a
        // moved/rotated exploded part lands in the right place in the merged
        // mesh. _getFullTransform is the node's world affine; joinParts uses
        // it for positions and its inverse-transpose for normals/tangents.
        Ogre::SceneNode* node = e->getParentSceneNode();
        jp.transform = node ? Ogre::Matrix4(node->_getFullTransform())
                            : Ogre::Matrix4::IDENTITY;
        parts.push_back(std::move(jp));
    }

    SubMeshOps::JoinResult jr = SubMeshOps::joinParts(parts);
    if (!jr.ok) {
        out.error = jr.error.isEmpty() ? QStringLiteral("join failed") : jr.error;
        return out;
    }

    // Join produces static geometry — no skeleton reconciliation (documented
    // limitation), so buildMesh is called without a skeleton name.
    Ogre::MeshPtr mesh = PartOpsMesh::buildMesh(jr.subMeshes, baseName);
    if (!mesh) {
        out.error = QStringLiteral("failed to build joined mesh");
        return out;
    }

    out.ok = true;
    out.mesh = mesh;
    out.createdSubMeshes = static_cast<int>(mesh->getNumSubMeshes());
    return out;
}
