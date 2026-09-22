#ifndef LATTICE_COMMANDS_H
#define LATTICE_COMMANDS_H

#include <QUndoCommand>
#include <QString>
#include <QJsonObject>

#include <OgreVector.h>

#include <string>
#include <vector>

/**
 * Undo commands for the lattice deformer (LatticeController).
 *
 * Two levels, mirroring Blender's lattice workflow:
 *
 *  - **LatticePointsCommand** — one control-point drag / reset / load INSIDE a
 *    live session. Snapshots the control-point array before and after; undo
 *    re-applies the earlier array through the controller (which re-deforms
 *    the mesh from its frozen rest positions). A no-op when the session has
 *    since closed or moved to another entity — the bake command below owns
 *    the mesh state at that point.
 *
 *  - **LatticeApplyCommand** — the BAKE. Captures dense per-submesh rest and
 *    deformed positions (the whole mesh, not a sparse map — a lattice touches
 *    most vertices) and writes one or the other back through EditableMesh's
 *    in-place commit. Resolves its entity by NAME on every undo/redo, never a
 *    cached pointer.
 */
namespace LatticeCmd {
using Positions = std::vector<std::vector<Ogre::Vector3>>; ///< [submesh][vertex], mesh-local.

/// Write `positions` into the named entity's vertex buffers (positions +
/// recomputed normals + bounds) via EditableMesh. Returns false when the
/// entity is gone or the layout no longer matches.
bool writePositionsToEntity(const std::string& entityName, const Positions& positions,
                            QString* error = nullptr);
}

class LatticePointsCommand : public QUndoCommand
{
public:
    LatticePointsCommand(std::string entityName,
                         std::vector<Ogre::Vector3> before,
                         std::vector<Ogre::Vector3> after,
                         const QString& description,
                         QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    std::string mEntityName;
    std::vector<Ogre::Vector3> mBefore;
    std::vector<Ogre::Vector3> mAfter;
    bool mFirstRedo = true;
};

class LatticeApplyCommand : public QUndoCommand
{
public:
    /** @param alreadyApplied  true when the deformed positions are already on
     *  the GPU (the interactive session) — the first redo() is then skipped. */
    LatticeApplyCommand(std::string entityName,
                        LatticeCmd::Positions rest,
                        LatticeCmd::Positions deformed,
                        QJsonObject lattice,
                        bool alreadyApplied,
                        QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

    bool ok() const { return mOk; }
    const QString& error() const { return mError; }
    const QJsonObject& lattice() const { return mLattice; }

private:
    std::string mEntityName;
    LatticeCmd::Positions mRest;
    LatticeCmd::Positions mDeformed;
    QJsonObject mLattice;
    bool mSkipFirstRedo = false;
    bool mOk = true;
    QString mError;
};

#endif // LATTICE_COMMANDS_H
