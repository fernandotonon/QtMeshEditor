/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef MOTIONGRAPHMANAGER_H
#define MOTIONGRAPHMANAGER_H

// Scene side of the motion graph (#526). The state machine itself is
// AnimGraph.h (pure); this owns one graph per entity, plays it, and keeps it
// with the project.
//
// PLAYBACK drives the entity's OWN Ogre AnimationStates — enabled flag, time,
// and a per-bone blend mask carrying each clip's share — so everything that
// already reads animation states (constraints, the viewport, the dope sheet)
// keeps working on top of the graph. Two Ogre details make that correct:
//   * the skeleton runs in ANIMBLEND_CUMULATIVE while the graph plays. In
//     the default AVERAGE mode Ogre rescales every state by 1/Σweights when
//     the total exceeds 1, which halves two COMPLEMENTARY masked clips (wave
//     on the arms + run on the legs, both at weight 1);
//   * per-bone weights are pre-normalised so they sum to 1 on every bone,
//     which is exactly what cumulative blending expects.
// Everything (state flags, times, weights, masks, blend mode) is snapshotted
// on Play and restored on Stop. While playing, the main loop leaves the
// entity alone (drives()).
//
// AUTHORING is undoable: every edit swaps two graph documents. Parameter
// values changed while playing only affect the running copy.
//
// PERSISTENCE: `<file>.animgraph.json` beside every export (single-entity
// export keeps that entity's graph and records its name so a renamed
// re-import rebinds it). Preview / authoring only — export writes the clips,
// never an engine-native graph.

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>
#include <QtQml/qqmlregistration.h>

#include <OgreSkeleton.h>

#include "AnimGraph.h"

#include <functional>
#include <vector>

namespace Ogre { class Entity; }
class QQmlEngine;
class QJSEngine;

class MotionGraphManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString entity READ entity WRITE setEntity NOTIFY graphChanged)
    Q_PROPERTY(QVariantList states READ stateRows NOTIFY graphChanged)
    Q_PROPERTY(QVariantList transitions READ transitionRows NOTIFY graphChanged)
    Q_PROPERTY(QVariantList params READ paramRows NOTIFY paramsChanged)
    Q_PROPERTY(QString entry READ entry NOTIFY graphChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playbackChanged)
    Q_PROPERTY(QString currentState READ currentState NOTIFY playbackChanged)
    Q_PROPERTY(QString previousState READ previousState NOTIFY playbackChanged)
    Q_PROPERTY(double blendProgress READ blendProgress NOTIFY playbackChanged)
    Q_PROPERTY(QString lastTransition READ lastTransition NOTIFY playbackChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool lastOk READ lastOk NOTIFY statusChanged)
    Q_PROPERTY(QStringList opIds READ opIds CONSTANT)

public:
    static MotionGraphManager* instance();
    static MotionGraphManager* peek() { return m_pSingleton; }
    static MotionGraphManager* qmlInstance(QQmlEngine*, QJSEngine*);
    static void kill();

    // ---- C++ API ---------------------------------------------------------
    AnimGraph::Graph graph(const QString& entity) const { return m_graphs.value(entity); }
    bool hasGraph(const QString& entity) const { return m_graphs.contains(entity); }
    QStringList graphEntities() const { return m_graphs.keys(); }
    /// Replace an entity's graph (validated). One undo step when undoable.
    bool setGraph(const QString& entity, const AnimGraph::Graph& g, bool undoable = true, QString* error = nullptr,
                  const QString& label = QString());
    /// Raw swap used by undo/redo: an empty object removes the graph.
    void applyGraphJson(const QString& entity, const QJsonObject& json);
    void clear();

    bool play(const QString& entity, QString* error = nullptr);
    void stop();
    /// Advance the running graph (MainWindow frame loop; tests call it).
    void tick(double dt);
    bool drives(const Ogre::Entity* e) const;
    /// Set a parameter: the running copy while playing, else the default
    /// stored in the graph (undoable).
    bool setParamValue(const QString& name, double value, QString* error = nullptr);
    std::vector<AnimGraph::Contribution> currentContributions() const { return AnimGraph::contributions(m_rt); }

    static QString sidecarPath(const QString& assetPath);
    /// Write the graphs of `entities` (all when empty). Removes a stale file
    /// when there is nothing to write.
    bool writeSidecar(const QString& assetPath, const QStringList& entities = {}) const;
    /// Load; `rename` maps saved entity names to scene names. With a single
    /// graph and `fallbackEntity` set, the graph binds to it whatever its
    /// saved name. Returns graphs loaded, -1 on error.
    int loadSidecar(const QString& assetPath, const QHash<QString, QString>& rename = {},
                    const QString& fallbackEntity = QString(), QString* error = nullptr);

    // ---- QML -------------------------------------------------------------
    QString entity() const { return m_entity; }
    void setEntity(const QString& e);
    QVariantList stateRows() const;
    QVariantList transitionRows() const;
    QVariantList paramRows() const;
    QString entry() const { return m_graphs.value(m_entity).entry; }
    bool playing() const { return m_playing; }
    QString currentState() const { return m_playing ? m_rt.current.primary.state : QString(); }
    QString previousState() const { return m_playing && m_rt.blending ? m_rt.previous.primary.state : QString(); }
    double blendProgress() const;
    QString lastTransition() const { return m_playing ? m_rt.lastTransition : QString(); }
    QString status() const { return m_status; }
    bool lastOk() const { return m_lastOk; }
    QStringList opIds() const { return AnimGraph::opIds(); }

    Q_INVOKABLE QStringList skinnedEntities() const;
    Q_INVOKABLE QString entityFromSelection() const;
    Q_INVOKABLE QStringList clipsOf(const QString& entity) const;
    Q_INVOKABLE QStringList bonesOf(const QString& entity) const;
    Q_INVOKABLE QVariantMap transitionDetails(const QString& id) const;

    Q_INVOKABLE QString addState(const QString& clip, double x, double y);
    Q_INVOKABLE bool removeState(const QString& name);
    Q_INVOKABLE bool renameState(const QString& name, const QString& newName);
    Q_INVOKABLE bool setStateField(const QString& name, const QString& key, const QVariant& value);
    /// Panel drag: `commit` false while dragging (no undo entry), true on release.
    Q_INVOKABLE void moveState(const QString& name, double x, double y, bool commit);
    Q_INVOKABLE bool setEntry(const QString& name);

    Q_INVOKABLE QString addTransition(const QString& from, const QString& to);
    Q_INVOKABLE bool removeTransition(const QString& id);
    Q_INVOKABLE bool setTransitionField(const QString& id, const QString& key, const QVariant& value);
    Q_INVOKABLE bool moveTransition(const QString& id, int direction);
    Q_INVOKABLE bool addCondition(const QString& id, const QString& param, const QString& op, double value);
    Q_INVOKABLE bool setCondition(const QString& id, int index, const QString& param, const QString& op, double value);
    Q_INVOKABLE bool removeCondition(const QString& id, int index);
    /// Mask = `bone` (and every descendant when `subtree`). Empty bone clears.
    Q_INVOKABLE bool setTransitionMaskFromBone(const QString& id, const QString& bone, bool subtree);

    Q_INVOKABLE bool addParam(const QString& name, const QString& type);
    Q_INVOKABLE bool removeParam(const QString& name);
    Q_INVOKABLE bool setParamFromUi(const QString& name, double value);
    Q_INVOKABLE bool fireTrigger(const QString& name);
    /// Quick start: idle / walk / run from the entity's clips (picked by name,
    /// else the first three) driven by a `speed` float.
    Q_INVOKABLE bool buildLocomotionTemplate();

    Q_INVOKABLE bool playFromUi();
    Q_INVOKABLE void stopFromUi();

signals:
    void graphChanged();
    void paramsChanged();
    void playbackChanged();
    void statusChanged();

private:
    MotionGraphManager();
    ~MotionGraphManager() override;

    void setStatus(const QString& text, bool ok);
    bool edit(const QString& label, const std::function<bool(AnimGraph::Graph*, QString*)>& fn);
    void ensureSceneHook();
    void discardForSceneClear();
    void applyToOgre();
    void restoreOgre();

    QString m_status;
    bool m_lastOk = true;
    QString m_entity;
    QHash<QString, AnimGraph::Graph> m_graphs;

    // Playback
    bool m_playing = false;
    QString m_playEntity;
    AnimGraph::Graph m_playGraph;            ///< snapshot taken at Play
    std::vector<AnimGraph::Param> m_runParams;
    AnimGraph::Runtime m_rt;
    struct StateSave { std::string name; bool enabled; float time; float weight; bool loop; bool hadMask; };
    std::vector<StateSave> m_stateSaves;
    Ogre::SkeletonAnimationBlendMode m_savedBlendMode = Ogre::ANIMBLEND_AVERAGE;
    bool m_savedSkip = false;
    double m_dragStartX = 0, m_dragStartY = 0;
    QString m_dragState;
    QJsonObject m_dragBefore;

    QPointer<QObject> m_hookedManager;
    static MotionGraphManager* m_pSingleton;
};

#endif // MOTIONGRAPHMANAGER_H
