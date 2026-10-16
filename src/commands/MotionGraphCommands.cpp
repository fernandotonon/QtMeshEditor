#include "MotionGraphCommands.h"

#include "MotionGraphManager.h"
#include "SentryReporter.h"

MotionGraphDocCommand::MotionGraphDocCommand(const QString& text, QString entity, QJsonObject before,
                                             QJsonObject after, QUndoCommand* parent)
    : QUndoCommand(text, parent), m_entity(std::move(entity)), m_before(std::move(before)), m_after(std::move(after))
{
}

void MotionGraphDocCommand::undo()
{
    MotionGraphManager::instance()->applyGraphJson(m_entity, m_before);
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.graph.undo"), text());
}

void MotionGraphDocCommand::redo()
{
    if (m_skipFirstRedo) { m_skipFirstRedo = false; return; }
    MotionGraphManager::instance()->applyGraphJson(m_entity, m_after);
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.graph.redo"), text());
}
