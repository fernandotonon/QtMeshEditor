#include "DnaScene.h"

#include "../EditableMesh.h"
#include "../Manager.h"
#include "../PartOpsMesh.h"
#include "../SentryReporter.h"
#include "DnaCorrectiveManager.h"
#include "DnaRigLogic.h"
#include "../commands/MorphCommands.h"

#include <QFileInfo>

#include <OgreException.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSceneNode.h>
#include <OgreSkeleton.h>
#include <OgreSkeletonManager.h>
#include <OgreResourceGroupManager.h>

#include <algorithm>
#include <map>
#include <vector>

namespace Dna {

namespace {

constexpr size_t kMaxInfluences = 4;  // Ogre hardware skinning limit

Ogre::SceneNode* fail(QString* error, const QString& msg)
{
    if (error) *error = msg;
    return nullptr;
}

}  // namespace

namespace {

constexpr int kOgreMaxBones = 256;  // OGRE_MAX_NUM_BONES: handles 0..255 are valid, i.e. 256 bones fit

// Keep only the joints that deform the imported meshes (skinned joints and
// their ancestors). MetaHuman skeletons carry hundreds of driver/helper joints
// with no skin weights (body 342, face 870 joints) that Ogre cannot hold and
// that do not move any vertex.
Rig pruneToDeformers(const Rig& rig, ImportReport& rep)
{
    std::vector<char> used(rig.joints.size(), 0);
    for (const Mesh& m : rig.meshes)
        for (const auto& inf : m.skin)
            for (const SkinInfluence& i : inf)
                if (i.joint < used.size() && i.weight > 0.0f) used[i.joint] = 1;
    for (size_t i = 0; i < used.size(); ++i) {
        if (!used[i]) continue;
        for (int p = rig.joints[i].parent; p >= 0 && static_cast<size_t>(p) < used.size() && !used[size_t(p)];
             p = rig.joints[size_t(p)].parent)
            used[size_t(p)] = 1;
    }
    size_t kept = 0;
    for (char u : used) kept += u ? 1 : 0;
    if (kept == 0)
        return rig;
    const size_t afterAncestors = kept;

    // Still over Ogre's bone limit: fold the least-influential LEAF joints into
    // their parent (lossy — their skin weight moves to the parent). Leaves only,
    // so the hierarchy stays valid; weakest first, with a folded joint's weight
    // added to its parent so a branch is judged by what it carries as a whole.
    std::vector<int> fold(rig.joints.size());  // joint -> kept joint it now maps to
    for (size_t i = 0; i < fold.size(); ++i) fold[i] = int(i);
    size_t folded = 0;
    if (kept > size_t(kOgreMaxBones)) {
        std::vector<double> total(rig.joints.size(), 0.0);
        for (const Mesh& m : rig.meshes)
            for (const auto& inf : m.skin)
                for (const SkinInfluence& i : inf)
                    if (i.joint < total.size()) total[i.joint] += i.weight;
        std::vector<int> children(rig.joints.size(), 0);
        for (size_t i = 0; i < used.size(); ++i)
            if (used[i] && rig.joints[i].parent >= 0) ++children[size_t(rig.joints[i].parent)];
        while (kept > size_t(kOgreMaxBones)) {
            int best = -1;
            for (size_t i = 0; i < used.size(); ++i) {
                if (!used[i] || children[i] != 0 || rig.joints[i].parent < 0) continue;
                // Rig-driven joints (drivers + correctives) go last; within a
                // class the least-weighted leaf goes first.
                const bool prot = i < rig.protectedJoint.size() && rig.protectedJoint[i];
                const bool bestProt = best >= 0 && size_t(best) < rig.protectedJoint.size()
                                      && rig.protectedJoint[size_t(best)];
                if (best < 0 || (prot != bestProt ? !prot : total[i] < total[size_t(best)])) best = int(i);
            }
            if (best < 0) break;
            const int par = rig.joints[size_t(best)].parent;
            used[size_t(best)] = 0;
            total[size_t(par)] += total[size_t(best)];
            --children[size_t(par)];
            fold[size_t(best)] = par;
            --kept;
            ++folded;
        }
        // Resolve chains (a folded joint may have been folded into one folded later).
        for (size_t i = 0; i < fold.size(); ++i)
            while (!used[size_t(fold[i])] && rig.joints[size_t(fold[i])].parent >= 0)
                fold[i] = rig.joints[size_t(fold[i])].parent;
    }
    if (kept == rig.joints.size())
        return rig;

    std::vector<int> remap(rig.joints.size(), -1);
    Rig out = rig;
    out.joints.clear();
    for (size_t i = 0; i < rig.joints.size(); ++i) {
        if (!used[i]) continue;
        remap[i] = static_cast<int>(out.joints.size());
        out.joints.push_back(rig.joints[i]);
    }
    for (Joint& j : out.joints) {
        int p = j.parent;
        while (p >= 0 && remap[size_t(p)] < 0) p = rig.joints[size_t(p)].parent;
        j.parent = p >= 0 ? remap[size_t(p)] : -1;
    }
    for (Mesh& m : out.meshes)
        for (auto& inf : m.skin) {
            std::vector<SkinInfluence> merged;
            for (const SkinInfluence& i : inf) {
                if (i.joint >= fold.size()) continue;
                const int target = remap[size_t(fold[i.joint])];
                if (target < 0) continue;
                bool found = false;
                for (SkinInfluence& e : merged)
                    if (e.joint == target) { e.weight += i.weight; found = true; break; }
                if (!found) merged.push_back({static_cast<uint16_t>(target), i.weight});
            }
            inf = std::move(merged);
        }
    out.lodJointIndices.clear();
    for (int j : rig.lodJointIndices)
        if (j >= 0 && size_t(j) < fold.size() && remap[size_t(fold[size_t(j)])] >= 0)
            out.lodJointIndices.push_back(remap[size_t(fold[size_t(j)])]);
    out.lodJointIndices.erase(std::unique(out.lodJointIndices.begin(), out.lodJointIndices.end()),
                              out.lodJointIndices.end());
    rep.warnings << QStringLiteral("Dropped %1 of %2 joints that carry no skin weights (driver/helper joints).")
                        .arg(rig.joints.size() - afterAncestors).arg(rig.joints.size());
    if (folded > 0)
        rep.warnings << QStringLiteral("LOSSY: Ogre allows at most %1 bones per skeleton, so %2 further "
                                       "low-influence leaf joints (twist/corrective joints) were folded into "
                                       "their parents. Deformation differs slightly from the DNA rig.")
                            .arg(kOgreMaxBones).arg(folded);
    return out;
}

Ogre::SceneNode* importRigImpl(const Rig& rig, const QString& baseName,
                               ImportReport* report, QString* error);

}  // namespace

Ogre::SceneNode* importRigToScene(const Rig& input, const QString& baseName,
                                  ImportReport* report, QString* error,
                                  std::shared_ptr<RigEvaluator> evaluator)
{
    ImportReport local;
    ImportReport& rep = report ? *report : local;
    rep = ImportReport();
    const Rig rig = pruneToDeformers(input, rep);
    if (static_cast<int>(rig.joints.size()) > kOgreMaxBones) {
        if (error)
            *error = QStringLiteral("The skeleton still has %1 joints after dropping non-deforming ones; "
                                    "Ogre supports at most %2 bones per skeleton.")
                         .arg(rig.joints.size()).arg(kOgreMaxBones);
        return nullptr;
    }
    const QStringList pruneNotes = rep.warnings;
    try {
        Ogre::SceneNode* n = importRigImpl(rig, baseName, &rep, error);
        rep.warnings = pruneNotes + rep.warnings;
        if (n && evaluator) {
            DnaCorrectiveManager* mgr = DnaCorrectiveManager::instance();
            const QString entityName = QString::fromStdString(n->getName());
            if (mgr->attach(entityName, evaluator)) {
                mgr->evaluateNow();   // populate the stats the report quotes
                const DnaCorrectiveManager::Stats st = mgr->statsFor(entityName);
                rep.correctivesAttached = true;
                rep.correctiveJoints = st.driven;
                rep.correctiveJointsFolded = st.missingDriven;
                if (st.missingDriven > 0)
                    rep.warnings << QStringLiteral(
                        "%1 of %2 rig-driven corrective joints do not fit Ogre's %3-bone limit and are not "
                        "evaluated; the other %4 are.").arg(st.missingDriven).arg(st.driven + st.missingDriven)
                        .arg(kOgreMaxBones).arg(st.driven);
            }
        }
        return n;
    } catch (const Ogre::Exception& e) {
        if (error) *error = QStringLiteral("Ogre error while building the DNA character: %1")
                                .arg(QString::fromStdString(e.getDescription()));
        return nullptr;
    }
}

namespace {

Ogre::SceneNode* importRigImpl(const Rig& rig, const QString& baseName,
                               ImportReport* report, QString* error)
{
    ImportReport local;
    ImportReport& rep = report ? *report : local;
    rep.warnings = rig.warnings;

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr())
        return fail(error, QStringLiteral("No active scene to import into."));
    if (rig.meshes.empty())
        return fail(error, QStringLiteral("The DNA has no meshes at LOD %1 to import.").arg(rig.lod));
    if (rig.joints.empty())
        return fail(error, QStringLiteral("The DNA has no joints."));

    // ---- skeleton (bone handle == DNA joint index) --------------------------
    auto& skelMgr = Ogre::SkeletonManager::getSingleton();
    const std::string skelName = (baseName + QStringLiteral("_dna_skeleton")).toStdString();
    if (skelMgr.resourceExists(skelName))
        skelMgr.remove(skelName);
    Ogre::SkeletonPtr skel = skelMgr.create(
        skelName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    std::vector<Ogre::Bone*> bones(rig.joints.size(), nullptr);
    for (size_t i = 0; i < rig.joints.size(); ++i)
        bones[i] = skel->createBone(rig.joints[i].name.toStdString(), static_cast<unsigned short>(i));
    for (size_t i = 0; i < rig.joints.size(); ++i) {
        const Joint& j = rig.joints[i];
        if (j.parent >= 0 && static_cast<size_t>(j.parent) < bones.size() && static_cast<size_t>(j.parent) != i)
            bones[static_cast<size_t>(j.parent)]->addChild(bones[i]);
        bones[i]->setPosition(Ogre::Vector3(float(j.translation[0]), float(j.translation[1]), float(j.translation[2])));
        bones[i]->setOrientation(Ogre::Quaternion(float(j.orientation[0]), float(j.orientation[1]),
                                                  float(j.orientation[2]), float(j.orientation[3])));
    }
    skel->setBindingPose();
    rep.joints = static_cast<int>(rig.joints.size());

    // ---- geometry -----------------------------------------------------------
    std::vector<EditableSubMesh> subs;
    std::vector<QString> names;
    std::vector<size_t> builtMeshIndex;  // rig.meshes index of each built submesh
    std::vector<ExpandedMesh> expanded;
    for (size_t mi = 0; mi < rig.meshes.size(); ++mi) {
        const Mesh& m = rig.meshes[mi];
        ExpandedMesh ex = expand(m, rig.flipV);
        if (ex.indices.empty()) {
            rep.warnings << QStringLiteral("Mesh '%1' has no valid triangles and was skipped.").arg(m.name);
            continue;
        }
        EditableSubMesh es;
        es.materialName = "BaseWhite";
        const size_t nv = ex.positionOf.size();
        es.vertices.resize(nv);
        for (size_t v = 0; v < nv; ++v) {
            EditableVertex& ev = es.vertices[v];
            ev.position = Ogre::Vector3(ex.positions[v * 3], ex.positions[v * 3 + 1], ex.positions[v * 3 + 2]);
            ev.normal = Ogre::Vector3(ex.normals[v * 3], ex.normals[v * 3 + 1], ex.normals[v * 3 + 2]);
            ev.uv = Ogre::Vector2(ex.uvs[v * 2], ex.uvs[v * 2 + 1]);
            ev.hasNormal = true;
            ev.hasUV = true;
            if (v < ex.skin.size() && !ex.skin[v].empty()) {
                std::vector<SkinInfluence> inf = ex.skin[v];
                rep.maxInfluencesSeen = std::max(rep.maxInfluencesSeen, static_cast<int>(inf.size()));
                std::sort(inf.begin(), inf.end(),
                          [](const SkinInfluence& a, const SkinInfluence& b) { return a.weight > b.weight; });
                if (inf.size() > kMaxInfluences) {
                    inf.resize(kMaxInfluences);
                    ++rep.truncatedInfluenceVertices;
                }
                float sum = 0.0f;
                for (const auto& i : inf) sum += i.weight;
                if (sum <= 0.0f) continue;
                for (const auto& i : inf) {
                    if (i.joint >= rig.joints.size() || i.weight <= 0.0f) continue;
                    ev.boneAssignments.push_back({i.joint, i.weight / sum});
                }
            }
        }
        for (size_t t = 0; t + 2 < ex.indices.size(); t += 3) {
            EditableTriangle tri;
            tri.indices[0] = ex.indices[t];
            tri.indices[1] = ex.indices[t + 1];
            tri.indices[2] = ex.indices[t + 2];
            es.triangles.push_back(tri);
        }
        rep.vertices += static_cast<int>(nv);
        rep.triangles += static_cast<int>(es.triangles.size());
        rep.skippedFaces += ex.skippedFaces;
        subs.push_back(std::move(es));
        names.push_back(m.name);
        builtMeshIndex.push_back(mi);
        expanded.push_back(std::move(ex));
    }
    if (subs.empty())
        return fail(error, QStringLiteral("No DNA mesh produced valid triangles."));
    if (rep.truncatedInfluenceVertices > 0)
        rep.warnings << QStringLiteral("%1 vertices carry more than %2 skin influences (max %3); only the %2 "
                                       "strongest were kept (Ogre hardware-skinning limit).")
                            .arg(rep.truncatedInfluenceVertices).arg(kMaxInfluences).arg(rep.maxInfluencesSeen);
    if (rep.skippedFaces > 0)
        rep.warnings << QStringLiteral("%1 faces with invalid indices were dropped.").arg(rep.skippedFaces);

    const std::string meshName = (baseName + QStringLiteral("_dna")).toStdString();
    // No skeleton name: the skeleton is a manual resource, bound below.
    Ogre::MeshPtr mesh = PartOpsMesh::buildMesh(subs, meshName, QString(), names, /*recomputeNormals=*/false);
    if (!mesh)
        return fail(error, QStringLiteral("Failed to build the Ogre mesh from the DNA geometry."));

    mesh->_notifySkeleton(skel);
    mesh->clearBoneAssignments();
    for (unsigned short i = 0; i < mesh->getNumSubMeshes() && i < subs.size(); ++i) {
        Ogre::SubMesh* sm = mesh->getSubMesh(i);
        sm->clearBoneAssignments();
        for (size_t vi = 0; vi < subs[i].vertices.size(); ++vi) {
            for (const auto& ba : subs[i].vertices[vi].boneAssignments) {
                Ogre::VertexBoneAssignment vba;
                vba.vertexIndex = static_cast<unsigned int>(vi);
                vba.boneIndex = ba.boneIndex;
                vba.weight = ba.weight;
                sm->addBoneAssignment(vba);
            }
        }
    }
    mesh->_compileBoneAssignments();
    rep.meshes = static_cast<int>(subs.size());

    Ogre::SceneNode* node = mgr->addSceneNode(baseName + QStringLiteral("_dna_node"));
    if (!node)
        return fail(error, QStringLiteral("Failed to create the scene node."));
    Ogre::Entity* entity = mgr->createEntity(node, mesh);

    // ---- morph targets: DNA channel -> one named pose per mesh slice ---------
    if (entity && !rig.blendShapeChannels.isEmpty()) {
        std::map<int, std::vector<MorphPoseSlice>> byChannel;
        for (size_t sub = 0; sub < builtMeshIndex.size(); ++sub) {
            const Mesh& m = rig.meshes[builtMeshIndex[sub]];
            const ExpandedMesh& ex = expanded[sub];
            std::vector<std::vector<uint32_t>> verticesOfPosition;
            for (uint32_t v = 0; v < ex.positionOf.size(); ++v) {
                if (ex.positionOf[v] >= verticesOfPosition.size())
                    verticesOfPosition.resize(ex.positionOf[v] + 1);
                verticesOfPosition[ex.positionOf[v]].push_back(v);
            }
            for (const BlendShapeTarget& bt : m.blendShapeTargets) {
                if (bt.channel < 0 || bt.channel >= rig.blendShapeChannels.size()) continue;
                MorphPoseSlice slice;
                slice.submeshHandle = static_cast<unsigned short>(sub + 1);
                for (size_t k = 0; k < bt.vertexIndices.size(); ++k) {
                    const uint32_t p = bt.vertexIndices[k];
                    if (p >= verticesOfPosition.size()) continue;
                    const Ogre::Vector3f d(bt.deltas[k * 3], bt.deltas[k * 3 + 1], bt.deltas[k * 3 + 2]);
                    for (const uint32_t v : verticesOfPosition[p])
                        slice.offsets[v] = d;
                }
                if (!slice.offsets.empty())
                    byChannel[bt.channel].push_back(std::move(slice));
            }
        }
        for (auto& [channel, slices] : byChannel) {
            AddMorphTargetCommand cmd(entity, rig.blendShapeChannels[channel], slices);
            cmd.setDeferInit(true);
            cmd.redo();
            ++rep.morphTargets;
        }
        if (rep.morphTargets > 0) {
            entity->_initialise(true);
            entity->refreshAvailableAnimationState();
        }
    }

    SentryReporter::addBreadcrumb(
        QStringLiteral("file.import"),
        QStringLiteral("DNA: %1 joints, %2 meshes, %3 verts, %4 morph targets")
            .arg(rep.joints).arg(rep.meshes).arg(rep.vertices).arg(rep.morphTargets));
    return node;
}

}  // namespace

Ogre::SceneNode* importToScene(const QString& path, const ReadOptions& options,
                               ImportReport* report, QString* error)
{
    ReadResult rr = read(path, options);
    if (!rr.ok)
        return fail(error, rr.error);

    // A DNA with rig-logic behaviour (a MetaHuman body) gets live correctives.
    // The evaluator also tells the importer which joints the rig reads or moves,
    // so a skeleton that must be cut down to Ogre's bone limit folds those last.
    // A rig without quaternion drivers (a head) is still imported, just static.
    std::shared_ptr<RigEvaluator> evaluator;
    if (available()) {
        QString evErr;
        evaluator = std::shared_ptr<RigEvaluator>(RigEvaluator::create(path, options, &evErr).release());
        if (evaluator && !evaluator->driverJoints().isEmpty() && !evaluator->drivenJoints().empty()) {
            rr.rig.protectedJoint.assign(rr.rig.joints.size(), 0);
            for (const QString& d : evaluator->driverJoints()) {
                const int j = evaluator->jointIndex(d);
                if (j >= 0 && size_t(j) < rr.rig.protectedJoint.size()) rr.rig.protectedJoint[size_t(j)] = 1;
            }
            for (const int j : evaluator->drivenJoints())
                if (j >= 0 && size_t(j) < rr.rig.protectedJoint.size()) rr.rig.protectedJoint[size_t(j)] = 1;
        } else {
            evaluator.reset();
        }
    }
    return importRigToScene(rr.rig, QFileInfo(path).completeBaseName(), report, error, evaluator);
}

}  // namespace Dna
