#include "ConstraintCommands.h"

#include "ConstraintManager.h"
#include "SentryReporter.h"

ConstraintDocCommand::ConstraintDocCommand(const QString& text, QJsonObject before, QJsonObject after,
                                           QJsonArray tracksBefore, QJsonArray tracksAfter, QUndoCommand* parent)
    : QUndoCommand(text, parent), m_before(std::move(before)), m_after(std::move(after)),
      m_tracksBefore(std::move(tracksBefore)), m_tracksAfter(std::move(tracksAfter))
{
}

void ConstraintDocCommand::undo()
{
    ConstraintManager* m = ConstraintManager::instance();
    if (!m_tracksBefore.isEmpty()) m->restoreTracks(m_tracksBefore);
    m->applyDocument(m_before);
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.constraint.undo"), text());
}

void ConstraintDocCommand::redo()
{
    if (m_skipFirstRedo) { m_skipFirstRedo = false; return; }
    ConstraintManager* m = ConstraintManager::instance();
    if (!m_tracksAfter.isEmpty()) m->restoreTracks(m_tracksAfter);
    m->applyDocument(m_after);
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.constraint.redo"), text());
}
