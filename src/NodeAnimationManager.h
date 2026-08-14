/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef NODEANIMATIONMANAGER_H
#define NODEANIMATIONMANAGER_H

#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
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

    /// The clip currently being authored/inspected. The Inspector
    /// section and the dope-sheet node band both read this so they
    /// agree on which clip's tracks are shown. Empty when no node
    /// clip is selected. Not persisted — it's editor UI state.
    Q_PROPERTY(QString activeClip READ activeClip WRITE setActiveClip NOTIFY activeClipChanged)
    QString activeClip() const { return m_activeClip; }
    void setActiveClip(const QString& name);

    /// The clip currently in an EDIT session (authoring), or empty. While a
    /// clip is being edited its driving AnimationState is forced DISABLED so
    /// the SceneNode stays free for the gizmo (an enabled state re-drives the
    /// node every frame and locks it). `beginEdit` opens the session (also
    /// sets it active); `endEdit` closes it — after which the clip plays via
    /// the main transport like any other animation. A draft clip (being
    /// edited) is hidden from the main animation list until endEdit.
    Q_PROPERTY(QString editingClip READ editingClip NOTIFY editingClipChanged)
    QString editingClip() const { return m_editingClip; }
    Q_INVOKABLE void beginEdit(const QString& name);
    Q_INVOKABLE void endEdit();
    /// True when `name` is being edited (draft) — used to hide it from the
    /// main animation list and to gate enable.
    Q_INVOKABLE bool isEditing(const QString& name) const { return !name.isEmpty() && m_editingClip == name; }

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

    /// Names of the SceneNodes that have a NodeAnimationTrack on
    /// `clipName`, in track-handle order. Read directly off the Ogre
    /// Animation so it stays correct even after undo/redo rebuilds a
    /// clip (the m_trackHandles allocator map is a runtime cache, not
    /// the source of truth for "which nodes are animated").
    Q_INVOKABLE QStringList animatedNodes(const QString& clipName) const;

    /// Clip duration in seconds, or 0 when the clip is missing.
    Q_INVOKABLE double clipLength(const QString& clipName) const;

    /// Dope-sheet row model: one QVariantMap per animated node,
    /// `{ "node": QString, "keyTimes": QVariantList<double seconds>,
    ///    "channels": { tx:bool, ty:bool, … sz:bool } }`.
    /// Mirrors AnimationControlController::allBoneRows() so the dope
    /// sheet's node band AND the curve editor can reuse the same
    /// rendering pattern. The `channels` field was added in #520 (the
    /// curve editor needs the active-channel map); `keyTimes` stays for
    /// backward compatibility with the dope sheet's existing band. (#520)
    Q_INVOKABLE QVariantList nodeRows(const QString& clipName) const;

    // ── Curve-editor surface (#520) ─────────────────────────────────
    // These mirror AnimationControlController's per-channel bone-curve
    // API (channelValuesAt / setKeyframeValue / setKeyframeValuePreview)
    // so qml/AnimationCurveEditor.qml can edit a node clip's TRS curves
    // IDENTICALLY to how it edits a skeletal bone's tracks. Channel ids
    // are the same tx/ty/tz/rw/rx/ry/rz/sx/sy/sz set; rotation channels
    // recompose the quaternion from all four components then normalise.

    /// The active-channel bool map for (`clipName`, `nodeName`), same
    /// shape/logic as AnimationControlController::collectActiveChannels —
    /// `{ tx:bool, …, sz:bool }`. Empty map when the track is missing.
    Q_INVOKABLE QVariantMap nodeChannels(const QString& clipName,
                                         const QString& nodeName) const;

    /// Per-keyframe values for one channel on (`clipName`, `nodeName`),
    /// in keyframe (time) order — the values the curve editor plots.
    /// Empty when the channel is unknown or the track is missing.
    Q_INVOKABLE QVariantList nodeChannelValuesAt(const QString& clipName,
                                                 const QString& nodeName,
                                                 const QString& channel) const;

    /// Write ONE channel into the keyframe at `time` on
    /// (`clipName`, `nodeName`), leaving the other nine components
    /// untouched, via an UNDOable SetNodeKeyframeCommand (which
    /// snapshots the full prior TRS so undo restores it). Rotation
    /// channels recompose + normalise the quaternion. Returns false
    /// when the channel/clip/node/keyframe is missing.
    Q_INVOKABLE bool setNodeKeyframeValue(const QString& clipName,
                                          const QString& nodeName,
                                          const QString& channel,
                                          double time, double value);

    /// Same as setNodeKeyframeValue but writes straight to the
    /// TransformKeyFrame with NO undo push — for live curve-handle
    /// drags; the caller commits the final value on release. Calls
    /// `track->_keyFrameDataChanged()` after the edit so the change is
    /// picked up (setRotation/setTranslate don't invalidate the track's
    /// interpolation caches — the arm-space gotcha in CLAUDE.md). (#520)
    Q_INVOKABLE bool setNodeKeyframeValuePreview(const QString& clipName,
                                                 const QString& nodeName,
                                                 const QString& channel,
                                                 double time, double value);

    /// Read the FULL TRS of the keyframe at `time` as {tx..sz}. Empty when the
    /// clip/track/keyframe is missing. The curve editor snapshots this at
    /// rotation-drag start so it can restore the exact pre-drag quaternion on
    /// release (a single-component revert can't, since preview normalisation
    /// drifts the other three). (#520)
    Q_INVOKABLE QVariantMap nodeKeyframeTRS(const QString& clipName,
                                            const QString& nodeName,
                                            double time) const;

    /// Restore the whole TRS ({tx..sz}) onto the keyframe at `time` in one shot
    /// (no undo push — the curve editor uses this to revert a drag preview
    /// before committing the undoable value). Returns false when missing. (#520)
    Q_INVOKABLE bool restoreNodeKeyframeTRS(const QString& clipName,
                                            const QString& nodeName,
                                            double time,
                                            const QVariantMap& trs);

    /// Resample ONE curve segment [t0, t1] on (`clipName`, `nodeName`,
    /// `channel`) into dense TransformKeyFrames so live Ogre playback
    /// traces the Bezier/Auto/Stepped shape held in CurveEditModel —
    /// the node-clip equivalent of
    /// AnimationControlController::resampleCurveSegment (bone tracks).
    /// Both endpoints must sit near existing keyframes. Pushes an
    /// UNDOable node-capable ResampleCurveCommand. Returns false when
    /// the clip/node/channel/segment is invalid. (#520)
    Q_INVOKABLE bool resampleNodeCurveSegment(const QString& clipName,
                                              const QString& nodeName,
                                              const QString& channel,
                                              double t0, double t1,
                                              double toleranceMul = 1.0,
                                              int fixedFps = 0);

    /// Resample every segment on (`clipName`, `nodeName`, `channel`) at
    /// the given `density` level (0..6 — matches the bone Bake dropdown:
    /// 0=Sparse, 1=Medium, 2=Dense, 3..6 = 10/15/30/60 FPS exact) inside
    /// ONE undo macro. Node-clip equivalent of
    /// AnimationControlController::resampleAllSegmentsForBone. Returns
    /// the number of segments resampled. (#520)
    Q_INVOKABLE int resampleAllNodeSegments(const QString& clipName,
                                            const QString& nodeName,
                                            const QString& channel,
                                            int density);

    /// Whether `name` names a live scene AnimationState that is enabled.
    Q_INVOKABLE bool isClipEnabled(const QString& name) const;

    /// Sample `clipName` at `time` and push it onto the scene so the
    /// viewport reflects the scrubbed pose immediately (used by the
    /// timeline slider while playback is paused). No-op on a missing
    /// clip. Enables the driving AnimationState if needed but leaves
    /// playback paused — the caller owns play/pause.
    Q_INVOKABLE void scrubClip(const QString& clipName, double time);

    // ── Undoable QML authoring surface ──────────────────────────────
    // These push QUndoCommands (NodeAnimCommands) so Inspector edits
    // are Ctrl+Z-able, matching every other authoring path. They are
    // distinct from the raw createClip/deleteClip/addKeyframe above,
    // which the commands themselves call and which the CLI/MCP use
    // directly (those surfaces don't share the GUI undo stack).

    /// Create a clip via CreateNodeAnimClipCommand (undoable).
    Q_INVOKABLE bool createClipUndoable(const QString& name, double length);

    /// Delete a clip via DeleteNodeAnimClipCommand (undoable, restores
    /// every track + keyframe on undo).
    Q_INVOKABLE bool deleteClipUndoable(const QString& name);

    /// Capture `nodeName`'s CURRENT world-relative transform (the live
    /// SceneNode's position/orientation/scale) as a keyframe on
    /// `clipName` at `time`, via SetNodeKeyframeCommand (undoable).
    /// This is the "key at playhead" authoring gesture — the user
    /// moves the node with the gizmo, scrubs the timeline, clicks Key.
    Q_INVOKABLE bool keyNodeCurrentTransform(const QString& clipName,
                                             const QString& nodeName,
                                             double time);

    /// Move an existing keyframe on (`clipName`,`nodeName`) from
    /// `oldTime` to `newTime` (undoable). Used by dope-sheet drag.
    Q_INVOKABLE bool moveNodeKeyframe(const QString& clipName,
                                      const QString& nodeName,
                                      double oldTime,
                                      double newTime);

    /// Delete the keyframe on (`clipName`,`nodeName`) nearest `time`
    /// (undoable). Used by dope-sheet right-click.
    Q_INVOKABLE bool deleteNodeKeyframe(const QString& clipName,
                                        const QString& nodeName,
                                        double time);

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

    /// Forget the {clipName, nodeName} → handle entry in
    /// `m_trackHandles`. Used by `SetNodeKeyframeCommand::undo`
    /// when it has to `destroyNodeTrack` the track it created in
    /// redo — without this, the stale handle would shadow a later
    /// `addKeyframe` for a different node and corrupt that node's
    /// animation (same class of bug as the original `qHash` issue
    /// PR #584 fixed, just in a different code path).
    void forgetTrackHandle(const QString& clipName, const QString& nodeName);

    /// Public relays so the NodeAnimCommands (which mutate tracks
    /// directly for move/delete rather than through addKeyframe) can
    /// notify the dope sheet + inspector to rebuild. Emitting a
    /// signal is otherwise only possible from within the class.
    void emitKeyframesChanged(const QString& clipName);
    void emitClipsChanged();

signals:
    /// The set of clips visible on the scene changed (create / delete).
    void clipsChanged();
    /// A clip's keyframes changed (add / overwrite / future remove).
    void keyframesChanged(const QString& clipName);
    /// The active (inspected) clip changed.
    void activeClipChanged();
    /// The edit-session clip changed (beginEdit / endEdit).
    void editingClipChanged();

private slots:
    /// Drop every node-animation track that targets a SceneNode being
    /// destroyed (Manager::sceneNodeDestroyed), and delete any clip left with
    /// no tracks. Without this the clip's tracks + AnimationState keep raw
    /// pointers to the freed node and the frame loop crashes on the next
    /// _applySceneAnimations (bug: "crashed when I tried to delete a node …
    /// while it had node animation checked"). Fired BEFORE destruction so the
    /// node name is still resolvable.
    void onSceneNodeDestroyed(Ogre::SceneNode* node);

private:
    explicit NodeAnimationManager(QObject* parent = nullptr);
    ~NodeAnimationManager() override;

    /// Per-clip {nodeName → track handle} map. Allocated lazily on
    /// the first `addKeyframe(clip, node, …)` call for a given pair
    /// and reused for every subsequent call. This is a collision-free
    /// replacement for an earlier `qHash & 0xFFFF` strategy that
    /// silently corrupted both nodes on a 16-bit hash collision
    /// (which is realistic at typical scene sizes — birthday paradox
    /// hits ~50% by ~300 names).
    unsigned short trackHandleForNode(const QString& clipName,
                                      const QString& nodeName);

    /// Look up a SceneNode by name on the current scene. Returns
    /// null when the node doesn't exist. Centralised so name-not-
    /// found behaviour is consistent across all setters.
    static Ogre::SceneNode* findSceneNode(const QString& nodeName);

    /// `clipName → { nodeName → handle }`. Mutated by `addKeyframe`
    /// (lazy allocation) and `deleteClip` (forgets the whole clip
    /// entry). Lives only on the manager — no Ogre dependency.
    QHash<QString, QHash<QString, unsigned short>> m_trackHandles;

    /// The clip currently being authored/inspected (editor UI state).
    QString m_activeClip;

    /// The clip in an open EDIT session (draft), or empty. See editingClip.
    QString m_editingClip;

    static NodeAnimationManager* s_instance;
};

#endif // NODEANIMATIONMANAGER_H
