#pragma once

#include "SkeletonEditor.h"

#include <QUndoCommand>
#include <QString>

#include <vector>

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

class ReparentBoneCommand : public QUndoCommand
{
public:
    ReparentBoneCommand(std::string entityName,
                        QString boneName,
                        QString newParentName,
                        SkeletonEditor::ReparentOptions opts,
                        QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool applied() const { return m_applied; }

private:
    std::string m_entityName;
    QString m_boneName;
    QString m_newParentName;
    SkeletonEditor::ReparentOptions m_opts;
    SkeletonEditor::Snapshot m_before;
    bool m_applied = false;
    bool m_firstRedo = true;
};

class SplitBoneCommand : public QUndoCommand
{
public:
    SplitBoneCommand(std::string entityName,
                     QString boneName,
                     float t,
                     QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool applied() const { return m_applied; }
    const QString& splitBoneName() const { return m_splitBoneName; }

private:
    std::string m_entityName;
    QString m_boneName;
    float m_t = 0.5f;
    QString m_splitBoneName;
    SkeletonEditor::Snapshot m_before;
    bool m_applied = false;
    bool m_firstRedo = true;
};

class ConnectBoneCommand : public QUndoCommand
{
public:
    ConnectBoneCommand(std::string entityName,
                       QString boneName,
                       bool connected,
                       QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool applied() const { return m_applied; }

private:
    std::string m_entityName;
    QString m_boneName;
    bool m_connected = true;
    SkeletonEditor::Snapshot m_before;
    bool m_applied = false;
    bool m_firstRedo = true;
};

class AttachBoneToEntityCommand : public QUndoCommand
{
public:
    AttachBoneToEntityCommand(std::string srcEntityName,
                              QStringList boneNames,
                              std::string dstEntityName,
                              SkeletonEditor::AttachOptions opts,
                              QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool applied() const { return m_applied; }
    const QString& attachedBoneName() const { return m_attachedBoneName; }

private:
    std::string m_srcEntityName;
    QStringList m_boneNames;
    std::string m_dstEntityName;
    SkeletonEditor::AttachOptions m_opts;
    SkeletonEditor::Snapshot m_dstBefore;
    QString m_attachedBoneName;
    bool m_applied = false;
    bool m_firstRedo = true;
};

/// Undoable rest-pose capture / snap / reset (epic #554 slice C #557).
class SetRestPoseCommand : public QUndoCommand
{
public:
    enum class Op { CaptureAll, SnapSelected, Reset };

    struct ExplicitPose {
        QString boneName;
        Ogre::Vector3 position = Ogre::Vector3::ZERO;
        Ogre::Quaternion orientation = Ogre::Quaternion::IDENTITY;
        Ogre::Vector3 scale = Ogre::Vector3::UNIT_SCALE;
    };

    SetRestPoseCommand(std::string entityName,
                       Op op,
                       QStringList boneNames = {},
                       QUndoCommand* parent = nullptr);

    /// Gizmo CommitBind path: commit these exact local TRS values as rest
    /// (do not re-read the skeleton — the instance may already have been
    /// reset by an animation tick between drag end and undo-push).
    SetRestPoseCommand(std::string entityName,
                       std::vector<ExplicitPose> explicitPoses,
                       QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

    bool applied() const { return m_applied; }

private:
    std::string m_entityName;
    Op m_op = Op::CaptureAll;
    QStringList m_boneNames;
    std::vector<ExplicitPose> m_explicitPoses;
    SkeletonEditor::Snapshot m_before;
    SkeletonEditor::Snapshot m_after;
    bool m_applied = false;
    bool m_firstRedo = true;
};
