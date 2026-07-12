/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "MorphCommands.h"

#include "../PropertiesPanelController.h"
#include "../SentryReporter.h"

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreAnimationTrack.h>
#include <OgreEntity.h>
#include <OgreKeyFrame.h>
#include <OgreMesh.h>
#include <OgrePose.h>

namespace {

// Snapshot every same-named pose on the mesh into the slice list,
// preserving submesh handle and the sparse offset map. The importer's
// pattern is one Animation per unique name with one VAT_POSE track per
// affected submesh — same-named poses across multiple submeshes are
// driven together by that single Animation. Undo / rename rebuilds
// must mirror that, so the snapshot is per-pose, not per-animation.
std::vector<MorphPoseSlice> snapshotByName(Ogre::Mesh* mesh, const std::string& name)
{
    std::vector<MorphPoseSlice> out;
    if (!mesh) return out;
    const auto& poseList = mesh->getPoseList();
    for (const Ogre::Pose* p : poseList) {
        if (!p || p->getName() != name) continue;
        MorphPoseSlice slice;
        slice.submeshHandle = p->getTarget();
        slice.offsets = p->getVertexOffsets();
        out.push_back(std::move(slice));
    }
    return out;
}

// Drop every same-named pose + the matching Animation (which the
// importer always creates 1:1 with the target name). Reset matching
// AnimationStates so weight sliders read 0 after the delete.
void removePosesByName(Ogre::Mesh* mesh, const QString& name, Ogre::Entity* entity)
{
    if (!mesh) return;
    const std::string sn = name.toStdString();

    // STOP PLAYBACK first. The render frame loop (MainWindow::
    // frameRenderingQueued) iterates the entity's AnimationStateSet and reads
    // each VAT_POSE track's pose references every frame. Removing a pose /
    // animation / state here while a clip is playing frees data the loop is
    // mid-read of → crash (reproduced: play a morph/vertex clip, delete a
    // target). deleteAnimation/renameAnimation already stop playback before
    // mutating; do the same at this shared mutation point so every entry
    // (delete + rename redo/undo) is safe. Also disable the state before
    // removing it so no dangling enabled state survives the refresh.
    if (auto* ppc = PropertiesPanelController::instance())
        ppc->setPlaying(false);
    if (entity) {
        if (auto* states = entity->getAllAnimationStates()) {
            if (states->hasAnimationState(sn))
                states->getAnimationState(sn)->setEnabled(false);
        }
    }

    // removePose(name) only removes the *first* pose with that name —
    // when an importer pose has the same name across multiple submeshes
    // we'd leak the rest. Walk + remove by index from the back so the
    // ushort indices stay stable for the remaining entries.
    const auto& poseList = mesh->getPoseList();
    std::vector<unsigned short> indicesToDrop;
    for (unsigned short pi = 0; pi < poseList.size(); ++pi) {
        if (poseList[pi] && poseList[pi]->getName() == sn)
            indicesToDrop.push_back(pi);
    }
    for (auto it = indicesToDrop.rbegin(); it != indicesToDrop.rend(); ++it)
        mesh->removePose(*it);

    if (mesh->hasAnimation(sn))
        mesh->removeAnimation(sn);

    if (entity) {
        if (auto* states = entity->getAllAnimationStates()) {
            if (states->hasAnimationState(sn))
                states->removeAnimationState(sn);
        }
        // Mesh-level animation list changed; refresh the entity's
        // mirror so removed states actually disappear from
        // getAllAnimationStates() going forward.
        entity->refreshAvailableAnimationState();
    }
}

// Build N same-named poses + their single shared Animation from a
// slice list. Mirrors MeshProcessor's pattern: one Animation, one
// VAT_POSE track per submesh, single t=0 keyframe with full influence
// on that submesh's pose.
void buildPosesFromSlices(Ogre::Mesh* mesh,
                          const QString& name,
                          const std::vector<MorphPoseSlice>& slices,
                          Ogre::Entity* entity)
{
    if (!mesh || slices.empty()) return;
    const std::string sn = name.toStdString();

    // Track the resulting pose index of every slice so we can wire
    // each VAT_POSE track to the right pose handle.
    std::vector<unsigned short> poseIndices;
    poseIndices.reserve(slices.size());
    for (const auto& slice : slices) {
        Ogre::Pose* pose = mesh->createPose(slice.submeshHandle, sn);
        if (!pose) continue;
        for (const auto& [vi, delta] : slice.offsets)
            pose->addVertex(vi, delta);
        // Index = current size - 1 (just-appended pose).
        poseIndices.push_back(static_cast<unsigned short>(mesh->getPoseCount() - 1));
    }

    Ogre::Animation* anim = mesh->hasAnimation(sn)
                                ? mesh->getAnimation(sn)
                                : mesh->createAnimation(sn, /*length=*/0.0f);
    if (!anim) return;

    for (size_t i = 0; i < slices.size() && i < poseIndices.size(); ++i) {
        const unsigned short handle = slices[i].submeshHandle;
        Ogre::VertexAnimationTrack* track =
            anim->hasVertexTrack(handle)
                ? anim->getVertexTrack(handle)
                : anim->createVertexTrack(handle, Ogre::VAT_POSE);
        if (!track) continue;
        auto* kf = track->getNumKeyFrames() > 0
                       ? static_cast<Ogre::VertexPoseKeyFrame*>(track->getKeyFrame(0))
                       : track->createVertexPoseKeyFrame(0.0f);
        kf->addPoseReference(poseIndices[i], 1.0f);
    }

    if (entity) {
        // Adding poses / a VAT_POSE animation to an already-loaded mesh means
        // the entity's pose (software + hardware) vertex-animation buffers were
        // never allocated — the importer sets poses up BEFORE the entity is
        // built, but here we add them to a LIVE entity. Re-initialise so Ogre
        // rebuilds those buffers; without it the render loop applies a pose
        // animation against null buffers and crashes (skinned meshes especially,
        // where skeletal + pose animation combine). Mirrors the AutoRig path,
        // which likewise re-initialises after mutating a live entity's mesh.
        entity->_initialise(true);
        entity->refreshAvailableAnimationState();
    }
}

// ─── Weight-clip preserve/restore across a pose reorder ──────────────
// Morph weight clips (mesh Animations that aren't per-target shape clips)
// reference poses by INDEX. A reorder rebuilds poses in a new order, so those
// indices become stale. Snapshot each weight clip's keyframes by pose NAME
// before the reorder, then rebuild them against the new pose indices after.

// name != a pose name → it's a weight clip, not a shape clip.
bool isWeightClip(Ogre::Mesh* mesh, const std::string& animName)
{
    for (const Ogre::Pose* p : mesh->getPoseList())
        if (p && p->getName() == animName) return false;
    return true;
}

struct WeightClipSnapshot {
    std::string name;
    float length = 0.0f;
    // time -> (poseName -> weight)
    std::vector<std::pair<float, std::map<std::string, float>>> keys;
};

std::vector<WeightClipSnapshot> snapshotWeightClips(Ogre::Mesh* mesh)
{
    std::vector<WeightClipSnapshot> out;
    for (unsigned short a = 0; a < mesh->getNumAnimations(); ++a) {
        Ogre::Animation* anim = mesh->getAnimation(a);
        if (!anim || !isWeightClip(mesh, anim->getName())) continue;
        WeightClipSnapshot snap;
        snap.name = anim->getName();
        snap.length = anim->getLength();
        // Merge all VAT_POSE tracks' keyframes keyed by time; resolve each pose
        // reference's index -> name against the CURRENT pose list.
        std::map<float, std::map<std::string, float>> byTime;
        for (const auto& [handle, track] : anim->_getVertexTrackList()) {
            if (!track || track->getAnimationType() != Ogre::VAT_POSE) continue;
            for (unsigned short k = 0; k < track->getNumKeyFrames(); ++k) {
                auto* kf = static_cast<Ogre::VertexPoseKeyFrame*>(track->getKeyFrame(k));
                for (const auto& ref : kf->getPoseReferences()) {
                    if (ref.poseIndex >= mesh->getPoseList().size()) continue;
                    const Ogre::Pose* p = mesh->getPoseList()[ref.poseIndex];
                    if (p) byTime[kf->getTime()][p->getName()] = ref.influence;
                }
            }
        }
        for (auto& [t, m] : byTime) snap.keys.push_back({t, std::move(m)});
        out.push_back(std::move(snap));
    }
    return out;
}

// Rebuild the snapshotted weight clips against the (reordered) pose list,
// resolving pose names to their NEW indices + submesh handles.
void rebuildWeightClips(Ogre::Mesh* mesh, const std::vector<WeightClipSnapshot>& snaps,
                        Ogre::Entity* entity)
{
    auto poseIndex = [&](const std::string& name) -> int {
        const auto& pl = mesh->getPoseList();
        for (unsigned short i = 0; i < pl.size(); ++i)
            if (pl[i] && pl[i]->getName() == name) return i;
        return -1;
    };
    for (const auto& snap : snaps) {
        if (mesh->hasAnimation(snap.name)) mesh->removeAnimation(snap.name);
        Ogre::Animation* anim = mesh->createAnimation(snap.name, snap.length);
        for (const auto& [t, weights] : snap.keys) {
            for (const auto& [poseName, w] : weights) {
                const int pi = poseIndex(poseName);
                if (pi < 0) continue;
                const unsigned short handle = mesh->getPoseList()[pi]->getTarget();
                Ogre::VertexAnimationTrack* track = anim->hasVertexTrack(handle)
                    ? anim->getVertexTrack(handle)
                    : anim->createVertexTrack(handle, Ogre::VAT_POSE);
                Ogre::VertexPoseKeyFrame* kf = nullptr;
                for (unsigned short ki = 0; ki < track->getNumKeyFrames(); ++ki) {
                    auto* e = static_cast<Ogre::VertexPoseKeyFrame*>(track->getKeyFrame(ki));
                    if (std::abs(e->getTime() - t) < 1e-4f) { kf = e; break; }
                }
                if (!kf) kf = track->createVertexPoseKeyFrame(t);
                kf->updatePoseReference(static_cast<unsigned short>(pi), w);
            }
        }
    }
    if (entity) entity->refreshAvailableAnimationState();
}

} // namespace

// ──────────────── AddMorphTargetCommand ─────────────────────────────

AddMorphTargetCommand::AddMorphTargetCommand(Ogre::Entity* entity,
                                             const QString& name,
                                             const std::vector<MorphPoseSlice>& slices,
                                             QUndoCommand* parent)
    : QUndoCommand(parent),
      mEntity(entity),
      mName(name),
      mSlices(slices)
{
    setText(QStringLiteral("Add morph target \"%1\"").arg(name));
}

void AddMorphTargetCommand::redo()
{
    if (!mEntity) return;
    Ogre::MeshPtr mesh = mEntity->getMesh();
    if (!mesh) return;
    buildPosesFromSlices(mesh.get(), mName, mSlices, mEntity);
    SentryReporter::addBreadcrumb("scene.anim.morph",
        QStringLiteral("add target '%1'").arg(mName));
}

void AddMorphTargetCommand::undo()
{
    if (!mEntity) return;
    Ogre::MeshPtr mesh = mEntity->getMesh();
    if (!mesh) return;
    removePosesByName(mesh.get(), mName, mEntity);
}

// ──────────────── DeleteMorphTargetCommand ──────────────────────────

DeleteMorphTargetCommand::DeleteMorphTargetCommand(Ogre::Entity* entity,
                                                   const QString& name,
                                                   QUndoCommand* parent)
    : QUndoCommand(parent),
      mEntity(entity),
      mName(name)
{
    setText(QStringLiteral("Delete morph target \"%1\"").arg(name));
    if (mEntity) {
        if (Ogre::MeshPtr mesh = mEntity->getMesh())
            mSnapshot = snapshotByName(mesh.get(), name.toStdString());
    }
}

void DeleteMorphTargetCommand::redo()
{
    if (!mEntity) return;
    Ogre::MeshPtr mesh = mEntity->getMesh();
    if (!mesh) return;
    removePosesByName(mesh.get(), mName, mEntity);
    SentryReporter::addBreadcrumb("scene.anim.morph",
        QStringLiteral("delete target '%1'").arg(mName));
}

void DeleteMorphTargetCommand::undo()
{
    if (!mEntity) return;
    Ogre::MeshPtr mesh = mEntity->getMesh();
    if (!mesh) return;
    buildPosesFromSlices(mesh.get(), mName, mSnapshot, mEntity);
}

// ──────────────── RenameMorphTargetCommand ──────────────────────────

RenameMorphTargetCommand::RenameMorphTargetCommand(Ogre::Entity* entity,
                                                   const QString& oldName,
                                                   const QString& newName,
                                                   QUndoCommand* parent)
    : QUndoCommand(parent),
      mEntity(entity),
      mOldName(oldName),
      mNewName(newName)
{
    setText(QStringLiteral("Rename morph target \"%1\" → \"%2\"")
                .arg(oldName, newName));
    if (mEntity) {
        if (Ogre::MeshPtr mesh = mEntity->getMesh())
            mSnapshot = snapshotByName(mesh.get(), oldName.toStdString());
    }
}

void RenameMorphTargetCommand::redo()
{
    if (!mEntity) return;
    Ogre::MeshPtr mesh = mEntity->getMesh();
    if (!mesh) return;
    removePosesByName(mesh.get(), mOldName, mEntity);
    buildPosesFromSlices(mesh.get(), mNewName, mSnapshot, mEntity);
    SentryReporter::addBreadcrumb("scene.anim.morph",
        QStringLiteral("rename target '%1' -> '%2'").arg(mOldName, mNewName));
}

void RenameMorphTargetCommand::undo()
{
    if (!mEntity) return;
    Ogre::MeshPtr mesh = mEntity->getMesh();
    if (!mesh) return;
    removePosesByName(mesh.get(), mNewName, mEntity);
    buildPosesFromSlices(mesh.get(), mOldName, mSnapshot, mEntity);
}

// ──────────────── ReorderMorphTargetsCommand ────────────────────────

ReorderMorphTargetsCommand::ReorderMorphTargetsCommand(Ogre::Entity* entity,
                                                       const QStringList& oldOrder,
                                                       const QStringList& newOrder,
                                                       QUndoCommand* parent)
    : QUndoCommand(parent),
      mEntity(entity),
      mOldOrder(oldOrder),
      mNewOrder(newOrder)
{
    setText(QStringLiteral("Reorder morph targets"));
    if (mEntity) {
        if (Ogre::MeshPtr mesh = mEntity->getMesh()) {
            // Snapshot every target's slices once — both undo and redo rebuild
            // from these, so the offsets survive the intermediate teardown.
            for (const QString& n : mOldOrder)
                mSnapshot[n] = snapshotByName(mesh.get(), n.toStdString());
        }
    }
}

void ReorderMorphTargetsCommand::applyOrder(const QStringList& order)
{
    if (!mEntity) return;
    Ogre::MeshPtr mesh = mEntity->getMesh();
    if (!mesh) return;

    // Pose indices are positional and VAT_POSE keyframes reference them by
    // index, so the only safe reorder is a full teardown + rebuild in the
    // desired name-order. removePosesByName drops each target's poses +
    // Animation; buildPosesFromSlices recreates them (and their keyframe
    // references) fresh, in call order → the new display order.
    //
    // Weight clips (smile/angry/…) reference poses by index too but AREN'T
    // rebuilt by the shape teardown below (their names differ from pose names),
    // so their keyframes would point at the wrong target after the reorder.
    // Snapshot them by pose NAME first, then rebuild against the new indices.
    const std::vector<WeightClipSnapshot> weightClips = snapshotWeightClips(mesh.get());

    for (const QString& n : order)
        removePosesByName(mesh.get(), n, mEntity);
    for (const QString& n : order) {
        auto it = mSnapshot.find(n);
        if (it != mSnapshot.end())
            buildPosesFromSlices(mesh.get(), n, it->second, mEntity);
    }

    // Re-key the weight clips to the reordered poses (by name → new index).
    rebuildWeightClips(mesh.get(), weightClips, mEntity);
}

void ReorderMorphTargetsCommand::redo()
{
    applyOrder(mNewOrder);
    SentryReporter::addBreadcrumb("scene.anim.morph",
        QStringLiteral("reorder targets"));
}

void ReorderMorphTargetsCommand::undo()
{
    applyOrder(mOldOrder);
}
