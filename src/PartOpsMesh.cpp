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
                                     const QString& skeletonName,
                                     const std::vector<QString>& subMeshNames)
{
    if (subMeshes.empty())
        return Ogre::MeshPtr();
    // EditableMesh::createNewMesh is the canonical build-a-fresh-mesh path
    // (createSubMesh per part + buildSubMeshBuffers + normals + bounds). We
    // borrow it by seeding an EditableMesh's submesh vector directly.
    EditableMesh em;
    em.subMeshes() = subMeshes;
    // recomputeNormals=false: SubMeshOps copied the source normals (incl.
    // authored / hard-edge normals) verbatim, so recomputing would change the
    // shading the split is meant to preserve (#859 review).
    Ogre::MeshPtr mesh = em.createNewMesh(baseName, /*recomputeNormals=*/false);
    if (!mesh)
        return mesh;

    // Register each part's name on the submesh (Mesh::nameSubMesh) so the Scene
    // tree shows "head"/"torso"/… instead of a positional index, and the name
    // round-trips through FBX export (FBXExporter reads getSubMeshNameMap) →
    // reimport (MeshProcessor reads aiMesh::mName). NB createNewMesh skips empty
    // editable submeshes, so guard on the built count.
    for (unsigned short i = 0;
         i < mesh->getNumSubMeshes() && i < static_cast<unsigned short>(subMeshNames.size());
         ++i) {
        if (!subMeshNames[i].isEmpty())
            mesh->nameSubMesh(subMeshNames[i].toStdString(), i);
    }

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
    Ogre::MeshPtr mesh = buildMesh(split.subMeshes, baseName, skelName, split.partNames);
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
