#include "ApplyMaterialCommand.h"

#include <OgreSubEntity.h>

ApplyMaterialCommand::ApplyMaterialCommand(std::vector<Target> targets,
                                           std::string newMaterialName,
                                           QUndoCommand* parent)
    : QUndoCommand(parent)
    , mTargets(std::move(targets))
    , mNewMaterialName(std::move(newMaterialName))
{
    setText(QObject::tr("Apply material '%1' to %n sub-entit%2(ies)", "",
                        static_cast<int>(mTargets.size()))
                .arg(QString::fromStdString(mNewMaterialName))
                .arg(mTargets.size() == 1 ? "y" : "ies"));
}

void ApplyMaterialCommand::redo()
{
    for (auto& [sub, _oldName] : mTargets) {
        if (sub) sub->setMaterialName(mNewMaterialName);
    }
}

void ApplyMaterialCommand::undo()
{
    for (auto& [sub, oldName] : mTargets) {
        if (sub) sub->setMaterialName(oldName);
    }
}
