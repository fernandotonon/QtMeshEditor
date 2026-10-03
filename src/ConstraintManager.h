/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef CONSTRAINTMANAGER_H
#define CONSTRAINTMANAGER_H

// Scene side of animation constraints (#525). The maths is AnimConstraints.h.
//
// EVALUATION. Constraints run every frame AFTER the animation is sampled and
// BEFORE the skin is computed, from an Ogre SceneManager listener
// (`preUpdateSceneGraph` — scene/node animations are already applied there,
// and Entity::_updateAnimation, which uploads bone matrices, runs later in
// the same render):
//   * node owners: the node's local TRS is the input. A node driven by a node
//     clip gets a fresh animated value every frame; a static node's value is
//     re-based (if it still equals what we wrote last frame, the stored base
//     is used, otherwise the user moved it and that is the new base), so an
//     influence < 1 never creeps and removing the constraint restores it;
//   * bone owners: the entity's skeleton is posed by applying its animation
//     states HERE (setAnimationState), the constraints are written on top,
//     and the entity is told to skip its own state update this frame
//     (`setSkipAnimationStateUpdate`) so Ogre does not re-sample over them.
//     Bones stay non-manual, so muting/removing a constraint simply lets the
//     next frame's sampling take over.
// Order: node owners first, then bone owners per skeleton by depth, each
// owner's stack from the BOTTOM up — the top-most constraint is applied last
// and wins where they conflict.
//
// BAKE flattens constrained motion into keyframes (bones into the chosen
// skeletal clip, including the ancestors an IK moves; nodes into a node
// clip), then mutes the baked constraints. glTF/FBX cannot store constraints,
// so export offers to bake; otherwise they persist in a
// `<file>.constraints.json` sidecar.
//
// UNDO: every edit swaps two documents (`qtmesh-anim-constraints-v1`); a
// bake also restores the tracks it rewrote.

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

#include <OgreSceneManager.h>

#include "AnimConstraints.h"

#include <vector>

class QQmlEngine;
class QJSEngine;

class ConstraintManager : public QObject, public Ogre::SceneManager::Listener
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QVariantList constraints READ constraintRows NOTIFY constraintsChanged)
    Q_PROPERTY(int count READ count NOTIFY constraintsChanged)
    Q_PROPERTY(int activeCount READ activeCount NOTIFY constraintsChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool lastOk READ lastOk NOTIFY statusChanged)
    Q_PROPERTY(QStringList typeIds READ typeIds CONSTANT)

public:
    static ConstraintManager* instance();
    static ConstraintManager* peek() { return m_pSingleton; }
    static ConstraintManager* qmlInstance(QQmlEngine*, QJSEngine*);
    static void kill();

    struct Result {
        bool ok = false;
        QString id;
        QString error;
    };

    /// Add `c` at the TOP of its owner's stack (so it wins). Validates owner
    /// and target; a parent-of without an offset captures the current one.
    Result add(AnimCon::Constraint c, bool undoable = true);
    Result remove(const QString& id, bool undoable = true);
    Result setParams(const QString& id, const QList<QPair<QString, QString>>& params, const bool* enabled,
                     bool undoable = true);
    /// Move within the owner's stack: -1 = up (toward the top / applied
    /// later), +1 = down.
    Result move(const QString& id, int direction, bool undoable = true);

    struct BakeOptions {
        QStringList ids;      ///< empty = every enabled constraint
        QString clip;         ///< skeletal clip for bone owners ("" = the entity's playing / first clip)
        QString nodeClip;     ///< node clip for node owners ("" = the clip already animating it, else "Constraints")
        double nodeLength = 0; ///< length of a node clip the bake creates (0 = longest node clip, else 1 s)
        int fps = 30;
    };
    /// Flatten the constrained motion into keyframes and mute the baked
    /// constraints. One undo step.
    Result bake(const BakeOptions& opts, bool undoable = true);

    const std::vector<AnimCon::Constraint>& constraints() const { return m_cons; }
    const AnimCon::Constraint* find(const QString& id) const;
    int count() const { return int(m_cons.size()); }
    int activeCount() const;

    QJsonObject document() const { return AnimCon::toDocument(m_cons); }
    bool applyDocument(const QJsonObject& doc, QString* error = nullptr);
    void clear();

    /// Sidecar beside an export (see AnimGeneratorManager for the convention).
    static QString sidecarPath(const QString& assetPath);
    bool writeSidecar(const QString& assetPath, const QStringList& objects = {}, const QJsonObject& meta = {}) const;
    static QJsonObject sidecarMeta(const QString& assetPath);
    int loadSidecar(const QString& assetPath, const QHash<QString, QString>& rename = {}, QString* error = nullptr);

    /// Evaluate every active constraint now (the listener calls this). Public
    /// for tests and for the bake.
    void evaluate();

    // Snapshot of a track a bake rewrites (restored by undo). Opaque JSON.
    void restoreTracks(const QJsonArray& snapshots);

    // ---- QML -------------------------------------------------------------
    QVariantList constraintRows() const;
    int count_() const { return count(); }
    QString status() const { return m_status; }
    bool lastOk() const { return m_lastOk; }
    QStringList typeIds() const { return AnimCon::typeIds(); }

    Q_INVOKABLE QString typeLabel(const QString& typeId) const;
    Q_INVOKABLE QVariantList rowsFor(const QString& owner) const;
    Q_INVOKABLE QVariantMap details(const QString& id) const;
    /// The selection as an owner ref ("bone:E/B" when a bone of the selected
    /// entity is selected in the animation panel, else "node:N"), or "".
    Q_INVOKABLE QString ownerFromSelection() const;
    Q_INVOKABLE QStringList nodeNames() const;
    Q_INVOKABLE QStringList skinnedEntities() const;
    Q_INVOKABLE QStringList bonesOf(const QString& entity) const;
    Q_INVOKABLE QStringList clipsOf(const QString& entity) const;
    Q_INVOKABLE bool addFromUi(const QString& typeId, const QString& owner, const QString& target,
                               const QString& pole);
    Q_INVOKABLE bool removeFromUi(const QString& id);
    Q_INVOKABLE bool setEnabledFromUi(const QString& id, bool enabled);
    Q_INVOKABLE bool setParamFromUi(const QString& id, const QString& key, const QString& value);
    Q_INVOKABLE bool moveFromUi(const QString& id, int direction);
    Q_INVOKABLE bool bakeFromUi(const QString& owner, const QString& clip, int fps);

    // Ogre::SceneManager::Listener
    void preUpdateSceneGraph(Ogre::SceneManager* source, Ogre::Camera* camera) override;

signals:
    void constraintsChanged();
    void statusChanged();

private:
    ConstraintManager();
    ~ConstraintManager() override;

    void setStatus(const QString& text, bool ok);
    Result commit(const std::vector<AnimCon::Constraint>& next, const QString& label, const QString& id,
                  bool undoable);
    bool validate(AnimCon::Constraint* c, QString* error) const;
    void ensureListener();
    void detachListener();
    void ensureSceneHook();
    void discardForSceneClear();
    /// Release entities / restore nodes no longer constrained.
    void syncOwners();

    QString m_status;
    bool m_lastOk = true;
    std::vector<AnimCon::Constraint> m_cons;
    bool m_evaluating = false;

    struct NodeState {
        AnimCon::Transform base;
        AnimCon::Transform lastWritten;
        bool written = false;
    };
    QHash<QString, NodeState> m_nodeStates;      ///< by node name
    QSet<QString> m_skippingEntities;             ///< entities we set skip-update on
    QSet<QString> m_touchedBones;                 ///< "entity/bone" written in the last evaluate()

    Ogre::SceneManager* m_listened = nullptr;
    QPointer<QObject> m_hookedManager;

    static ConstraintManager* m_pSingleton;
};

#endif // CONSTRAINTMANAGER_H
