#ifndef UV_EDIT_COMMAND_H
#define UV_EDIT_COMMAND_H

#include <OgreVector2.h>
#include <QUndoCommand>

#include <vector>

namespace Ogre {
class Entity;
}

/// Undo record for a UV transform in the UV editor (issue #461).
class UVEditCommand : public QUndoCommand
{
public:
    struct VertChange {
        int subMeshIndex = 0;
        int vertexIndex = 0;
        Ogre::Vector2 oldUv;
        Ogre::Vector2 newUv;
    };

    UVEditCommand(Ogre::Entity* entity,
                  int uvChannel,
                  std::vector<VertChange> changes,
                  const QString& description,
                  QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    void apply(bool useNew);

    Ogre::Entity* m_entity = nullptr;
    int m_uvChannel = 0;
    std::vector<VertChange> m_changes;
    bool m_firstRedo = true;
};

#endif // UV_EDIT_COMMAND_H
