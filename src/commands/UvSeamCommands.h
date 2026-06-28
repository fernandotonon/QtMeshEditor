#ifndef UV_SEAM_COMMANDS_H
#define UV_SEAM_COMMANDS_H

#include "EditableMesh.h"

#include <OgreEntity.h>
#include <QUndoCommand>
#include <QString>
#include <cstdint>
#include <vector>

class UvSeamMarkCommand : public QUndoCommand
{
public:
    struct EdgeChange {
        size_t subMeshIndex = 0;
        uint64_t edgeKey = 0;
        bool oldSeam = false;
        bool newSeam = false;
    };

    UvSeamMarkCommand(Ogre::Entity* entity,
                      std::vector<EdgeChange> changes,
                      const QString& description,
                      QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    void apply(bool useNew);

    Ogre::Entity* m_entity = nullptr;
    std::vector<EdgeChange> m_changes;
    bool m_firstRedo = true;
};

class UvPinCommand : public QUndoCommand
{
public:
    struct PinChange {
        size_t subMeshIndex = 0;
        unsigned int vertexIndex = 0;
        bool oldPinned = false;
        bool newPinned = false;
    };

    UvPinCommand(Ogre::Entity* entity,
                 std::vector<PinChange> changes,
                 const QString& description,
                 QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    void apply(bool useNew);

    Ogre::Entity* m_entity = nullptr;
    std::vector<PinChange> m_changes;
    bool m_firstRedo = true;
};

class UvSeamTopologyCommand : public QUndoCommand
{
public:
    UvSeamTopologyCommand(Ogre::Entity* entity,
                          std::vector<EditableSubMesh> before,
                          std::vector<EditableSubMesh> after,
                          const QString& description,
                          bool topologyChange = true,
                          QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    void applyMesh(const std::vector<EditableSubMesh>& state);

    Ogre::Entity* m_entity = nullptr;
    std::vector<EditableSubMesh> m_before;
    std::vector<EditableSubMesh> m_after;
    bool m_topologyChange = true;
    bool m_firstRedo = true;
};

#endif // UV_SEAM_COMMANDS_H
