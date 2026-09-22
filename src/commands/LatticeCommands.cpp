#include "LatticeCommands.h"

#include "EditableMesh.h"
#include "LatticeController.h"
#include "Manager.h"

#include <OgreEntity.h>

namespace LatticeCmd {

static Ogre::Entity* resolveEntity(const std::string& name)
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    for (Ogre::Entity* obj : mgr->getEntities()) {
        if (obj && obj->getMovableType() == "Entity" && obj->getName() == name)
            return obj;
    }
    return nullptr;
}

bool writePositionsToEntity(const std::string& entityName, const Positions& positions, QString* error)
{
    Ogre::Entity* entity = resolveEntity(entityName);
    if (!entity) {
        if (error) *error = QStringLiteral("entity '%1' not found").arg(QString::fromStdString(entityName));
        return false;
    }
    EditableMesh mesh;
    if (!mesh.loadFromEntity(entity)) {
        if (error) *error = QStringLiteral("could not read '%1'").arg(QString::fromStdString(entityName));
        return false;
    }
    auto& subs = mesh.subMeshes();
    if (subs.size() != positions.size()) {
        if (error) *error = QStringLiteral("submesh count changed since the lattice was applied");
        return false;
    }
    for (size_t s = 0; s < subs.size(); ++s) {
        if (subs[s].vertices.size() != positions[s].size()) {
            if (error) *error = QStringLiteral("vertex count changed since the lattice was applied");
            return false;
        }
        for (size_t v = 0; v < positions[s].size(); ++v) subs[s].vertices[v].position = positions[s][v];
    }
    mesh.recalculateNormals();
    if (!mesh.commitToEntity(entity)) {
        if (error) *error = QStringLiteral("failed to write vertex data");
        return false;
    }
    return true;
}

} // namespace LatticeCmd

// ---------------------------------------------------------------------------

LatticePointsCommand::LatticePointsCommand(std::string entityName, std::vector<Ogre::Vector3> before,
                                           std::vector<Ogre::Vector3> after, const QString& description,
                                           QUndoCommand* parent)
    : QUndoCommand(description, parent), mEntityName(std::move(entityName)), mBefore(std::move(before)),
      mAfter(std::move(after))
{
}

void LatticePointsCommand::undo()
{
    if (auto* ctl = LatticeController::instance()) ctl->restorePointsFromUndo(mEntityName, mBefore);
}

void LatticePointsCommand::redo()
{
    if (mFirstRedo) { mFirstRedo = false; return; } // the live edit already happened
    if (auto* ctl = LatticeController::instance()) ctl->restorePointsFromUndo(mEntityName, mAfter);
}

// ---------------------------------------------------------------------------

LatticeApplyCommand::LatticeApplyCommand(std::string entityName, LatticeCmd::Positions rest,
                                         LatticeCmd::Positions deformed, QJsonObject lattice,
                                         bool alreadyApplied, QUndoCommand* parent)
    : QUndoCommand(QStringLiteral("Apply Lattice Deform"), parent), mEntityName(std::move(entityName)),
      mRest(std::move(rest)), mDeformed(std::move(deformed)), mLattice(std::move(lattice)),
      mSkipFirstRedo(alreadyApplied)
{
}

void LatticeApplyCommand::undo()
{
    mOk = LatticeCmd::writePositionsToEntity(mEntityName, mRest, &mError);
}

void LatticeApplyCommand::redo()
{
    if (mSkipFirstRedo) { mSkipFirstRedo = false; return; }
    mOk = LatticeCmd::writePositionsToEntity(mEntityName, mDeformed, &mError);
}
