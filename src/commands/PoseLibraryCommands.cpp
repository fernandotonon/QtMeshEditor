/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "PoseLibraryCommands.h"

#include "../PoseLibrary.h"
#include "../SentryReporter.h"

#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreSkeleton.h>
#include <OgreSkeletonInstance.h>

namespace {

// Mirrors PoseLibrary's internal skeleton lookup so the commands
// don't need a private-API hook. Null when the entity has no
// skeleton (a static prop) or is itself null.
Ogre::SkeletonInstance* skeletonOf(Ogre::Entity* entity)
{
    if (!entity) return nullptr;
    if (!entity->hasSkeleton()) return nullptr;
    return entity->getSkeleton();
}

// Walk every Bone on the entity's skeleton and capture its TRS
// into the returned hash. Mirrors `PoseLibrary::savePose`'s
// capture loop — same NAME-keyed shape so the commands and the
// library agree on what a "snapshot" is.
PoseLibSnapshot capturePose(Ogre::Entity* entity)
{
    PoseLibSnapshot out;
    auto* skel = skeletonOf(entity);
    if (!skel) return out;
    out.reserve(skel->getNumBones());
    for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
        Ogre::Bone* bone = skel->getBone(i);
        if (!bone) continue;
        PoseLibBoneTRS trs;
        trs.translate = bone->getPosition();
        trs.rotation = bone->getOrientation();
        trs.scale = bone->getScale();
        out.insert(QString::fromStdString(bone->getName()), trs);
    }
    return out;
}

// Write a captured snapshot back onto the live bones. Bones
// present in the snapshot but missing on the current skeleton are
// skipped silently — same partial-apply contract PoseLibrary uses.
void applyPose(Ogre::Entity* entity, const PoseLibSnapshot& snap)
{
    auto* skel = skeletonOf(entity);
    if (!skel) return;
    for (auto it = snap.cbegin(); it != snap.cend(); ++it) {
        const std::string boneName = it.key().toStdString();
        if (!skel->hasBone(boneName)) continue;
        Ogre::Bone* bone = skel->getBone(boneName);
        if (!bone) continue;
        bone->setPosition(it.value().translate);
        bone->setOrientation(it.value().rotation);
        bone->setScale(it.value().scale);
    }
}

// Save a precomputed snapshot into the library under `name`. Done
// in two steps because PoseLibrary's public surface only accepts
// "save what's on the entity right now": we apply the snapshot
// transiently, hit PoseLibrary::savePose, then restore the bones
// to whatever they were before. Caller is responsible for the
// "restore" via captureBeforeAndAfter.
void writeSnapshotToLibrary(Ogre::Entity* entity,
                            const QString& name,
                            const PoseLibSnapshot& snap)
{
    if (!entity || name.isEmpty()) return;
    auto* lib = PoseLibrary::instance();
    if (!lib) return;
    const PoseLibSnapshot live = capturePose(entity);
    applyPose(entity, snap);
    lib->savePose(entity, name);
    applyPose(entity, live);
}

} // namespace

// ──────────────── SavePoseCommand ───────────────────────────────────

SavePoseCommand::SavePoseCommand(Ogre::Entity* entity,
                                 const QString& name,
                                 QUndoCommand* parent)
    : QUndoCommand(parent), mEntity(entity), mName(name)
{
    setText(QStringLiteral("Save pose \"%1\"").arg(name));
    mNewSnapshot = capturePose(entity);

    // Snapshot the prior content if a same-name pose already
    // exists, so undo can restore it. We capture it by
    // round-tripping: apply the saved pose, capture, restore
    // current bones. (Pose data is only readable through the
    // entity's live bones, not directly through PoseLibrary's
    // public surface.)
    auto* lib = PoseLibrary::instance();
    if (lib && lib->hasPose(entity, name)) {
        const PoseLibSnapshot live = capturePose(entity);
        lib->applyPose(entity, name);
        mPriorSnapshot = capturePose(entity);
        applyPose(entity, live);
    }
}

void SavePoseCommand::redo()
{
    if (!mEntity) return;
    writeSnapshotToLibrary(mEntity, mName, mNewSnapshot);
    SentryReporter::addBreadcrumb("scene.anim.pose.cmd",
        QStringLiteral("redo: save '%1'").arg(mName));
}

void SavePoseCommand::undo()
{
    if (!mEntity) return;
    auto* lib = PoseLibrary::instance();
    if (!lib) return;
    if (mPriorSnapshot.has_value()) {
        // Overwrite case — write the prior content back under
        // the same name.
        writeSnapshotToLibrary(mEntity, mName, *mPriorSnapshot);
    } else {
        // Pure-add case — drop the pose we created.
        lib->deletePose(mEntity, mName);
    }
    SentryReporter::addBreadcrumb("scene.anim.pose.cmd",
        QStringLiteral("undo: save '%1'").arg(mName));
}

// ──────────────── DeletePoseCommand ─────────────────────────────────

DeletePoseCommand::DeletePoseCommand(Ogre::Entity* entity,
                                     const QString& name,
                                     QUndoCommand* parent)
    : QUndoCommand(parent), mEntity(entity), mName(name)
{
    setText(QStringLiteral("Delete pose \"%1\"").arg(name));
    auto* lib = PoseLibrary::instance();
    if (lib && lib->hasPose(entity, name)) {
        mWasPresent = true;
        // Round-trip-capture the pose content. We need this to
        // recreate the pose on undo since once deleted the
        // library has nothing to restore from.
        const PoseLibSnapshot live = capturePose(entity);
        lib->applyPose(entity, name);
        mSnapshot = capturePose(entity);
        applyPose(entity, live);
    }
}

void DeletePoseCommand::redo()
{
    if (!mEntity || !mWasPresent) return;
    auto* lib = PoseLibrary::instance();
    if (lib) lib->deletePose(mEntity, mName);
    SentryReporter::addBreadcrumb("scene.anim.pose.cmd",
        QStringLiteral("redo: delete '%1'").arg(mName));
}

void DeletePoseCommand::undo()
{
    if (!mEntity || !mWasPresent) return;
    writeSnapshotToLibrary(mEntity, mName, mSnapshot);
    SentryReporter::addBreadcrumb("scene.anim.pose.cmd",
        QStringLiteral("undo: delete '%1'").arg(mName));
}

// ──────────────── ApplyPoseCommand ──────────────────────────────────

ApplyPoseCommand::ApplyPoseCommand(Ogre::Entity* entity,
                                   const QString& name,
                                   QUndoCommand* parent)
    : QUndoCommand(parent), mEntity(entity), mName(name)
{
    setText(QStringLiteral("Apply pose \"%1\"").arg(name));
    // Capture the live bone TRS BEFORE redo applies the saved
    // pose. Undo will write these back.
    mPreApply = capturePose(entity);
}

void ApplyPoseCommand::redo()
{
    if (!mEntity) return;
    auto* lib = PoseLibrary::instance();
    if (lib) lib->applyPose(mEntity, mName);
    SentryReporter::addBreadcrumb("scene.anim.pose.cmd",
        QStringLiteral("redo: apply '%1'").arg(mName));
}

void ApplyPoseCommand::undo()
{
    if (!mEntity) return;
    applyPose(mEntity, mPreApply);
    SentryReporter::addBreadcrumb("scene.anim.pose.cmd",
        QStringLiteral("undo: apply '%1'").arg(mName));
}
