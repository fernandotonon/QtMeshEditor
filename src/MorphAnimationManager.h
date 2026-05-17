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

signals:
    /// Emitted when a morph weight on any entity is changed via
    /// `setWeight`. QML uses this to re-fetch values.
    void morphWeightChanged(Ogre::Entity* entity, const QString& name, double weight);
    /// Emitted when the morph-target list visible to the Inspector
    /// could have changed (selection moved, scene reloaded, etc.).
    void morphTargetsChanged();

private:
    explicit MorphAnimationManager(QObject* parent = nullptr);
    ~MorphAnimationManager() override;

    static MorphAnimationManager* s_instance;
};

#endif // MORPHANIMATIONMANAGER_H
