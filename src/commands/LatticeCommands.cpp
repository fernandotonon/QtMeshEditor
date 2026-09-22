#include "LatticeCommands.h"

#include "EditableMesh.h"
#include "LatticeController.h"
#include "Manager.h"

#include <OgreEntity.h>
#include <OgreSceneManager.h>

namespace LatticeCmd {

static Ogre::Entity* resolveEntity(const std::string& name)
{
    Manager* mgr = Manager::getSingletonPtr();
    Ogre::SceneManager* scene = mgr ? mgr->getSceneMgr() : nullptr;
    if (!scene || !scene->hasEntity(name)) return nullptr;
    return scene->getEntity(name);
}

bool writePositionsToEntity(const std::string& entityName, const Positions& positions, const Positions* normals,
                            bool recomputeNormals, const Ogre::Mesh* expectedMesh, QString* error)
{
    Ogre::Entity* entity = resolveEntity(entityName);
    if (!entity) {
        if (error) *error = QStringLiteral("entity '%1' not found").arg(QString::fromStdString(entityName));
        return false;
    }
    if (expectedMesh && entity->getMesh().get() != expectedMesh) {
        if (error) *error = QStringLiteral("'%1' now carries a different mesh than the lattice was applied to")
                                .arg(QString::fromStdString(entityName));
        return false;
    }
    // A live interactive session on this mesh holds a rest snapshot that is
    // about to stop describing the mesh — drop it (no write) before ours.
    if (auto* ctl = LatticeController::instance()) ctl->abandonSessionFor(entityName);

    EditableMesh mesh;
    if (!mesh.loadFromEntity(entity)) {
        if (error) *error = QStringLiteral("could not read '%1'").arg(QString::fromStdString(entityName));
        return false;
    }
    auto& subs = mesh.subMeshes();
    if (subs.size() != positions.size() || (normals && normals->size() != positions.size())) {
        if (error) *error = QStringLiteral("submesh count changed since the lattice was applied");
        return false;
    }
    for (size_t s = 0; s < subs.size(); ++s) {
        if (subs[s].vertices.size() != positions[s].size()
            || (normals && (*normals)[s].size() != positions[s].size())) {
            if (error) *error = QStringLiteral("vertex count changed since the lattice was applied");
            return false;
        }
        for (size_t v = 0; v < positions[s].size(); ++v) {
            subs[s].vertices[v].position = positions[s][v];
            if (normals) subs[s].vertices[v].normal = (*normals)[s][v];
        }
    }
    if (!mesh.commitToEntity(entity, recomputeNormals)) {
        if (error) *error = QStringLiteral("failed to write vertex data");
        return false;
    }
    return true;
}

} // namespace LatticeCmd

// ---------------------------------------------------------------------------

LatticeGridCommand::LatticeGridCommand(std::string entityName, uint64_t sessionId, QJsonObject before,
                                       QJsonObject after, const QString& description, QUndoCommand* parent)
    : QUndoCommand(description, parent), mEntityName(std::move(entityName)), mSessionId(sessionId),
      mBefore(std::move(before)), mAfter(std::move(after))
{
}

void LatticeGridCommand::undo()
{
    if (auto* ctl = LatticeController::instance()) ctl->restoreGridFromUndo(mEntityName, mSessionId, mBefore);
}

void LatticeGridCommand::redo()
{
    if (mFirstRedo) { mFirstRedo = false; return; } // the live edit already happened
    if (auto* ctl = LatticeController::instance()) ctl->restoreGridFromUndo(mEntityName, mSessionId, mAfter);
}

// ---------------------------------------------------------------------------

LatticeApplyCommand::LatticeApplyCommand(std::string entityName, const Ogre::Mesh* meshIdentity,
                                         LatticeCmd::Positions rest, LatticeCmd::Positions restNormals,
                                         LatticeCmd::Positions deformed, QJsonObject lattice, bool alreadyApplied,
                                         QUndoCommand* parent)
    : QUndoCommand(QStringLiteral("Apply Lattice Deform"), parent), mEntityName(std::move(entityName)),
      mMeshIdentity(meshIdentity), mRest(std::move(rest)), mRestNormals(std::move(restNormals)), mDeformed(std::move(deformed)),
      mLattice(std::move(lattice)), mSkipFirstRedo(alreadyApplied)
{
}

void LatticeApplyCommand::undo()
{
    // Restore the authored normals verbatim — recomputing would smooth hard edges.
    const bool haveNormals = mRestNormals.size() == mRest.size();
    mOk = LatticeCmd::writePositionsToEntity(mEntityName, mRest, haveNormals ? &mRestNormals : nullptr,
                                             /*recomputeNormals=*/!haveNormals, mMeshIdentity, &mError);
}

void LatticeApplyCommand::redo()
{
    if (mSkipFirstRedo) { mSkipFirstRedo = false; return; }
    mOk = LatticeCmd::writePositionsToEntity(mEntityName, mDeformed, nullptr, /*recomputeNormals=*/true,
                                             mMeshIdentity, &mError);
}
