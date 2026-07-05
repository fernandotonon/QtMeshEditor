/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef MORPH_COMMANDS_H
#define MORPH_COMMANDS_H

#include <QUndoCommand>
#include <QString>
#include <QStringList>

#include <OgreVector.h>

#include <map>
#include <string>
#include <vector>

namespace Ogre { class Entity; class Mesh; }

// All three morph-authoring commands operate on a single Ogre::Entity's
// mesh. The mesh keeps the canonical pose + animation state, so undo /
// redo is just "rebuild the pose and its driving Animation against the
// snapshot we captured up-front." Each snapshot is intentionally small:
// poses store sparse `{vertexIndex -> deltaVec}` maps, not full mesh
// copies — typical character poses touch a few hundred vertices each.

// One per-submesh delta source. `submeshHandle` follows Ogre's 1-based
// convention (0 = shared verts, 1..N = per-submesh).
struct MorphPoseSlice {
    unsigned short submeshHandle = 1;
    std::map<unsigned int, Ogre::Vector3f> offsets;
};

// AddMorphTargetCommand: create a new named pose + matching VAT_POSE
// Animation. Undo removes both. Intended use is "save current edit
// delta as new target" — the manager computes the offsets, this
// command persists them.
class AddMorphTargetCommand : public QUndoCommand
{
public:
    AddMorphTargetCommand(Ogre::Entity* entity,
                          const QString& name,
                          const std::vector<MorphPoseSlice>& slices,
                          QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    Ogre::Entity* mEntity = nullptr;
    QString mName;
    std::vector<MorphPoseSlice> mSlices;
};

// DeleteMorphTargetCommand: remove all same-named poses + the matching
// Animation. The constructor snapshots the current pose offsets so undo
// can rebuild exactly what was lost.
class DeleteMorphTargetCommand : public QUndoCommand
{
public:
    DeleteMorphTargetCommand(Ogre::Entity* entity,
                             const QString& name,
                             QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    Ogre::Entity* mEntity = nullptr;
    QString mName;
    // Captured at construction; reused by undo.
    std::vector<MorphPoseSlice> mSnapshot;
};

// RenameMorphTargetCommand: destroy the same-named poses + animation
// and recreate them under a new name. Undo reverses the rename.
class RenameMorphTargetCommand : public QUndoCommand
{
public:
    RenameMorphTargetCommand(Ogre::Entity* entity,
                             const QString& oldName,
                             const QString& newName,
                             QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    Ogre::Entity* mEntity = nullptr;
    QString mOldName;
    QString mNewName;
    std::vector<MorphPoseSlice> mSnapshot;
};

// ReorderMorphTargetsCommand: change the display order of morph targets.
// The order is defined by the sequence of unique names in the mesh's pose
// list; VAT_POSE keyframes reference poses by INDEX, so a reorder rebuilds
// every target's poses + driving Animation in the new order (recreating the
// keyframe references correctly). Undo restores the previous order. Captures
// both orders + a per-name slice snapshot at construction.
class ReorderMorphTargetsCommand : public QUndoCommand
{
public:
    ReorderMorphTargetsCommand(Ogre::Entity* entity,
                               const QStringList& oldOrder,
                               const QStringList& newOrder,
                               QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    void applyOrder(const QStringList& order);
    Ogre::Entity* mEntity = nullptr;
    QStringList mOldOrder;
    QStringList mNewOrder;
    // name -> its pose slices, captured up-front so rebuild is exact.
    std::map<QString, std::vector<MorphPoseSlice>> mSnapshot;
};

#endif // MORPH_COMMANDS_H
