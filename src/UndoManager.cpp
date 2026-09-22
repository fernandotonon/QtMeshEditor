#include "UndoManager.h"

UndoManager* UndoManager::m_pSingleton = nullptr;

UndoManager* UndoManager::getSingleton()
{
    if (!m_pSingleton)
        m_pSingleton = new UndoManager();
    return m_pSingleton;
}

void UndoManager::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

UndoManager::UndoManager() : QObject(nullptr)
{
    connect(&mUndoStack, &QUndoStack::undoTextChanged, this, &UndoManager::undoTextChanged);
    connect(&mUndoStack, &QUndoStack::redoTextChanged, this, &UndoManager::redoTextChanged);
}

void UndoManager::push(QUndoCommand* cmd)
{
    mUndoStack.push(cmd);
}

bool UndoManager::canUndo() const
{
    return (mSessionStack && mSessionStack->canUndo()) || mUndoStack.canUndo();
}

bool UndoManager::canRedo() const
{
    return (mSessionStack && mSessionStack->canRedo()) || mUndoStack.canRedo();
}

void UndoManager::setSessionStack(QUndoStack* stack)
{
    mSessionStack = stack;
}

void UndoManager::undo()
{
    if (mSessionStack && mSessionStack->canUndo()) {
        mSessionStack->undo();
        return;
    }
    if (mUndoStack.canUndo())
        mUndoStack.undo();
}

void UndoManager::redo()
{
    if (mSessionStack && mSessionStack->canRedo()) {
        mSessionStack->redo();
        return;
    }
    if (mUndoStack.canRedo())
        mUndoStack.redo();
}

void UndoManager::clear()
{
    mUndoStack.clear();
}
