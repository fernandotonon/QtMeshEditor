#include "UVEditCommand.h"

#include "EditableMesh.h"
#include "EditModeController.h"
#include "SentryReporter.h"
#include "UVEditorController.h"

#include <OgreEntity.h>

UVEditCommand::UVEditCommand(Ogre::Entity* entity,
                             int uvChannel,
                             std::vector<VertChange> changes,
                             const QString& description,
                             QUndoCommand* parent)
    : QUndoCommand(description, parent)
    , m_entity(entity)
    , m_uvChannel(uvChannel)
    , m_changes(std::move(changes))
{
}

void UVEditCommand::apply(bool useNew)
{
    if (!m_entity || m_changes.empty())
        return;

    EditableMesh localMesh;
    EditableMesh* mesh = nullptr;
    auto* edit = EditModeController::instance();
    if (edit && edit->isEditModeActive() && edit->editEntity() == m_entity && edit->currentMesh()) {
        mesh = edit->currentMesh();
    } else if (auto* uv = UVEditorController::instance()) {
        mesh = uv->workingMeshForEntity(m_entity);
    }
    if (!mesh) {
        if (!localMesh.loadFromEntity(m_entity))
            return;
        mesh = &localMesh;
    }

    UVEditorController* uvCtrl = UVEditorController::instance();
    const bool useControllerMesh =
        uvCtrl && uvCtrl->workingMeshForEntity(m_entity) == mesh;

    for (const auto& ch : m_changes) {
        const Ogre::Vector2& uv = useNew ? ch.newUv : ch.oldUv;
        if (useControllerMesh) {
            uvCtrl->applyWorkingMeshUv(ch.subMeshIndex, ch.vertexIndex, uv);
        } else {
            mesh->setVertexUV(static_cast<size_t>(ch.subMeshIndex),
                              static_cast<size_t>(ch.vertexIndex), uv);
        }
    }

    if (useControllerMesh) {
        if (!uvCtrl->commitWorkingMeshUvs())
            return;
    } else {
        if (!mesh->commitUvsToEntity(m_entity, m_uvChannel))
            return;
    }

    if (edit && edit->isEditModeActive() && edit->editEntity() == m_entity)
        edit->notifyMeshDataChanged();

    if (uvCtrl)
        uvCtrl->refreshAfterUvEdit();
}

void UVEditCommand::undo()
{
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.transform"),
                                  QStringLiteral("Undo UV edit"));
    apply(false);
}

void UVEditCommand::redo()
{
    if (m_firstRedo) {
        m_firstRedo = false;
        return;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.transform"),
                                  QStringLiteral("Redo UV edit"));
    apply(true);
}
