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
    bool canUndo() const { return mUndoStack.canUndo(); }
    bool canRedo() const { return mUndoStack.canRedo(); }

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
};

#endif // UNDO_MANAGER_H
