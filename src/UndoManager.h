#ifndef UNDO_MANAGER_H
#define UNDO_MANAGER_H

#include <QObject>
#include <QUndoStack>

class UndoManager : public QObject
{
    Q_OBJECT

public:
    static UndoManager* getSingleton();
    static void kill();

    QUndoStack* stack() { return &mUndoStack; }

    void push(QUndoCommand* cmd);
    bool canUndo() const;
    bool canRedo() const;

    /** Route undo/redo through a SESSION stack first (a modal editing session
     *  such as the lattice deformer keeps its transient edits there). While
     *  set, undo()/redo() act on `stack` when it has something to undo/redo
     *  and fall through to the global stack otherwise, so history recorded
     *  before the session stays reachable. Pass nullptr to detach. `push()`
     *  always targets the GLOBAL stack — the owner pushes onto its own. The
     *  caller owns the stack and must detach before destroying it. */
    void setSessionStack(QUndoStack* stack);
    QUndoStack* sessionStack() const { return mSessionStack; }

public slots:
    void undo();
    void redo();
    void clear();

signals:
    void undoTextChanged(const QString& text);
    void redoTextChanged(const QString& text);

private:
    UndoManager();
    ~UndoManager() override = default;

    static UndoManager* m_pSingleton;
    QUndoStack mUndoStack;
    QUndoStack* mSessionStack = nullptr;
};

#endif // UNDO_MANAGER_H
