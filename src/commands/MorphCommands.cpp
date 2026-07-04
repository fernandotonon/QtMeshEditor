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

    if (entity)
        entity->refreshAvailableAnimationState();
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
