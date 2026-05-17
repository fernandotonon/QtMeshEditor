/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef NODE_ANIM_COMMANDS_H
#define NODE_ANIM_COMMANDS_H

#include <QHash>
#include <QString>
#include <QUndoCommand>

#include <OgreQuaternion.h>
#include <OgreVector.h>

#include <optional>
#include <vector>

// Per-keyframe snapshot inside a NodeAnimationTrack. Used by both
// the bulk DeleteClipCommand (which captures every keyframe on
// every track) and SetNodeKeyframeCommand's undo path.
struct NodeKeyframeSnapshot {
    double time = 0.0;
    Ogre::Vector3 translate = Ogre::Vector3::ZERO;
    Ogre::Quaternion rotation = Ogre::Quaternion::IDENTITY;
    Ogre::Vector3 scale = Ogre::Vector3(1, 1, 1);
};

// Per-node track snapshot — the node it animates, plus all its
// keyframes in time order. `nodeName` is the SceneNode name as the
// manager sees it; the rebuild path re-resolves it through
// Manager::getSceneMgr()->getSceneNode.
struct NodeTrackSnapshot {
    QString nodeName;
    std::vector<NodeKeyframeSnapshot> keys;
};

// CreateClipCommand: creates a fresh, empty clip. Undo destroys it.
// Trivial because the snapshot is just (name, length) — nothing
// exists yet to snapshot.
class CreateNodeAnimClipCommand : public QUndoCommand
{
public:
    CreateNodeAnimClipCommand(const QString& name,
                              double length,
                              QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    QString mName;
    double mLength = 0.0;
};

// DeleteClipCommand: snapshots every track + keyframe at construction
// so undo rebuilds the full clip + tracks + keyframes. Captured
// while the clip still exists; the redo path then drops the live
// clip.
class DeleteNodeAnimClipCommand : public QUndoCommand
{
public:
    DeleteNodeAnimClipCommand(const QString& name,
                              QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    QString mName;
    double mLength = 0.0;
    std::vector<NodeTrackSnapshot> mTracks;
};

// SetNodeKeyframeCommand: writes one keyframe (or overwrites an
// existing one within the manager's merge epsilon). On undo we
// either delete the keyframe we just added or restore the prior
// values if we overwrote one — `mPriorKeyframe` carries that
// disambiguation.
class SetNodeKeyframeCommand : public QUndoCommand
{
public:
    SetNodeKeyframeCommand(const QString& clipName,
                           const QString& nodeName,
                           double time,
                           const Ogre::Vector3& translate,
                           const Ogre::Quaternion& rotation,
                           const Ogre::Vector3& scale,
                           QUndoCommand* parent = nullptr);
    void undo() override;
    void redo() override;

private:
    QString mClipName;
    QString mNodeName;
    NodeKeyframeSnapshot mNew;
    // Empty when there was no prior keyframe (i.e. we're adding a
    // new one). Populated when we overwrote an existing key within
    // the manager's merge epsilon — that key's prior state.
    std::optional<NodeKeyframeSnapshot> mPriorKeyframe;
    // True when the track itself didn't exist at construction time —
    // undo then drops the whole track, not just the keyframe (avoids
    // leaving an empty NodeAnimationTrack on the clip).
    bool mTrackCreatedByRedo = false;
};

#endif // NODE_ANIM_COMMANDS_H
