#include "WeldVerticesCommand.h"

#include "../Manager.h"
#include "../SentryReporter.h"

#include <OgreEntity.h>
#include <OgreSubMesh.h>

namespace {
Ogre::Entity* weldResolveEntity(const std::string& name)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    for (Ogre::Entity* ent : mgr->getEntities()) {
        if (!ent || ent->getMovableType() != "Entity") continue;
        if (ent->getName() == name) return ent;
    }
    return nullptr;
}
} // namespace

WeldVerticesCommand::WeldVerticesCommand(std::string entityName,
                                         QUndoCommand* parent)
    : QUndoCommand(parent)
    , m_entityName(std::move(entityName))
{
    setText(QStringLiteral("Weld duplicate vertices"));
}

void WeldVerticesCommand::redo()
{
    Ogre::Entity* entity = weldResolveEntity(m_entityName);
    if (!entity || !entity->getMesh()) return;
    Ogre::MeshPtr mesh = entity->getMesh();

    if (m_firstRedo) {
        m_firstRedo = false;
        // Snapshot index buffers + assignment lists for undo.
        for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
            Ogre::SubMesh* sub = mesh->getSubMesh(si);
            if (sub && sub->indexData && sub->indexData->indexBuffer) {
                IndexSnapshot snap;
                snap.submesh = si;
                const auto& buf = sub->indexData->indexBuffer;
                snap.bytes.resize(static_cast<int>(buf->getSizeInBytes()));
                buf->readData(0, buf->getSizeInBytes(), snap.bytes.data());
                m_indexSnaps.push_back(std::move(snap));
            }
            if (sub && !sub->useSharedVertices) {
                AssignSnapshot as;
                as.submesh = si;
                for (const auto& [vi, vba] : sub->getBoneAssignments())
                    as.list.push_back(vba);
                m_assignSnaps.push_back(std::move(as));
            }
        }
        AssignSnapshot shared;
        shared.submesh = -1;
        for (const auto& [vi, vba] : mesh->getBoneAssignments())
            shared.list.push_back(vba);
        m_assignSnaps.push_back(std::move(shared));
    }

    m_report = MeshWeldOps::apply(entity);
    m_applied = m_report.ok
                && (m_report.weldedVertices > 0 || m_report.weightsUnified > 0);
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.weld"),
        QStringLiteral("%1: %2 refs remapped, %3 weights unified")
            .arg(QString::fromStdString(m_entityName))
            .arg(m_report.weldedVertices)
            .arg(m_report.weightsUnified));
}

void WeldVerticesCommand::undo()
{
    Ogre::Entity* entity = weldResolveEntity(m_entityName);
    if (!entity || !entity->getMesh()) return;
    Ogre::MeshPtr mesh = entity->getMesh();

    for (const IndexSnapshot& snap : m_indexSnaps) {
        if (snap.submesh >= mesh->getNumSubMeshes()) continue;
        Ogre::SubMesh* sub = mesh->getSubMesh(snap.submesh);
        if (!sub || !sub->indexData || !sub->indexData->indexBuffer) continue;
        const auto& buf = sub->indexData->indexBuffer;
        if (static_cast<int>(buf->getSizeInBytes()) != snap.bytes.size())
            continue;
        buf->writeData(0, buf->getSizeInBytes(), snap.bytes.constData(), true);
    }

    if (mesh->hasSkeleton()) {
        for (const AssignSnapshot& as : m_assignSnaps) {
            if (as.submesh < 0) {
                mesh->clearBoneAssignments();
                for (const auto& vba : as.list)
                    mesh->addBoneAssignment(vba);
                if (mesh->sharedVertexData)
                    mesh->_compileBoneAssignments();
            } else if (as.submesh < mesh->getNumSubMeshes()) {
                Ogre::SubMesh* sub = mesh->getSubMesh(
                    static_cast<unsigned short>(as.submesh));
                sub->clearBoneAssignments();
                for (const auto& vba : as.list)
                    sub->addBoneAssignment(vba);
                sub->_compileBoneAssignments();
            }
        }
    }
    m_applied = false;
}
