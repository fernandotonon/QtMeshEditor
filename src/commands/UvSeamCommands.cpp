#include "UvSeamCommands.h"

#include "EditModeController.h"
#include "SentryReporter.h"
#include "UVEditorController.h"
#include "UvSeamData.h"

namespace {

void syncEditMeshFrom(Ogre::Entity* entity, EditableMesh* mesh)
{
    if (!entity || !mesh)
        return;
    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == entity && edit->currentMesh())
            edit->currentMesh()->subMeshes() = mesh->subMeshes();
    }
}

void commitUvMeshState(Ogre::Entity* entity, EditableMesh* mesh)
{
    if (!entity || !mesh)
        return;

    int channel = 0;
    if (auto* uv = UVEditorController::instance())
        channel = uv->uvChannel();

    mesh->commitUvsToEntity(entity, channel, nullptr);
    UvSeamData::writeBindingsToMesh(entity->getMesh().get(), mesh->subMeshes());
    syncEditMeshFrom(entity, mesh);

    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == entity)
            edit->notifyMeshDataChanged();
    }
    if (auto* uv = UVEditorController::instance())
        uv->refreshAfterUvEdit();
}

void commitTopologyMeshState(Ogre::Entity* entity, EditableMesh* mesh)
{
    if (!entity || !mesh)
        return;

    mesh->resizeEntityBuffers(entity);
    UvSeamData::writeBindingsToMesh(entity->getMesh().get(), mesh->subMeshes());
    EditModeController::rewriteEntityAfterTopologyChange(entity);
    syncEditMeshFrom(entity, mesh);

    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == entity)
            edit->notifyMeshDataChanged();
    }
    if (auto* uv = UVEditorController::instance()) {
        uv->clearUvSelection();
        uv->refreshAfterUvEdit();
    }
}

} // namespace

EditableMesh* meshForEntity(Ogre::Entity* entity)
{
    if (!entity)
        return nullptr;
    if (auto* edit = EditModeController::instance()) {
        if (edit->isEditModeActive() && edit->editEntity() == entity && edit->currentMesh())
            return edit->currentMesh();
    }
    if (auto* uv = UVEditorController::instance()) {
        if (auto* wm = uv->workingMeshForEntity(entity))
            return wm;
    }
    return nullptr;
}

// ── UvSeamMarkCommand ───────────────────────────────────────────────────────

UvSeamMarkCommand::UvSeamMarkCommand(Ogre::Entity* entity,
                                     std::vector<EdgeChange> changes,
                                     const QString& description,
                                     QUndoCommand* parent)
    : QUndoCommand(description, parent)
    , m_entity(entity)
    , m_changes(std::move(changes))
{
}

void UvSeamMarkCommand::apply(bool useNew)
{
    EditableMesh* mesh = meshForEntity(m_entity);
    if (!mesh)
        return;
    for (const auto& ch : m_changes) {
        if (ch.subMeshIndex >= mesh->subMeshes().size())
            continue;
        auto& sub = mesh->subMeshes()[ch.subMeshIndex];
        const unsigned int a = static_cast<unsigned int>(ch.edgeKey >> 32);
        const unsigned int b = static_cast<unsigned int>(ch.edgeKey & 0xFFFFFFFFu);
        UvSeamData::setSeam(sub, a, b, useNew ? ch.newSeam : ch.oldSeam);
    }
    UvSeamData::writeBindingsToMesh(m_entity->getMesh().get(), mesh->subMeshes());
    syncEditMeshFrom(m_entity, mesh);
    if (auto* uv = UVEditorController::instance())
        uv->refreshAfterUvEdit();
}

void UvSeamMarkCommand::undo()
{
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.seam"), QStringLiteral("Undo seam mark"));
    apply(false);
}

void UvSeamMarkCommand::redo()
{
    if (m_firstRedo) {
        m_firstRedo = false;
        return;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.seam"), QStringLiteral("Redo seam mark"));
    apply(true);
}

// ── UvPinCommand ──────────────────────────────────────────────────────────────

UvPinCommand::UvPinCommand(Ogre::Entity* entity,
                           std::vector<PinChange> changes,
                           const QString& description,
                           QUndoCommand* parent)
    : QUndoCommand(description, parent)
    , m_entity(entity)
    , m_changes(std::move(changes))
{
}

void UvPinCommand::apply(bool useNew)
{
    EditableMesh* mesh = meshForEntity(m_entity);
    if (!mesh)
        return;
    for (const auto& ch : m_changes) {
        if (ch.subMeshIndex >= mesh->subMeshes().size())
            continue;
        UvSeamData::setPinned(mesh->subMeshes()[ch.subMeshIndex], ch.vertexIndex,
                              useNew ? ch.newPinned : ch.oldPinned);
    }
    UvSeamData::writeBindingsToMesh(m_entity->getMesh().get(), mesh->subMeshes());
    syncEditMeshFrom(m_entity, mesh);
    if (auto* uv = UVEditorController::instance())
        uv->refreshAfterUvEdit();
}

void UvPinCommand::undo()
{
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.pin"), QStringLiteral("Undo UV pin"));
    apply(false);
}

void UvPinCommand::redo()
{
    if (m_firstRedo) {
        m_firstRedo = false;
        return;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.pin"), QStringLiteral("Redo UV pin"));
    apply(true);
}

// ── UvSeamTopologyCommand ─────────────────────────────────────────────────────

UvSeamTopologyCommand::UvSeamTopologyCommand(Ogre::Entity* entity,
                                             std::vector<EditableSubMesh> before,
                                             std::vector<EditableSubMesh> after,
                                             const QString& description,
                                             bool topologyChange,
                                             QUndoCommand* parent)
    : QUndoCommand(description, parent)
    , m_entity(entity)
    , m_before(std::move(before))
    , m_after(std::move(after))
    , m_topologyChange(topologyChange)
{
}

void UvSeamTopologyCommand::applyMesh(const std::vector<EditableSubMesh>& state)
{
    EditableMesh* mesh = meshForEntity(m_entity);
    if (!mesh)
        return;
    mesh->subMeshes() = state;
    if (auto* uv = UVEditorController::instance()) {
        if (EditableMesh* wm = uv->workingMeshForEntity(m_entity); wm && wm != mesh)
            wm->subMeshes() = state;
    }
    if (m_topologyChange)
        commitTopologyMeshState(m_entity, mesh);
    else
        commitUvMeshState(m_entity, mesh);
}

void UvSeamTopologyCommand::undo()
{
    applyMesh(m_before);
}

void UvSeamTopologyCommand::redo()
{
    if (m_firstRedo) {
        m_firstRedo = false;
        return;
    }
    applyMesh(m_after);
}
