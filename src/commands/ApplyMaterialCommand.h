#ifndef APPLY_MATERIAL_COMMAND_H
#define APPLY_MATERIAL_COMMAND_H

#include <QUndoCommand>
#include <string>
#include <utility>
#include <vector>

namespace Ogre {
    class SubEntity;
}

/// Slice I: undo record for "Apply material to selection". Captures the
/// (sub-entity, oldMaterialName) pair for every touched sub-entity at
/// the time of the apply, so undo restores the pre-apply binding for
/// each one independently. SubEntity pointers are owned by Ogre and
/// remain valid for the session — the command does not survive entity
/// reload.
class ApplyMaterialCommand : public QUndoCommand
{
public:
    /// targets stores (sub-entity, old material name) so undo can
    /// restore each binding independently. `newMaterialName` is the
    /// material applied on redo.
    using Target = std::pair<Ogre::SubEntity*, std::string>;

    ApplyMaterialCommand(std::vector<Target> targets,
                         std::string newMaterialName,
                         QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    std::vector<Target> mTargets;
    std::string         mNewMaterialName;
};

#endif // APPLY_MATERIAL_COMMAND_H
