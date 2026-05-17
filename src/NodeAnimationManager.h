/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef NODEANIMATIONMANAGER_H
#define NODEANIMATIONMANAGER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QtQml/qqmlregistration.h>

#include <OgreVector.h>
#include <OgreQuaternion.h>

namespace Ogre { class SceneNode; class Animation; class NodeAnimationTrack; }

/**
 * @brief QML_SINGLETON owning per-scene-node transform-animation clips.
 *
 * Wraps Ogre's `SceneManager` Animation + NodeAnimationTrack subsystem,
 * which QtMeshEditor never previously exercised. Use cases: animated
 * props, doors, simple machinery, scripted camera moves, animated
 * lights — anything that needs a transform track without a skeleton.
 *
 * Slice C1 (this file) ships the *data layer* surface — the manager
 * owns the clips, you create them by name, push keyframes, list,
 * delete. The Inspector subgroup, dope-sheet integration, undo
 * commands, and CLI/MCP land in C2..Cn (mirroring the morph slice
 * pattern: data first, UI per-slice).
 *
 * Clips are owned by `Manager::getSceneMgr()` so they share lifetime
 * with the scene. When the scene is destroyed (asset close, exit)
 * everything goes with it; we don't manually destroy on shutdown.
 */
class NodeAnimationManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    static NodeAnimationManager* instance();
    static NodeAnimationManager* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// Create a named clip with `length` seconds duration. Returns
    /// false if the name already exists on the scene (clip names are
    /// unique within `Ogre::SceneManager`'s animation table).
    bool createClip(const QString& name, double length);

    /// Delete a clip and its driving AnimationState. Returns false
    /// if the clip doesn't exist.
    bool deleteClip(const QString& name);

    /// Add (or overwrite) a keyframe for `nodeName` on `clipName` at
    /// `time`. Returns false on missing clip, missing node, negative
    /// time, or time > clip length. If the same node already has a
    /// keyframe within 1ms of `time`, that keyframe is updated in
    /// place — keeps the user's edits idempotent.
    bool addKeyframe(const QString& clipName,
                     const QString& nodeName,
                     double time,
                     const Ogre::Vector3& translate,
                     const Ogre::Quaternion& rotation,
                     const Ogre::Vector3& scale);

    /// Toggle whether the clip is playing. Returns false on missing
    /// clip; otherwise true.
    bool setClipEnabled(const QString& name, bool enabled);

    /// All clip names on the current scene, in creation order.
    Q_INVOKABLE QStringList listClips() const;

    /// Per-node sorted list of keyframe times for `clipName`. Empty
    /// when the clip is missing or the node has no keyframes.
    Q_INVOKABLE QList<double> keyTimesForNode(const QString& clipName,
                                              const QString& nodeName) const;

    /// QML-friendly variants of the setters; pass POD-ish doubles in
    /// instead of Ogre types so the API binds cleanly into JS.
    Q_INVOKABLE bool createClipForName(const QString& name, double length);
    Q_INVOKABLE bool deleteClipForName(const QString& name);
    Q_INVOKABLE bool addKeyframeFromQml(const QString& clipName,
                                         const QString& nodeName,
                                         double time,
                                         double tx, double ty, double tz,
                                         double qw, double qx, double qy, double qz,
                                         double sx, double sy, double sz);

signals:
    /// The set of clips visible on the scene changed (create / delete).
    void clipsChanged();
    /// A clip's keyframes changed (add / overwrite / future remove).
    void keyframesChanged(const QString& clipName);

private:
    explicit NodeAnimationManager(QObject* parent = nullptr);
    ~NodeAnimationManager() override;

    /// Pick a stable track handle for `nodeName` within `clip`. Same
    /// hash strategy across calls so two add operations on the same
    /// node land on the same track.
    static unsigned short trackHandleForNode(const QString& nodeName);

    /// Look up a SceneNode by name on the current scene. Returns
    /// null when the node doesn't exist. Centralised so name-not-
    /// found behaviour is consistent across all setters.
    static Ogre::SceneNode* findSceneNode(const QString& nodeName);

    static NodeAnimationManager* s_instance;
};

#endif // NODEANIMATIONMANAGER_H
