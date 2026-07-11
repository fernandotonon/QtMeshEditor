#pragma once

#include "LightManager.h"
#include "LightRigLibrary.h"

#include <OgreColourValue.h>

#include <QUndoCommand>
#include <QList>
#include <QString>

class CreateLightCommand : public QUndoCommand
{
public:
    explicit CreateLightCommand(const LightSnapshot& snapshot, QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    LightSnapshot m_snapshot;
    bool m_firstRedo = true;
};

class DeleteLightsCommand : public QUndoCommand
{
public:
    explicit DeleteLightsCommand(QList<LightSnapshot> snapshots, QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    QList<LightSnapshot> m_snapshots;
    bool m_firstRedo = true;
};

class RenameLightCommand : public QUndoCommand
{
public:
    RenameLightCommand(const QString& oldName, const QString& newName, QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    QString m_oldName;
    QString m_newName;
    bool m_firstRedo = true;
};

class DuplicateLightsCommand : public QUndoCommand
{
public:
    explicit DuplicateLightsCommand(QList<LightSnapshot> cloneSnapshots, QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    QList<LightSnapshot> m_cloneSnapshots;
    bool m_firstRedo = true;
};

enum class LightPropertyClass
{
    Type = 0,
    Enabled,
    Colour,
    Intensity,
    Range,
    Attenuation,
    SpotCone,
    Shadow,
    Linking,
    IesProfile,
    AreaShape
};

QString lightPropertyClassLabel(LightPropertyClass propertyClass);

class EditLightPropertyCommand : public QUndoCommand
{
public:
    EditLightPropertyCommand(LightPropertyClass propertyClass,
                             QList<LightSnapshot> before,
                             QList<LightSnapshot> after,
                             QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;
    int id() const override;
    bool mergeWith(const QUndoCommand* other) override;

private:
    static void applySnapshots(const QList<LightSnapshot>& snapshots);

    LightPropertyClass m_propertyClass;
    QList<LightSnapshot> m_before;
    QList<LightSnapshot> m_after;
    bool m_firstRedo = true;
};

class ApplyLightRigCommand : public QUndoCommand
{
public:
    ApplyLightRigCommand(const LightRigApplyResult& result, QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;

private:
    QString m_rigId;
    QString m_rigGroupNodeName;
    QList<LightSnapshot> m_addedLights;
    QList<RemovedRigGroupSnapshot> m_removedRigGroups;
    QList<LightSnapshot> m_removedUserLights;
    QList<LightSnapshot> m_removedLights;
    Ogre::ColourValue m_ambientBefore;
    Ogre::ColourValue m_ambientAfter;
    QString m_hdriBefore;
    QString m_suggestedHdri;
    bool m_replaceExisting = false;
    bool m_firstRedo = true;
};

class SetSceneAmbientCommand : public QUndoCommand
{
public:
    SetSceneAmbientCommand(const Ogre::ColourValue& before,
                           const Ogre::ColourValue& after,
                           QUndoCommand* parent = nullptr);

    void undo() override;
    void redo() override;
    int id() const override;
    bool mergeWith(const QUndoCommand* other) override;

private:
    static void applyAmbient(const Ogre::ColourValue& ambient);

    Ogre::ColourValue m_before;
    Ogre::ColourValue m_after;
    bool m_firstRedo = true;
};
