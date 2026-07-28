#include "PartOpsController.h"

#include "SelectionSet.h"
#include "UndoManager.h"
#include "SentryReporter.h"
#include "commands/SplitMeshCommand.h"
#include "commands/ExplodePartsCommand.h"
#include "commands/JoinPartsCommand.h"
#include "commands/AddPrintPegsCommand.h"

#include <OgreEntity.h>
#include <OgreMesh.h>

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

bool PartOpsController::canExplode() const
{
    const auto* sel = SelectionSet::getSingleton();
    if (!sel)
        return false;
    const QList<Ogre::Entity*> entities = sel->getResolvedEntities();
    if (entities.size() != 1 || !entities.first() || !entities.first()->getMesh())
        return false;
    // Nothing to explode from a single-submesh mesh.
    return entities.first()->getMesh()->getNumSubMeshes() >= 2;
}

bool PartOpsController::canJoin() const
{
    const auto* sel = SelectionSet::getSingleton();
    if (!sel)
        return false;
    // Count only real mesh entities — getResolvedEntities() can contain nulls
    // (mixed selections with non-mesh nodes), and canExplode() already guards
    // that. Join needs >= 2 ACTUAL parts, else the button would enable on a
    // 1-mesh+1-null selection and the command would be built with one name.
    int meshCount = 0;
    for (Ogre::Entity* e : sel->getResolvedEntities())
        if (e && e->getMesh())
            ++meshCount;
    return meshCount >= 2;
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

void PartOpsController::explodeSelected(double distance, bool capBoundaries)
{
    const auto* sel = SelectionSet::getSingleton();
    if (!sel) {
        emit explodeFinished(tr("No selection."), true);
        return;
    }
    const QList<Ogre::Entity*> entities = sel->getResolvedEntities();
    if (entities.size() != 1 || !entities.first() || !entities.first()->getMesh()) {
        emit explodeFinished(tr("Select a single mesh to explode."), true);
        return;
    }
    if (entities.first()->getMesh()->getNumSubMeshes() < 2) {
        emit explodeFinished(tr("Mesh has a single part — split it first."), true);
        return;
    }

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("explode_parts"));
    const std::string entName = entities.first()->getName();
    auto* cmd = new ExplodePartsCommand(entName, static_cast<float>(distance), capBoundaries);
    UndoManager::getSingleton()->push(cmd);

    if (!cmd->ok()) {
        emit explodeFinished(cmd->error().isEmpty() ? tr("Explode failed.") : cmd->error(), true);
        return;
    }
    emit explodeFinished(tr("Exploded into %1 part nodes.").arg(cmd->createdParts()), false);
}

void PartOpsController::joinSelected()
{
    const auto* sel = SelectionSet::getSingleton();
    if (!sel) {
        emit joinFinished(tr("No selection."), true);
        return;
    }
    // Collect only real mesh entities — getResolvedEntities() can contain nulls
    // (non-mesh nodes in a mixed selection), so gating on the raw size would let
    // a 1-mesh+1-null selection through with a single real part. Build the name
    // list first, then require >= 2 ACTUAL parts.
    std::vector<std::string> names;
    QString fusedBase;
    for (Ogre::Entity* e : sel->getResolvedEntities()) {
        if (!e || !e->getMesh())
            continue;
        names.push_back(e->getName());
        // The fused node is named after the first selected part with a "_fused"
        // suffix (Manager uniquifies), so it's obviously the join result.
        if (fusedBase.isEmpty())
            fusedBase = QString::fromStdString(e->getName()) + QStringLiteral("_fused");
    }
    if (names.size() < 2) {
        emit joinFinished(tr("Select two or more parts to join."), true);
        return;
    }
    const int partCount = static_cast<int>(names.size());

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("join_parts"));
    auto* cmd = new JoinPartsCommand(std::move(names), fusedBase);
    UndoManager::getSingleton()->push(cmd);

    if (!cmd->ok()) {
        emit joinFinished(cmd->error().isEmpty() ? tr("Join failed.") : cmd->error(), true);
        return;
    }
    emit joinFinished(tr("Joined %1 parts into one mesh (%2 submeshes).")
                          .arg(partCount).arg(cmd->createdSubMeshes()), false);
}

void PartOpsController::preparePrintSplit(double clearance, double pegRadius,
                                          double pegDepth, int maxPegsPerBoundary)
{
    const auto* sel = SelectionSet::getSingleton();
    if (!sel) {
        emit printPrepFinished(tr("No selection."), true);
        return;
    }
    const QList<Ogre::Entity*> entities = sel->getResolvedEntities();
    if (entities.size() != 1 || !entities.first() || !entities.first()->getMesh()) {
        emit printPrepFinished(tr("Select a single split mesh."), true);
        return;
    }
    if (entities.first()->getMesh()->getNumSubMeshes() < 2) {
        emit printPrepFinished(tr("Mesh has a single part — split it into parts first."), true);
        return;
    }

    SubMeshOps::PegOptions opts;
    opts.clearance = static_cast<float>(clearance);
    opts.pegRadius = static_cast<float>(pegRadius);
    opts.pegDepth = static_cast<float>(pegDepth);
    opts.maxPegsPerBoundary = maxPegsPerBoundary;

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("prepare_print_split"));
    const std::string entName = entities.first()->getName();
    auto* cmd = new AddPrintPegsCommand(entName, opts);
    UndoManager::getSingleton()->push(cmd);

    if (!cmd->ok()) {
        emit printPrepFinished(cmd->error().isEmpty() ? tr("Print prep failed.") : cmd->error(), true);
        return;
    }
    if (cmd->peggedBoundaries() == 0) {
        emit printPrepFinished(
            tr("No stable part boundary found — no pegs added. Try adjusting the peg size."), true);
        return;
    }
    emit printPrepFinished(
        tr("Added %1 pegs across %2 boundaries.").arg(cmd->totalPegs()).arg(cmd->peggedBoundaries()),
        false);
}
