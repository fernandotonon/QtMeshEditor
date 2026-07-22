#include "PartOpsMesh.h"

#include "EditableMesh.h"

#include <OgreEntity.h>
#include <OgreSubMesh.h>
#include <OgreException.h>

#include <memory>

bool PartOpsMesh::readSubMeshes(Ogre::Entity* entity,
                                std::vector<EditableSubMesh>& outSubMeshes)
{
    outSubMeshes.clear();
    if (!entity || !entity->getMesh())
        return false;
    EditableMesh em;
    if (!em.loadFromEntity(entity))
        return false;
    outSubMeshes = em.subMeshes();
    return !outSubMeshes.empty();
}

Ogre::MeshPtr PartOpsMesh::buildMesh(const std::vector<EditableSubMesh>& subMeshes,
                                     const std::string& baseName,
                                     const QString& skeletonName)
{
    if (subMeshes.empty())
        return Ogre::MeshPtr();
    // EditableMesh::createNewMesh is the canonical build-a-fresh-mesh path
    // (createSubMesh per part + buildSubMeshBuffers + normals + bounds). We
    // borrow it by seeding an EditableMesh's submesh vector directly.
    EditableMesh em;
    em.subMeshes() = subMeshes;
    Ogre::MeshPtr mesh = em.createNewMesh(baseName);
    if (!mesh)
        return mesh;

    // createNewMesh only authors geometry; a SKINNED source needs its skeleton
    // rebound and bone assignments recompiled onto the new mesh (mirrors
    // EditableMesh::resizeEntityBuffers). Split preserves bone indices, and
    // every part references the same source skeleton, so the assignments stay
    // valid without renumbering.
    if (!skeletonName.isEmpty()) {
        // setSkeletonName loads the skeleton resource lazily; if it can't be
        // resolved it throws — catch and return a geometry-only mesh rather
        // than failing the whole split.
        try {
            mesh->setSkeletonName(skeletonName.toStdString());
        } catch (const Ogre::Exception&) {
            return mesh;
        }
        if (mesh->hasSkeleton()) {
            mesh->clearBoneAssignments();
            for (unsigned short i = 0; i < mesh->getNumSubMeshes()
                 && i < static_cast<unsigned short>(subMeshes.size()); ++i) {
                Ogre::SubMesh* sm = mesh->getSubMesh(i);
                const EditableSubMesh& es = subMeshes[i];
                sm->clearBoneAssignments();
                for (size_t vi = 0; vi < es.vertices.size(); ++vi) {
                    for (const auto& ba : es.vertices[vi].boneAssignments) {
                        Ogre::VertexBoneAssignment vba;
                        vba.vertexIndex = static_cast<unsigned int>(vi);
                        vba.boneIndex = ba.boneIndex;
                        vba.weight = ba.weight;
                        sm->addBoneAssignment(vba);
                    }
                }
            }
            mesh->_compileBoneAssignments();
        }
    }
    return mesh;
}

PartOpsMesh::SplitOutcome
PartOpsMesh::splitEntity(Ogre::Entity* entity,
                         const std::vector<int>& faceLabels,
                         const std::vector<SubMeshOps::FaceGroup>& groups,
                         const SubMeshOps::SplitOptions& opts,
                         const std::string& baseName)
{
    SplitOutcome out;
    std::vector<EditableSubMesh> src;
    if (!readSubMeshes(entity, src)) {
        out.error = QStringLiteral("could not read mesh geometry from entity");
        return out;
    }

    SubMeshOps::SplitResult split =
        SubMeshOps::splitByFaceGroups(src, faceLabels, groups, opts);
    if (!split.ok) {
        out.error = split.error;
        return out;
    }

    QString skelName;
    if (entity->getMesh() && entity->getMesh()->hasSkeleton())
        skelName = QString::fromStdString(entity->getMesh()->getSkeletonName());
    Ogre::MeshPtr mesh = buildMesh(split.subMeshes, baseName, skelName);
    if (!mesh) {
        out.error = QStringLiteral("failed to build split mesh");
        return out;
    }

    out.ok = true;
    out.mesh = mesh;
    out.partNames = std::move(split.partNames);
    out.createdSubMeshes = split.createdSubMeshes;
    out.duplicatedBoundaryVertices = split.duplicatedBoundaryVertices;
    return out;
}
