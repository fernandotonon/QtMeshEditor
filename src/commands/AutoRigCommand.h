#ifndef AUTO_RIG_COMMAND_H
#define AUTO_RIG_COMMAND_H

#include <QUndoCommand>
#include <QString>

#include <string>
#include <vector>

#include "AutoRig.h"

namespace Ogre { class Entity; }

/**
 * Undoable wrapper around `AutoRig::rigEntity[WithMarkers]` (+ optional
 * `SkinWeights::computeAndApply`) — issue #407 follow-up.
 *
 * Auto-rig only ever runs on a STATIC (skeleton-less) mesh, so the undo is
 * unambiguous: strip the freshly-attached skeleton and revert the entity to
 * its static form (`AutoRig::unrigEntity`). There is no prior skeleton/weights
 * to snapshot — the "before" state is simply "no skeleton".
 *
 * `redo()` runs the rig on its first invocation (and again on later redos —
 * `unrigEntity` leaves a clean static mesh, so re-rigging is idempotent);
 * `undo()` strips the rig. When `alsoSkin` is set, the skin pass runs inside
 * the same command (not as a child) so a single Ctrl+Z reverts rig + skin
 * together. The captured report lets the controller surface bone/marker counts
 * to the UI after pushing the command.
 *
 * Targets the entity by name so it survives scene rebuilds, like the other
 * entity-scoped commands.
 */
class AutoRigCommand : public QUndoCommand
{
public:
    AutoRigCommand(std::string entityName,
                   AutoRig::Options opts,
                   std::vector<AutoRig::Marker> markers,
                   bool alsoSkin,
                   QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    const AutoRig::Report& report() const { return mReport; }
    bool applied() const { return mReport.applied; }
    bool skinned() const { return mSkinned; }

private:
    Ogre::Entity* resolveEntity() const;

    std::string                  mEntityName;
    AutoRig::Options             mOpts;
    std::vector<AutoRig::Marker> mMarkers;
    bool                         mAlsoSkin = false;

    AutoRig::Report              mReport;
    bool                         mSkinned = false;
};

#endif // AUTO_RIG_COMMAND_H
