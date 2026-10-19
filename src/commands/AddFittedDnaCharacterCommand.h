#ifndef ADD_FITTED_DNA_CHARACTER_COMMAND_H
#define ADD_FITTED_DNA_CHARACTER_COMMAND_H

#include <QString>
#include <QUndoCommand>

#include <memory>
#include <string>

namespace Dna { struct Rig; }

/**
 * Undoable commit of a MetaHuman fit (epic #1095, Phase 4): adds the FITTED character to the scene as a
 * new node next to the original — nothing existing is modified, which is what makes it reversible.
 * redo(): import the fitted rig (with live body correctives when the source DNA has them) and select it.
 * undo(): destroy that node. The rig is held by shared_ptr so redo can rebuild it any number of times.
 */
class AddFittedDnaCharacterCommand : public QUndoCommand
{
public:
    /** @param rig         the fitted rig (editor frame, metres).
     *  @param sourcePath  the DNA file the rig came from (the corrective evaluator is created from it).
     *  @param baseName    base for the node/entity name; a counter keeps it unique. */
    AddFittedDnaCharacterCommand(std::shared_ptr<Dna::Rig> rig, QString sourcePath, QString baseName);

    void redo() override;
    void undo() override;

    /** The outcome, shared with the caller: QUndoStack::push() runs redo() and DELETES a command that marked
     *  itself obsolete, so the caller must not read the command after pushing — it reads this instead. */
    struct Outcome {
        bool created = false;
        QString nodeName, error;
    };
    std::shared_ptr<Outcome> outcome() const { return m_outcome; }

    QString nodeName() const { return m_nodeName; }
    /** false when the first redo could not build the character (the command then does nothing). */
    bool created() const { return m_created; }
    QString error() const { return m_error; }

private:
    std::shared_ptr<Dna::Rig> m_rig;
    QString m_sourcePath, m_baseName, m_nodeName, m_error;
    bool m_created = false;
    int m_revision = 0;
    std::shared_ptr<Outcome> m_outcome = std::make_shared<Outcome>();
};

#endif
