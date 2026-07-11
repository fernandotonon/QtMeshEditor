/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef MORPHANIMATIONMANAGER_H
#define MORPHANIMATIONMANAGER_H

#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

namespace Ogre { class Entity; }

/**
 * @brief QML_SINGLETON owning per-entity morph-target weight state.
 *
 * Wraps the Ogre::Pose + per-pose VAT_POSE animation pattern the
 * importer (MeshProcessor) sets up. Each named morph target on an
 * entity is backed by an `Ogre::Animation` of the same name; weight =
 * the matching `Ogre::AnimationState::getWeight()`.
 *
 * Slice A1 surface — minimal but complete enough that the rest of
 * the slice (Inspector sliders, dope sheet, authoring, export) can
 * build on top:
 *   - `morphTargetsFor(entity)` — list of target names.
 *   - `weight(entity, name)` / `setWeight(entity, name, w)` —
 *     read / write a single weight.
 *   - Signals on change so QML can bind.
 *
 * Authoring (create new targets from edit-mode deltas), export, and
 * dope-sheet wiring come in follow-up sub-slices.
 */
class MorphAnimationManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    static MorphAnimationManager* instance();
    static MorphAnimationManager* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    /// Enumerate morph-target names on an entity, in mesh-pose-list order.
    /// Returns empty when the entity has no morph targets or is null.
    QStringList morphTargetsFor(Ogre::Entity* entity) const;

    /// Read the current weight for `name` on `entity`. Returns 0.0
    /// when the entity has no such target or weight tracking isn't
    /// available (e.g. the per-pose AnimationState wasn't found).
    float weight(Ogre::Entity* entity, const QString& name) const;

    /// Set a single morph-target weight. Enables the matching
    /// AnimationState so the weight actually applies, and clamps
    /// the input to [0..1]. Emits `morphWeightChanged` on real
    /// changes. Returns false when the entity has no such target.
    bool setWeight(Ogre::Entity* entity, const QString& name, float w);

    /// QML-friendly variants that resolve the entity from
    /// SelectionSet's first entity. Used by the Inspector subgroup.
    Q_INVOKABLE QStringList morphTargetsForSelection() const;
    Q_INVOKABLE double weightForSelection(const QString& name) const;
    Q_INVOKABLE bool setWeightForSelection(const QString& name, double w);

    /// Weight keyframing over time (Slice 2, #519). Morph targets are the shared
    /// SHAPES (smile, browRaise, jawOpen…); a "morph clip" is a named mesh
    /// Animation that keyframes those shapes' weights over time (smile / angry /
    /// surprised). A clip holds one VAT_POSE track per target's pose; each
    /// keyframe references the pose at the influence (= weight) at that time.
    /// Each clip exports as a separate glTF morph-weights animation. Multiple
    /// clips share the same targets. Distinct from the static per-target
    /// Animation (named exactly the target name) that only carries the shape.

    /// The currently-active morph clip that keyframe edits write to (default
    /// `kWeightClipName`). Every keyframe method below operates on THIS clip.
    Q_PROPERTY(QString activeMorphClip READ activeMorphClip WRITE setActiveMorphClip
               NOTIFY morphClipsChanged)
    QString activeMorphClip() const { return m_activeMorphClip; }
    void setActiveMorphClip(const QString& name);

    /// Named morph (weight) clips on the selected entity — mesh VAT_POSE
    /// animations that are NOT per-target shape clips. For the clip dropdown.
    Q_INVOKABLE QStringList morphClips() const;

    /// Create an empty morph clip named `name` and make it active. Returns false
    /// if the name is empty / already exists / no selection.
    Q_INVOKABLE bool createMorphClip(const QString& name);

    /// Delete a morph clip (its Animation + AnimationState). Returns false on
    /// no-op. Does NOT touch the shared targets/poses.
    Q_INVOKABLE bool deleteMorphClip(const QString& name);

    /// Rename a morph clip. Rejects the reserved default name collisions.
    Q_INVOKABLE bool renameMorphClip(const QString& oldName, const QString& newName);

    /// Record `weight` for target `name` at `time` on the ACTIVE morph clip
    /// (creates the clip/track/keyframe as needed, updates in place otherwise).
    /// Extends the clip length to cover `time`. Returns false on no-op.
    Q_INVOKABLE bool setMorphWeightKeyframe(const QString& name, double time, double weight);

    /// Remove the active clip's weight keyframe for `name` at (approx) `time`.
    Q_INVOKABLE bool clearMorphWeightKeyframe(const QString& name, double time);

    /// Move a weight keyframe from `oldTime` to `newTime` on the active clip,
    /// preserving its weight (dope-sheet drag). False on no-op/not-found/collision.
    Q_INVOKABLE bool moveMorphWeightKeyframe(const QString& name,
                                             double oldTime, double newTime);

    /// The weight stored at (approx) `time` for `name` on the active clip, or -1.
    Q_INVOKABLE double morphWeightAt(const QString& name, double time) const;

    /// Active clip's keyframe times (seconds) for `name`, ascending. Empty if
    /// none. For the dope sheet.
    Q_INVOKABLE QVariantList morphWeightKeyframeTimes(const QString& name) const;

    /// Default morph-clip name (used when no clip is named explicitly + the
    /// first clip created). Also the glTF export fallback name.
    static const char* kWeightClipName;

    /// Make the ACTIVE morph clip the playable animation on the selected entity:
    /// select it in the Animation Control panel + enable its AnimationState so
    /// the timeline scrubs/plays the keyed weights. No-op if it doesn't exist.
    Q_INVOKABLE bool activateWeightClip();

    /// Authoring (slice A3). All three push a QUndoCommand on the
    /// shared UndoManager stack so Ctrl+Z reverses the change. All
    /// return false on no-op (entity missing, name collision, etc.).

    /// Create a new morph target whose vertex positions match the
    /// current edit state. Snapshots `EditableMesh` (or whatever the
    /// current edit state of the selected entity is) against the
    /// mesh's bind positions and stores the non-zero deltas as a new
    /// Ogre::Pose + matching VAT_POSE Animation. Falls back to a
    /// no-op if the user isn't in edit mode for the entity, or no
    /// vertex actually moved. `name` must be unique on the mesh.
    Q_INVOKABLE bool addMorphTargetFromCurrentEdit(const QString& name);

    /// Rename a morph target. Internally destroys + recreates the
    /// same-named Pose + Animation under the new name (Ogre 14.5
    /// doesn't expose `setName` on Pose).
    Q_INVOKABLE bool renameMorphTarget(const QString& oldName,
                                       const QString& newName);

    /// Delete a morph target. Drops the matching Pose(s) and
    /// Animation, and resets any AnimationState that referenced it.
    Q_INVOKABLE bool deleteMorphTarget(const QString& name);

    /// Reorder morph targets: move `name` by `delta` positions in the display
    /// order (-1 = up, +1 = down; larger jumps clamp to the ends). Pushes an
    /// undoable ReorderMorphTargetsCommand. Returns false if the move is a
    /// no-op (already at the edge / name not found / no selection).
    Q_INVOKABLE bool moveMorphTarget(const QString& name, int delta);

    /// Reorder by absolute index: move `name` to position `toIndex` in the
    /// display order (for drag-and-drop). Returns false on a no-op.
    Q_INVOKABLE bool moveMorphTargetToIndex(const QString& name, int toIndex);

signals:
    /// Emitted when a morph weight on any entity is changed via
    /// `setWeight`. QML uses this to re-fetch values.
    void morphWeightChanged(Ogre::Entity* entity, const QString& name, double weight);
    /// Emitted when the morph-target list visible to the Inspector
    /// could have changed (selection moved, scene reloaded, etc.).
    void morphTargetsChanged();
    /// Emitted when the morph-clip list or the active clip changes.
    void morphClipsChanged();

private:
    explicit MorphAnimationManager(QObject* parent = nullptr);
    ~MorphAnimationManager() override;

    // The clip that keyframe edits target. Defaults to kWeightClipName so
    // existing single-clip behaviour is preserved until the user makes more.
    QString m_activeMorphClip;

    static MorphAnimationManager* s_instance;
};

#endif // MORPHANIMATIONMANAGER_H
