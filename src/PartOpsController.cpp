#include "PartOpsController.h"

#include "SelectionSet.h"
#include "UndoManager.h"
#include "SentryReporter.h"
#include "commands/SplitMeshCommand.h"

#include <OgreEntity.h>

PartOpsController* PartOpsController::m_pSingleton = nullptr;

PartOpsController* PartOpsController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new PartOpsController();
    return m_pSingleton;
}

PartOpsController* PartOpsController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void PartOpsController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

PartOpsController::PartOpsController() : QObject(nullptr)
{
    if (auto* sel = SelectionSet::getSingleton())
        connect(sel, &SelectionSet::selectionChanged, this, &PartOpsController::selectionChanged);
}

bool PartOpsController::hasSelection() const
{
    const auto* sel = SelectionSet::getSingleton();
    return sel && sel->getResolvedEntities().size() == 1;
}

void PartOpsController::splitSelectedIntoParts(const QString& upAxis, const QString& category,
                                               bool noModel)
{
    const auto* sel = SelectionSet::getSingleton();
    if (!sel) {
        emit splitFinished(tr("No selection."), true);
        return;
    }
    const QList<Ogre::Entity*> entities = sel->getResolvedEntities();
    if (entities.size() != 1 || !entities.first()) {
        emit splitFinished(tr("Select a single mesh to split."), true);
        return;
    }

    int axis = 1;
    const QString a = upAxis.trimmed().toLower();
    if (a == QStringLiteral("x")) axis = 0;
    else if (a == QStringLiteral("z")) axis = 2;

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("split_into_parts%1")
                                      .arg(noModel ? QStringLiteral(" offline") : QString()));
    const std::string entName = entities.first()->getName();
    // push() runs redo() synchronously (AutoRigController pattern); read the
    // result back. A failed split leaves a harmless no-op on the undo stack.
    auto* cmd = new SplitMeshCommand(entName, axis, category, noModel,
                                     QStringLiteral("Body"));
    UndoManager::getSingleton()->push(cmd);

    if (!cmd->ok()) {
        emit splitFinished(cmd->error().isEmpty() ? tr("Split failed.") : cmd->error(), true);
        return;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.parts.split_segments"),
                                  QStringLiteral("gui parts=%1").arg(cmd->createdSubMeshes()));
    emit splitFinished(tr("Split into %1 part submeshes.").arg(cmd->createdSubMeshes()), false);
}
