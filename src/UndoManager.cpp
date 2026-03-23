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

void UndoManager::undo()
{
    if (mUndoStack.canUndo())
        mUndoStack.undo();
}

void UndoManager::redo()
{
    if (mUndoStack.canRedo())
        mUndoStack.redo();
}

void UndoManager::clear()
{
    mUndoStack.clear();
}
