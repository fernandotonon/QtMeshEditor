#ifndef MOTION_GRAPH_COMMANDS_H
#define MOTION_GRAPH_COMMANDS_H

// Undo step for motion-graph authoring (#526): every edit swaps the entity's
// graph document. An empty document means "no graph". The first redo() is
// skipped — the manager applied the change before pushing.

#include <QJsonObject>
#include <QUndoCommand>

class MotionGraphDocCommand : public QUndoCommand
{
public:
    MotionGraphDocCommand(const QString& text, QString entity, QJsonObject before, QJsonObject after,
                          QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    QString m_entity;
    QJsonObject m_before;
    QJsonObject m_after;
    bool m_skipFirstRedo = true;
};

#endif // MOTION_GRAPH_COMMANDS_H
