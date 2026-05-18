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
#include <QString>
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
};

#endif // POSE_LIBRARY_COMMANDS_H
