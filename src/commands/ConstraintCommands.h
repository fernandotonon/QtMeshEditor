#ifndef CONSTRAINT_COMMANDS_H
#define CONSTRAINT_COMMANDS_H

// Undo step for animation constraints (#525). Every edit swaps two
// ConstraintManager documents; a bake also carries the snapshots of the
// tracks it rewrote (restored on undo, re-written on redo). The first redo()
// is skipped: the manager already applied the change before pushing.

#include <QJsonArray>
#include <QJsonObject>
#include <QUndoCommand>

class ConstraintDocCommand : public QUndoCommand
{
public:
    ConstraintDocCommand(const QString& text, QJsonObject before, QJsonObject after,
                         QJsonArray tracksBefore = {}, QJsonArray tracksAfter = {},
                         QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    QJsonObject m_before;
    QJsonObject m_after;
    QJsonArray m_tracksBefore;
    QJsonArray m_tracksAfter;
    bool m_skipFirstRedo = true;
};

#endif // CONSTRAINT_COMMANDS_H
