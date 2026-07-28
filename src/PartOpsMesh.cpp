#include "PartOpsMesh.h"

#include "EditableMesh.h"
#include "SentryReporter.h"

#include <OgreEntity.h>
#include <OgreSubEntity.h>
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
    if (outSubMeshes.empty())
        return false;

    // EditableMesh reads the SubMesh's base material name, but a material
    // assigned at runtime via Material Mode lives on the Entity's SubEntity
    // (ApplyMaterialCommand mutates the SubEntity, not the SubMesh). Prefer the
    // effective SubEntity material so split/explode/join keep what the user
    // sees — otherwise join would coalesce visibly-distinct parts and rebuild
    // with stale bindings. SubEntity order matches SubMesh (== editable submesh)
    // order, so the mapping is positional.
    const unsigned short n = entity->getNumSubEntities();
    for (unsigned short i = 0; i < n && i < outSubMeshes.size(); ++i) {
        Ogre::SubEntity* se = entity->getSubEntity(i);
        if (!se)
            continue;
        const std::string effective = se->getMaterialName();
        if (!effective.empty())
            outSubMeshes[i].materialName = effective;
    }
    return true;
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

PartOpsMesh::PrintPrepOutcome
PartOpsMesh::addPrintPegsToEntity(Ogre::Entity* entity, const SubMeshOps::PegOptions& opts,
                                  const std::string& baseName)
{
    PrintPrepOutcome out;
    if (!entity || !entity->getMesh()) {
        out.error = QStringLiteral("no entity");
        return out;
    }
    if (entity->getMesh()->getNumSubMeshes() < 2) {
        out.error = QStringLiteral("mesh has a single part — split it into parts first");
        return out;
    }
    std::vector<EditableSubMesh> src;
    if (!readSubMeshes(entity, src)) {
        out.error = QStringLiteral("could not read mesh geometry from entity");
        return out;
    }

    // Recover per-part names from the mesh's submesh name map (a prior split
    // named them head/torso/…), else positional. Used for the connector naming
    // + boundary report.
    const auto& nameMap = entity->getMesh()->getSubMeshNameMap();
    std::vector<QString> names(src.size());
    for (const auto& kv : nameMap)
        if (kv.second < names.size())
            names[kv.second] = QString::fromStdString(kv.first);
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i].isEmpty())
            names[i] = QStringLiteral("part%1").arg(i);

    SubMeshOps::PrintPrepResult prep = SubMeshOps::preparePrintPegs(src, opts, names);
    if (!prep.ok && prep.subMeshes.empty()) {
        out.error = prep.error;
        return out;
    }
    for (const auto& b : prep.boundaries)
        if (!b.pegged)
            out.warnings.push_back(QStringLiteral("%1↔%2: %3").arg(b.nameA, b.nameB, b.reason));

    QString skelName;
    if (entity->getMesh()->hasSkeleton())
        skelName = QString::fromStdString(entity->getMesh()->getSkeletonName());
    Ogre::MeshPtr mesh = buildMesh(prep.subMeshes, baseName, skelName, prep.partNames);
    if (!mesh) {
        out.error = QStringLiteral("failed to build pegged mesh");
        return out;
    }

    out.ok = true;   // the op ran; peggedBoundaries==0 means no safe boundary.
    out.mesh = mesh;
    out.partNames = std::move(prep.partNames);
    out.peggedBoundaries = prep.peggedBoundaries;
    out.totalPegs = prep.totalPegs;

    // Telemetry for the operation itself so EVERY caller (CLI/MCP/command) gets a
    // breadcrumb, not just the undo command's redo() (CodeRabbit).
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.parts.print_pegs"),
                                  QStringLiteral("boundaries=%1 pegs=%2 capped=%3 warnings=%4")
                                      .arg(out.peggedBoundaries).arg(out.totalPegs)
                                      .arg(prep.cappedParts)
                                      .arg(static_cast<int>(out.warnings.size())));
    return out;
}
