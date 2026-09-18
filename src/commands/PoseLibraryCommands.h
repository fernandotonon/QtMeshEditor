/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef POSE_LIBRARY_COMMANDS_H
#define POSE_LIBRARY_COMMANDS_H

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUndoCommand>

#include <OgreQuaternion.h>
#include <OgreVector.h>

#include <optional>

namespace Ogre { class Entity; }

// Per-bone snapshot captured directly by the commands. We keep our
// own structure (rather than reaching into `PoseLibrary::BonePoseSnapshot`)
// so the commands don't depend on the library's private types and
// undo can store snapshots for poses that were never saved (the
// ApplyPoseCommand's "where were the bones before I applied" data).
struct PoseLibBoneTRS {
    Ogre::Vector3 translate{Ogre::Vector3::ZERO};
    Ogre::Quaternion rotation{Ogre::Quaternion::IDENTITY};
    Ogre::Vector3 scale{Ogre::Vector3(1, 1, 1)};
};

using PoseLibSnapshot = QHash<QString, PoseLibBoneTRS>;

// SavePoseCommand: redo captures-and-saves under `name`. Undo
// deletes the pose IF this command created it (i.e. there was no
// same-name pose before), or restores the previous content if we
// overwrote one.
class SavePoseCommand : public QUndoCommand
{
public:
    SavePoseCommand(Ogre::Entity* entity,
                    const QString& name,
                    QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    Ogre::Entity* mEntity = nullptr;
    QString mName;
    // Bones-TRS at command-construction time. This is what redo
    // saves into the library; it captures BEFORE the user makes
    // further edits so the saved pose is the user's intended
    // snapshot, not whatever the bones look like after the
    // command sits unexecuted on the stack.
    PoseLibSnapshot mNewSnapshot;
    // Empty when there was no prior pose by that name; populated
    // when we're overwriting an existing one so undo can restore it.
    std::optional<PoseLibSnapshot> mPriorSnapshot;
};

// DeletePoseCommand: redo drops the pose. Construction snapshots
// the pose content so undo can recreate it.
class DeletePoseCommand : public QUndoCommand
{
public:
    DeletePoseCommand(Ogre::Entity* entity,
                      const QString& name,
                      QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    Ogre::Entity* mEntity = nullptr;
    QString mName;
    PoseLibSnapshot mSnapshot;
    bool mWasPresent = false;
};

// ApplyPoseCommand: redo applies the saved pose to the live
// skeleton; undo restores the pre-apply bone TRS values. The
// snapshot is captured at construction so we know the exact
// state to revert to even if the user keeps editing on the
// undo stack.
class ApplyPoseCommand : public QUndoCommand
{
public:
    ApplyPoseCommand(Ogre::Entity* entity,
                     const QString& name,
                     QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    Ogre::Entity* mEntity = nullptr;
    QString mName;
    // Bone TRS values as they were BEFORE redo applied the saved
    // pose. Undo writes these back.
    PoseLibSnapshot mPreApply;
    // True only when the most recent redo() actually applied the
    // pose. If the pose name was missing / library returned false,
    // redo is a no-op and undo MUST also be a no-op — otherwise
    // we'd clobber user edits made after the failed apply with the
    // stale `mPreApply` snapshot (Codex P1 on PR #595).
    bool mRedoApplied = false;
};

// ApplyPoseMaskedCommand: like ApplyPoseCommand but only the bones
// named in the mask are written. Undo restores the pre-apply TRS of
// exactly those bones, so bones the mask excluded are never touched
// on either leg — that's the point of the feature.
class ApplyPoseMaskedCommand : public QUndoCommand
{
public:
    ApplyPoseMaskedCommand(Ogre::Entity* entity,
                           const QString& name,
                           const QStringList& boneNames,
                           QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    Ogre::Entity* mEntity = nullptr;
    QString mName;
    QSet<QString> mMask;
    // Pre-apply TRS for the MASKED bones only.
    PoseLibSnapshot mPreApply;
    bool mRedoApplied = false;
};

// ApplyPoseBlendedCommand: starts a time-blended transition toward
// the saved pose. Undo cancels any in-flight blend and restores the
// pre-blend bone TRS, so Ctrl+Z mid-transition lands back where the
// author started rather than at a random point along the curve.
class ApplyPoseBlendedCommand : public QUndoCommand
{
public:
    ApplyPoseBlendedCommand(Ogre::Entity* entity,
                            const QString& name,
                            float durationSeconds,
                            QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    Ogre::Entity* mEntity = nullptr;
    QString mName;
    float mDuration = 0.0f;
    PoseLibSnapshot mPreApply;
    bool mRedoApplied = false;
};

// MirrorPoseCommand: redo writes the mirrored pose under `dstName`.
// Undo removes it (pure-add) or restores the prior content
// (overwrite) — the same two-case shape as SavePoseCommand.
class MirrorPoseCommand : public QUndoCommand
{
public:
    MirrorPoseCommand(Ogre::Entity* entity,
                      const QString& srcName,
                      const QString& dstName,
                      QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    Ogre::Entity* mEntity = nullptr;
    QString mSrcName;
    QString mDstName;
    // Prior content of `dstName`, when we're overwriting one.
    std::optional<PoseLibSnapshot> mPriorSnapshot;
    bool mRedoApplied = false;
};

// BlendPosesCommand: redo writes the A/B blend under `dstName`.
// Same undo shape as MirrorPoseCommand.
class BlendPosesCommand : public QUndoCommand
{
public:
    BlendPosesCommand(Ogre::Entity* entity,
                      const QString& aName,
                      const QString& bName,
                      float weight,
                      const QString& dstName,
                      QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    Ogre::Entity* mEntity = nullptr;
    QString mAName;
    QString mBName;
    float mWeight = 0.5f;
    QString mDstName;
    std::optional<PoseLibSnapshot> mPriorSnapshot;
    bool mRedoApplied = false;
};

#endif // POSE_LIBRARY_COMMANDS_H
