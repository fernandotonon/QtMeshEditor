#ifndef ANIM_GENERATOR_COMMANDS_H
#define ANIM_GENERATOR_COMMANDS_H

// Undo step for procedural animation generators (#524). Every generator
// mutation — add, remove, parameter edit, mute, bake, path edit — is ONE
// command that swaps two AnimGeneratorManager documents (generators + base
// snapshots). Applying a document restores the tracks that leave it and
// re-materialises the ones in it, so the scene replays exactly. The first
// redo() is skipped: the manager already applied the change before pushing.

#include <QJsonObject>
#include <QUndoCommand>

class AnimGeneratorDocCommand : public QUndoCommand
{
public:
    AnimGeneratorDocCommand(const QString& text, QJsonObject before, QJsonObject after,
                            QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    QJsonObject m_before;
    QJsonObject m_after;
    bool m_skipFirstRedo = true;
};

#endif // ANIM_GENERATOR_COMMANDS_H
