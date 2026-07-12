#pragma once

#include "SkeletonEditor.h"

#include <QUndoCommand>
#include <QString>

/// Undoable bone create (epic #554 slice A).
class CreateBoneCommand : public QUndoCommand
{
public:
    CreateBoneCommand(std::string entityName,
                      SkeletonEditor::CreateOptions opts,
                      QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool applied() const { return m_applied; }
    const QString& createdBoneName() const { return m_createdBoneName; }

private:
    std::string m_entityName;
    SkeletonEditor::CreateOptions m_opts;
    QString m_createdBoneName;
    bool m_applied = false;
    bool m_firstRedo = true;
};

class RemoveBoneCommand : public QUndoCommand
{
public:
    RemoveBoneCommand(std::string entityName,
                      QString boneName,
                      SkeletonEditor::RemoveOptions opts,
                      QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool applied() const { return m_applied; }

private:
    std::string m_entityName;
    QString m_boneName;
    SkeletonEditor::RemoveOptions m_opts;
    SkeletonEditor::Snapshot m_before;
    bool m_applied = false;
    bool m_firstRedo = true;
};

class RenameBoneCommand : public QUndoCommand
{
public:
    RenameBoneCommand(std::string entityName,
                      QString oldName,
                      QString newName,
                      QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool applied() const { return m_applied; }

private:
    std::string m_entityName;
    QString m_oldName;
    QString m_newName;
    bool m_applied = false;
    bool m_firstRedo = true;
};

class DuplicateBoneCommand : public QUndoCommand
{
public:
    DuplicateBoneCommand(std::string entityName,
                         QString sourceBoneName,
                         QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool applied() const { return m_applied; }
    const QString& duplicatedBoneName() const { return m_duplicatedBoneName; }

private:
    std::string m_entityName;
    QString m_sourceBoneName;
    QString m_duplicatedBoneName;
    bool m_applied = false;
    bool m_firstRedo = true;
};

/// Undoable skeleton overlay visibility toggle (Inspector Skeleton checkbox).
class ToggleSkeletonDebugCommand : public QUndoCommand
{
public:
    ToggleSkeletonDebugCommand(QString entityName, bool show, QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    void apply(bool show);

    QString m_entityName;
    bool m_show = false;
};
