#ifndef WELD_VERTICES_COMMAND_H
#define WELD_VERTICES_COMMAND_H

#include <QUndoCommand>
#include <QByteArray>
#include <QString>

#include "../MeshWeldOps.h"

#include <OgreMesh.h>

#include <string>
#include <vector>

// Undoable "Weld duplicate vertices" (validation flow): index-remap weld of
// byte-identical duplicates + skin-weight unification across co-located
// twins (MeshWeldOps::apply). Undo restores the snapshotted index buffers
// and bone-assignment lists (recompiled in place — no Entity::_initialise,
// which would swap VertexData under the live SkeletonInstance).
class WeldVerticesCommand : public QUndoCommand
{
public:
    explicit WeldVerticesCommand(std::string entityName,
                                 QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool applied() const { return m_applied; }
    MeshWeldOps::Report report() const { return m_report; }

private:
    struct IndexSnapshot {
        unsigned short submesh = 0;
        QByteArray bytes;          // full index buffer contents
    };
    struct AssignSnapshot {
        int submesh = -1;          // -1 = mesh-level (shared)
        std::vector<Ogre::VertexBoneAssignment> list;
    };

    std::string m_entityName;
    std::vector<IndexSnapshot> m_indexSnaps;
    std::vector<AssignSnapshot> m_assignSnaps;
    MeshWeldOps::Report m_report;
    bool m_applied = false;
    bool m_firstRedo = true;
};

#endif // WELD_VERTICES_COMMAND_H
