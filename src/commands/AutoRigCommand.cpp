#include "commands/AutoRigCommand.h"
#include "Manager.h"
#include "SkinWeights.h"

#include <Ogre.h>
#include <OgreEntity.h>

AutoRigCommand::AutoRigCommand(std::string entityName,
                               AutoRig::Options opts,
                               std::vector<AutoRig::Marker> markers,
                               bool alsoSkin,
                               QUndoCommand* parent)
    : QUndoCommand(parent)
    , mEntityName(std::move(entityName))
    , mOpts(opts)
    , mMarkers(std::move(markers))
    , mAlsoSkin(alsoSkin)
{
    setText(mMarkers.empty() ? QStringLiteral("Auto-Rig")
                             : QStringLiteral("Auto-Rig from Markers"));
}

Ogre::Entity* AutoRigCommand::resolveEntity() const
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    for (Ogre::Entity* e : mgr->getEntities()) {
        if (e && e->getMovableType() == "Entity" && e->getName() == mEntityName)
            return e;
    }
    return nullptr;
}

void AutoRigCommand::redo()
{
    Ogre::Entity* entity = resolveEntity();
    if (!entity) {
        mReport.applied = false;
        mReport.error   = QStringLiteral("Entity no longer in scene.");
        return;
    }

    // unrigEntity (run on undo) leaves a clean static mesh, so re-running the
    // rig on a redo is idempotent — no special first-vs-replay handling needed.
    mSkinned = false;
    mReport  = AutoRig::rigEntityWithMarkers(entity, mMarkers, mOpts);
    if (mReport.applied && mAlsoSkin) {
        const auto sw = SkinWeights::computeAndApply(entity, {});
        mSkinned = sw.applied;
        if (!sw.applied)
            mReport.error = QStringLiteral("rigged, but skinning failed: %1")
                                .arg(sw.error);
    }
}

void AutoRigCommand::undo()
{
    if (!mReport.applied) return;   // nothing was attached
    if (Ogre::Entity* entity = resolveEntity())
        AutoRig::unrigEntity(entity);
}
