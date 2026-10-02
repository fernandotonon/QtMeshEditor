#include "AnimGeneratorCommands.h"

#include "AnimGeneratorManager.h"
#include "SentryReporter.h"

AnimGeneratorDocCommand::AnimGeneratorDocCommand(const QString& text, QJsonObject before, QJsonObject after,
                                                 QUndoCommand* parent)
    : QUndoCommand(text, parent), m_before(std::move(before)), m_after(std::move(after))
{
}

void AnimGeneratorDocCommand::undo()
{
    AnimGeneratorManager::instance()->applyDocument(m_before);
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.generator.undo"), text());
}

void AnimGeneratorDocCommand::redo()
{
    if (m_skipFirstRedo) { m_skipFirstRedo = false; return; }
    AnimGeneratorManager::instance()->applyDocument(m_after);
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.generator.redo"), text());
}
