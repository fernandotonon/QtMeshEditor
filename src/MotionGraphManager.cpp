/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "MotionGraphManager.h"

#include "AnimationControlController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/MotionGraphCommands.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlEngine>
#include <QSaveFile>
#include <QSet>

#include <OgreAnimationState.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSkeletonInstance.h>

#include <algorithm>
#include <cmath>

using namespace AnimGraph;

namespace {

Ogre::Entity* entityByName(const QString& n)
{
    Manager* m = Manager::getSingletonPtr();
    Ogre::SceneManager* s = m ? m->getSceneMgr() : nullptr;
    if (!s || n.isEmpty()) return nullptr;
    const std::string sn = n.toStdString();
    return s->hasEntity(sn) ? s->getEntity(sn) : nullptr;
}

double clipLength(const QString& clip, const void* ctx)
{
    auto* e = static_cast<const Ogre::Entity*>(ctx);
    if (!e || !e->hasSkeleton()) return 0.0;
    const std::string c = clip.toStdString();
    auto* skel = const_cast<Ogre::Entity*>(e)->getSkeleton();
    return skel->hasAnimation(c) ? double(skel->getAnimation(c)->getLength()) : 0.0;
}

void crumb(const char* op, const QString& msg)
{
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.graph.%1").arg(QLatin1String(op)), msg);
}

QString docSchema() { return QStringLiteral("qtmesh-anim-graphs-v1"); }

void collectSubtree(Ogre::Node* n, QStringList* out)
{
    out->append(QString::fromStdString(n->getName()));
    for (Ogre::Node* c : n->getChildren()) collectSubtree(c, out);
}

} // namespace

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

MotionGraphManager* MotionGraphManager::m_pSingleton = nullptr;

MotionGraphManager* MotionGraphManager::instance()
{
    if (!m_pSingleton) m_pSingleton = new MotionGraphManager();
    return m_pSingleton;
}

MotionGraphManager* MotionGraphManager::qmlInstance(QQmlEngine*, QJSEngine*)
{
    MotionGraphManager* m = instance();
    QQmlEngine::setObjectOwnership(m, QQmlEngine::CppOwnership);
    return m;
}

void MotionGraphManager::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

MotionGraphManager::MotionGraphManager() { ensureSceneHook(); }

MotionGraphManager::~MotionGraphManager() = default;

void MotionGraphManager::setStatus(const QString& text, bool ok)
{
    m_status = text;
    m_lastOk = ok;
    emit statusChanged();
}

void MotionGraphManager::ensureSceneHook()
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr || m_hookedManager == mgr) return;
    m_hookedManager = mgr;
    connect(mgr, &Manager::sceneClearing, this, &MotionGraphManager::discardForSceneClear);
}

void MotionGraphManager::discardForSceneClear()
{
    // The entities are about to be destroyed: forget playback without
    // touching them, and drop their graphs.
    const bool had = !m_graphs.isEmpty() || m_playing;
    m_playing = false;
    m_stateSaves.clear();
    m_graphs.clear();
    m_entity.clear();
    if (had) {
        emit graphChanged();
        emit paramsChanged();
        emit playbackChanged();
        crumb("clear", QStringLiteral("scene replaced"));
    }
}

void MotionGraphManager::clear()
{
    stop();
    m_graphs.clear();
    m_entity.clear();
    emit graphChanged();
    emit paramsChanged();
}

// ---------------------------------------------------------------------------
// Authoring
// ---------------------------------------------------------------------------

bool MotionGraphManager::setGraph(const QString& entity, const Graph& g, bool undoable, QString* error,
                                  const QString& label)
{
    ensureSceneHook();
    QString err;
    if (!validate(g, &err)) {
        if (error) *error = err;
        setStatus(err, false);
        return false;
    }
    const QJsonObject before = m_graphs.contains(entity) ? toJson(m_graphs.value(entity)) : QJsonObject{};
    const QJsonObject after = toJson(g);
    applyGraphJson(entity, after);
    if (undoable && before != after)
        if (UndoManager* um = UndoManager::getSingleton())
            um->push(new MotionGraphDocCommand(label.isEmpty() ? QStringLiteral("Edit motion graph") : label, entity,
                                               before, after));
    return true;
}

void MotionGraphManager::applyGraphJson(const QString& entity, const QJsonObject& json)
{
    if (json.isEmpty()) {
        m_graphs.remove(entity);
    } else {
        Graph g;
        if (!fromJson(json, &g)) return;
        m_graphs.insert(entity, g);
    }
    // Keep a running preview in step with edits.
    if (m_playing && entity == m_playEntity) {
        if (!m_graphs.contains(entity) || !m_graphs.value(entity).state(m_rt.current.primary.state)) {
            stop();
        } else {
            m_playGraph = m_graphs.value(entity);
            std::vector<Param> merged = m_playGraph.params;
            for (auto& p : merged)
                for (const auto& r : m_runParams) if (r.name == p.name && r.type == p.type) p.value = r.value;
            m_runParams = merged;
        }
    }
    emit graphChanged();
    emit paramsChanged();
}

bool MotionGraphManager::edit(const QString& label, const std::function<bool(Graph*, QString*)>& fn)
{
    if (m_entity.isEmpty()) {
        setStatus(QStringLiteral("pick an entity first"), false);
        return false;
    }
    Graph g = m_graphs.value(m_entity);
    QString err;
    if (!fn(&g, &err)) {
        setStatus(err, false);
        return false;
    }
    if (!setGraph(m_entity, g, true, &err, label)) return false;
    setStatus(label, true);
    crumb("edit", label);
    return true;
}

void MotionGraphManager::setEntity(const QString& e)
{
    if (e == m_entity) return;
    m_entity = e;
    emit graphChanged();
    emit paramsChanged();
}

QString MotionGraphManager::addState(const QString& clip, double x, double y)
{
    QString name;
    edit(QStringLiteral("Add state"), [&](Graph* g, QString* err) {
        if (!clipsOf(m_entity).contains(clip)) { *err = QStringLiteral("'%1' has no clip '%2'").arg(m_entity, clip); return false; }
        State s;
        s.name = uniqueStateName(*g, clip);
        s.clip = clip;
        s.x = x;
        s.y = y;
        if (g->states.empty()) g->entry = s.name;
        g->states.push_back(s);
        name = s.name;
        return true;
    });
    if (!name.isEmpty()) crumb("add_state", name);
    return name;
}

bool MotionGraphManager::removeState(const QString& name)
{
    return edit(QStringLiteral("Remove state"), [&](Graph* g, QString* err) {
        auto it = std::find_if(g->states.begin(), g->states.end(), [&](const State& s) { return s.name == name; });
        if (it == g->states.end()) { *err = QStringLiteral("no state '%1'").arg(name); return false; }
        g->states.erase(it);
        g->transitions.erase(std::remove_if(g->transitions.begin(), g->transitions.end(),
                                            [&](const Transition& t) { return t.from == name || t.to == name; }),
                             g->transitions.end());
        if (g->entry == name) g->entry = g->states.empty() ? QString() : g->states.front().name;
        return true;
    });
}

bool MotionGraphManager::renameState(const QString& name, const QString& newName)
{
    return edit(QStringLiteral("Rename state"), [&](Graph* g, QString* err) {
        const QString n = newName.trimmed();
        if (n.isEmpty() || n == QLatin1String("*")) { *err = QStringLiteral("invalid state name"); return false; }
        if (n == name) return true;
        if (g->state(n)) { *err = QStringLiteral("a state named '%1' already exists").arg(n); return false; }
        bool found = false;
        for (auto& s : g->states) if (s.name == name) { s.name = n; found = true; }
        if (!found) { *err = QStringLiteral("no state '%1'").arg(name); return false; }
        for (auto& t : g->transitions) {
            if (t.from == name) t.from = n;
            if (t.to == name) t.to = n;
        }
        if (g->entry == name) g->entry = n;
        return true;
    });
}

bool MotionGraphManager::setStateField(const QString& name, const QString& key, const QVariant& value)
{
    return edit(QStringLiteral("Edit state"), [&](Graph* g, QString* err) {
        State* s = nullptr;
        for (auto& x : g->states) if (x.name == name) s = &x;
        if (!s) { *err = QStringLiteral("no state '%1'").arg(name); return false; }
        if (key == QLatin1String("clip")) {
            if (!clipsOf(m_entity).contains(value.toString())) { *err = QStringLiteral("no clip '%1'").arg(value.toString()); return false; }
            s->clip = value.toString();
        } else if (key == QLatin1String("loop")) {
            s->loop = value.toBool();
        } else if (key == QLatin1String("speed")) {
            bool ok = false;
            const double v = value.toDouble(&ok);
            if (!ok || !std::isfinite(v)) { *err = QStringLiteral("speed must be a number"); return false; }
            s->speed = v;
        } else {
            *err = QStringLiteral("unknown state field '%1'").arg(key);
            return false;
        }
        return true;
    });
}

void MotionGraphManager::moveState(const QString& name, double x, double y, bool commit)
{
    if (m_entity.isEmpty() || !m_graphs.contains(m_entity)) return;
    if (m_dragState != name) {
        m_dragState = name;
        m_dragBefore = toJson(m_graphs.value(m_entity));
    }
    Graph& g = m_graphs[m_entity];
    for (auto& s : g.states)
        if (s.name == name) { s.x = x; s.y = y; }
    emit graphChanged();
    if (commit) {
        const QJsonObject after = toJson(g);
        if (after != m_dragBefore)
            if (UndoManager* um = UndoManager::getSingleton())
                um->push(new MotionGraphDocCommand(QStringLiteral("Move state"), m_entity, m_dragBefore, after));
        m_dragState.clear();
    }
}

bool MotionGraphManager::setEntry(const QString& name)
{
    return edit(QStringLiteral("Set entry state"), [&](Graph* g, QString* err) {
        if (!g->state(name)) { *err = QStringLiteral("no state '%1'").arg(name); return false; }
        g->entry = name;
        return true;
    });
}

QString MotionGraphManager::addTransition(const QString& from, const QString& to)
{
    QString id;
    edit(QStringLiteral("Add transition"), [&](Graph* g, QString* err) {
        if (from != QLatin1String("*") && !g->state(from)) { *err = QStringLiteral("no state '%1'").arg(from); return false; }
        if (!g->state(to)) { *err = QStringLiteral("no state '%1'").arg(to); return false; }
        if (from == to) { *err = QStringLiteral("a transition needs two different states"); return false; }
        Transition t;
        t.id = uniqueTransitionId(*g);
        t.from = from;
        t.to = to;
        g->transitions.push_back(t);
        id = t.id;
        return true;
    });
    if (!id.isEmpty()) crumb("add_transition", id);
    return id;
}

bool MotionGraphManager::removeTransition(const QString& id)
{
    return edit(QStringLiteral("Remove transition"), [&](Graph* g, QString* err) {
        auto it = std::find_if(g->transitions.begin(), g->transitions.end(), [&](const Transition& t) { return t.id == id; });
        if (it == g->transitions.end()) { *err = QStringLiteral("no transition '%1'").arg(id); return false; }
        g->transitions.erase(it);
        return true;
    });
}

bool MotionGraphManager::setTransitionField(const QString& id, const QString& key, const QVariant& value)
{
    return edit(QStringLiteral("Edit transition"), [&](Graph* g, QString* err) {
        Transition* t = nullptr;
        for (auto& x : g->transitions) if (x.id == id) t = &x;
        if (!t) { *err = QStringLiteral("no transition '%1'").arg(id); return false; }
        bool ok = true;
        if (key == QLatin1String("duration")) {
            const double v = value.toDouble(&ok);
            if (!ok || !std::isfinite(v) || v < 0) { *err = QStringLiteral("duration must be ≥ 0 seconds"); return false; }
            t->duration = v;
        } else if (key == QLatin1String("exitTime") || key == QLatin1String("exit_time")) {
            const QString s = value.toString().trimmed();
            if (s.isEmpty() || s == QLatin1String("none")) { t->exitTime = -1.0; return true; }
            const double v = value.toDouble(&ok);
            if (!ok || !std::isfinite(v)) { *err = QStringLiteral("exit time must be 0..1 (or empty for none)"); return false; }
            t->exitTime = std::min(v, 1.0);
        } else if (key == QLatin1String("curve")) {
            if (!curveFromId(value.toString(), &t->curve)) { *err = QStringLiteral("curve must be linear|ease|step"); return false; }
        } else if (key == QLatin1String("from")) {
            t->from = value.toString();
        } else if (key == QLatin1String("to")) {
            t->to = value.toString();
        } else if (key == QLatin1String("mask")) {
            t->mask = value.toStringList();
        } else {
            *err = QStringLiteral("unknown transition field '%1'").arg(key);
            return false;
        }
        return true;
    });
}

bool MotionGraphManager::moveTransition(const QString& id, int direction)
{
    return edit(QStringLiteral("Reorder transitions"), [&](Graph* g, QString* err) {
        int i = -1;
        for (int k = 0; k < int(g->transitions.size()); ++k) if (g->transitions[size_t(k)].id == id) i = k;
        const int j = i + (direction < 0 ? -1 : 1);
        if (i < 0 || j < 0 || j >= int(g->transitions.size())) { *err = QStringLiteral("cannot move further"); return false; }
        std::swap(g->transitions[size_t(i)], g->transitions[size_t(j)]);
        return true;
    });
}

bool MotionGraphManager::addCondition(const QString& id, const QString& param, const QString& op, double value)
{
    return edit(QStringLiteral("Add condition"), [&](Graph* g, QString* err) {
        Transition* t = nullptr;
        for (auto& x : g->transitions) if (x.id == id) t = &x;
        if (!t) { *err = QStringLiteral("no transition '%1'").arg(id); return false; }
        Condition c;
        c.param = param;
        if (!opFromId(op, &c.op)) { *err = QStringLiteral("unknown operator '%1'").arg(op); return false; }
        c.value = value;
        t->conditions.push_back(c);
        return true;
    });
}

bool MotionGraphManager::setCondition(const QString& id, int index, const QString& param, const QString& op, double value)
{
    return edit(QStringLiteral("Edit condition"), [&](Graph* g, QString* err) {
        Transition* t = nullptr;
        for (auto& x : g->transitions) if (x.id == id) t = &x;
        if (!t || index < 0 || index >= int(t->conditions.size())) { *err = QStringLiteral("no such condition"); return false; }
        Condition& c = t->conditions[size_t(index)];
        c.param = param;
        if (!opFromId(op, &c.op)) { *err = QStringLiteral("unknown operator '%1'").arg(op); return false; }
        c.value = value;
        return true;
    });
}

bool MotionGraphManager::removeCondition(const QString& id, int index)
{
    return edit(QStringLiteral("Remove condition"), [&](Graph* g, QString* err) {
        Transition* t = nullptr;
        for (auto& x : g->transitions) if (x.id == id) t = &x;
        if (!t || index < 0 || index >= int(t->conditions.size())) { *err = QStringLiteral("no such condition"); return false; }
        t->conditions.erase(t->conditions.begin() + index);
        return true;
    });
}

bool MotionGraphManager::setTransitionMaskFromBone(const QString& id, const QString& bone, bool subtree)
{
    QStringList mask;
    if (!bone.isEmpty()) {
        Ogre::Entity* e = entityByName(m_entity);
        if (!e || !e->hasSkeleton() || !e->getSkeleton()->hasBone(bone.toStdString())) {
            setStatus(QStringLiteral("no bone '%1' on '%2'").arg(bone, m_entity), false);
            return false;
        }
        Ogre::Bone* b = e->getSkeleton()->getBone(bone.toStdString());
        if (subtree) collectSubtree(b, &mask);
        else mask << bone;
    }
    const bool ok = setTransitionField(id, QStringLiteral("mask"), mask);
    if (ok) crumb("mask", QStringLiteral("%1: %2 bones").arg(id).arg(mask.size()));
    return ok;
}

bool MotionGraphManager::addParam(const QString& name, const QString& type)
{
    const bool ok = edit(QStringLiteral("Add parameter"), [&](Graph* g, QString* err) {
        Param p;
        p.name = name.trimmed();
        if (p.name.isEmpty()) { *err = QStringLiteral("a parameter needs a name"); return false; }
        if (g->param(p.name)) { *err = QStringLiteral("parameter '%1' already exists").arg(p.name); return false; }
        if (!paramTypeFromId(type, &p.type)) { *err = QStringLiteral("type must be bool|float|trigger"); return false; }
        g->params.push_back(p);
        return true;
    });
    if (ok) crumb("add_param", name);
    return ok;
}

bool MotionGraphManager::removeParam(const QString& name)
{
    return edit(QStringLiteral("Remove parameter"), [&](Graph* g, QString* err) {
        auto it = std::find_if(g->params.begin(), g->params.end(), [&](const Param& p) { return p.name == name; });
        if (it == g->params.end()) { *err = QStringLiteral("no parameter '%1'").arg(name); return false; }
        for (const auto& t : g->transitions)
            for (const auto& c : t.conditions)
                if (c.param == name) {
                    *err = QStringLiteral("'%1' is used by transition %2 — remove that condition first").arg(name, displayName(t));
                    return false;
                }
        g->params.erase(it);
        return true;
    });
}

bool MotionGraphManager::setParamValue(const QString& name, double value, QString* error)
{
    if (!std::isfinite(value)) {
        if (error) *error = QStringLiteral("value must be finite");
        return false;
    }
    if (m_playing) {
        for (auto& p : m_runParams)
            if (p.name == name) {
                p.value = p.type == ParamType::Float ? value : (value != 0.0 ? 1.0 : 0.0);
                emit paramsChanged();
                crumb("param", QStringLiteral("%1 = %2").arg(name).arg(p.value));
                return true;
            }
        if (error) *error = QStringLiteral("no parameter '%1'").arg(name);
        return false;
    }
    const QString entity = m_entity;
    Graph g = m_graphs.value(entity);
    Param* p = g.param(name);
    if (!p) {
        if (error) *error = QStringLiteral("no parameter '%1'").arg(name);
        return false;
    }
    p->value = p->type == ParamType::Float ? value : (value != 0.0 ? 1.0 : 0.0);
    return setGraph(entity, g, true, error, QStringLiteral("Set parameter"));
}

bool MotionGraphManager::setParamFromUi(const QString& name, double value)
{
    QString err;
    if (!setParamValue(name, value, &err)) { setStatus(err, false); return false; }
    return true;
}

bool MotionGraphManager::fireTrigger(const QString& name)
{
    if (!m_playing) {
        setStatus(QStringLiteral("press Play first — a trigger fires the running graph"), false);
        return false;
    }
    return setParamFromUi(name, 1.0);
}

bool MotionGraphManager::buildLocomotionTemplate()
{
    const QStringList clips = clipsOf(m_entity);
    if (clips.isEmpty()) {
        setStatus(QStringLiteral("'%1' has no skeletal clips").arg(m_entity), false);
        return false;
    }
    auto pick = [&](const char* word, int fallback) {
        for (const QString& c : clips) if (c.contains(QLatin1String(word), Qt::CaseInsensitive)) return c;
        return clips.value(std::min(fallback, int(clips.size()) - 1));
    };
    const bool ok = edit(QStringLiteral("Locomotion template"), [&](Graph* g, QString*) {
        Graph t;
        t.states = {{QStringLiteral("idle"), pick("idle", 0), true, 1.0, 20, 70},
                    {QStringLiteral("walk"), pick("walk", 1), true, 1.0, 190, 70},
                    {QStringLiteral("run"), pick("run", 2), true, 1.0, 360, 70}};
        t.entry = QStringLiteral("idle");
        t.params = g->params;
        if (!t.param(QStringLiteral("speed"))) t.params.push_back({QStringLiteral("speed"), ParamType::Float, 0.0});
        auto mk = [](const char* id, const char* from, const char* to, Op op, double v) {
            Transition x;
            x.id = QString::fromLatin1(id);
            x.from = QString::fromLatin1(from);
            x.to = QString::fromLatin1(to);
            x.conditions = {{QStringLiteral("speed"), op, v}};
            x.duration = 0.3;
            x.curve = Curve::Ease;
            return x;
        };
        t.transitions = {mk("t1", "idle", "walk", Op::Greater, 0.1), mk("t2", "walk", "run", Op::Greater, 2.0),
                         mk("t3", "run", "walk", Op::LessEq, 2.0), mk("t4", "walk", "idle", Op::LessEq, 0.1)};
        *g = t;
        return true;
    });
    if (ok) crumb("template", m_entity);
    return ok;
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

bool MotionGraphManager::play(const QString& entity, QString* error)
{
    auto fail = [&](const QString& m) {
        if (error) *error = m;
        setStatus(m, false);
        return false;
    };
    if (m_playing) stop();
    ensureSceneHook();
    Ogre::Entity* e = entityByName(entity);
    if (!e || !e->hasSkeleton() || !e->getAllAnimationStates()) return fail(QStringLiteral("'%1' is not an animated entity").arg(entity));
    if (!m_graphs.contains(entity)) return fail(QStringLiteral("'%1' has no motion graph").arg(entity));
    const Graph g = m_graphs.value(entity);
    QString err;
    if (!validate(g, &err)) return fail(err);
    if (!g.state(g.entry)) return fail(QStringLiteral("the graph has no entry state"));
    for (const auto& s : g.states)
        if (!e->getAllAnimationStates()->hasAnimationState(s.clip.toStdString()))
            return fail(QStringLiteral("state '%1': '%2' has no clip '%3'").arg(s.name, entity, s.clip));

    // Snapshot everything we are about to drive.
    m_stateSaves.clear();
    const size_t numBones = e->getSkeleton()->getNumBones();
    for (const auto& [name, st] : e->getAllAnimationStates()->getAnimationStates()) {
        StateSave sv{name, st->getEnabled(), st->getTimePosition(), st->getWeight(), st->getLoop(), st->hasBlendMask(), {}};
        if (sv.hadMask)
            for (size_t b = 0; b < numBones; ++b) sv.mask.push_back(st->getBlendMaskEntry(b));
        m_stateSaves.push_back(std::move(sv));
    }
    m_savedBlendMode = e->getSkeleton()->getBlendMode();
    e->getSkeleton()->setBlendMode(Ogre::ANIMBLEND_CUMULATIVE);

    m_playEntity = entity;
    m_playGraph = g;
    m_runParams = g.params;
    start(m_playGraph, &m_rt, clipLength, e);
    m_playing = true;
    applyToOgre();
    setStatus(QStringLiteral("Playing the motion graph from '%1'").arg(g.entry), true);
    crumb("play", entity);
    emit playbackChanged();
    emit paramsChanged();
    return true;
}

void MotionGraphManager::stop()
{
    if (!m_playing) return;
    restoreOgre();
    m_playing = false;
    m_rt = Runtime{};
    m_runParams.clear();
    crumb("stop", m_playEntity);
    m_playEntity.clear();
    emit playbackChanged();
    emit paramsChanged();
}

bool MotionGraphManager::playFromUi()
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("Motion graph: play"));
    return play(m_entity);
}

void MotionGraphManager::stopFromUi()
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("Motion graph: stop"));
    stop();
    setStatus(QStringLiteral("Stopped — the entity's own animation states are restored."), true);
}

bool MotionGraphManager::drives(const Ogre::Entity* e) const
{
    return m_playing && e && QString::fromStdString(e->getName()) == m_playEntity;
}

double MotionGraphManager::blendProgress() const
{
    if (!m_playing || !m_rt.blending || m_rt.blendDuration <= 0.0) return 1.0;
    return std::clamp(m_rt.blendElapsed / m_rt.blendDuration, 0.0, 1.0);
}

void MotionGraphManager::tick(double dt)
{
    if (!m_playing) return;
    Ogre::Entity* e = entityByName(m_playEntity);
    if (!e || !e->hasSkeleton()) {
        // The entity went away under us: nothing left to restore.
        m_playing = false;
        m_stateSaves.clear();
        emit playbackChanged();
        return;
    }
    const bool wasBlending = m_rt.blending;
    const QString fired = step(m_playGraph, &m_runParams, &m_rt, dt, clipLength, e);
    applyToOgre();
    if (!fired.isEmpty()) {
        crumb("transition", fired);
        emit paramsChanged();   // a trigger may have been consumed
    }
    if (!fired.isEmpty() || wasBlending || m_rt.blending) emit playbackChanged();
}

void MotionGraphManager::applyToOgre()
{
    Ogre::Entity* e = entityByName(m_playEntity);
    if (!e || !e->hasSkeleton()) return;
    Ogre::SkeletonInstance* skel = e->getSkeleton();
    Ogre::AnimationStateSet* states = e->getAllAnimationStates();
    const size_t numBones = skel->getNumBones();

    // Per clip: per-bone weight, and the time of its strongest contribution.
    struct Agg { double time = 0.0; double strongest = -1.0; std::vector<float> w; };
    QHash<QString, Agg> agg;
    for (const Contribution& c : contributions(m_rt)) {
        if (!states->hasAnimationState(c.clip.toStdString())) continue;
        Agg& a = agg[c.clip];
        if (a.w.empty()) a.w.assign(numBones, 0.0f);
        if (c.weight > a.strongest) { a.strongest = c.weight; a.time = c.time; }
        const QSet<QString> mask(c.mask.begin(), c.mask.end());
        for (unsigned short b = 0; b < numBones; ++b) {
            float f = 1.0f;
            if (!mask.isEmpty()) {
                const bool in = mask.contains(QString::fromStdString(skel->getBone(b)->getName()));
                f = (c.invertMask ? !in : in) ? 1.0f : 0.0f;
            }
            a.w[b] += float(c.weight) * f;
        }
    }
    // Normalise per bone (cumulative blending expects Σ = 1 per bone).
    for (unsigned short b = 0; b < numBones; ++b) {
        float total = 0.0f;
        for (auto it = agg.begin(); it != agg.end(); ++it) total += it->w[b];
        if (total > 1e-6f)
            for (auto it = agg.begin(); it != agg.end(); ++it) it->w[b] /= total;
    }
    for (const auto& [name, st] : states->getAnimationStates()) {
        auto it = agg.find(QString::fromStdString(name));
        if (it == agg.end()) {
            st->setEnabled(false);
            continue;
        }
        st->setEnabled(true);
        // The runtime already wraps looping items into [0, length) and clamps
        // one-shots at length; an Ogre LOOPING state would fmod(length) back
        // to 0 and show a finished one-shot's FIRST frame.
        st->setLoop(false);
        st->setWeight(1.0f);
        st->setTimePosition(float(it->time));
        if (!st->hasBlendMask()) st->createBlendMask(numBones, 0.0f);
        for (unsigned short b = 0; b < numBones; ++b) st->setBlendMaskEntry(b, it->w[b]);
    }
    states->_notifyDirty();
}

void MotionGraphManager::restoreOgre()
{
    Ogre::Entity* e = entityByName(m_playEntity);
    if (!e || !e->hasSkeleton()) { m_stateSaves.clear(); return; }
    Ogre::AnimationStateSet* states = e->getAllAnimationStates();
    for (const StateSave& s : m_stateSaves) {
        if (!states->hasAnimationState(s.name)) continue;
        Ogre::AnimationState* st = states->getAnimationState(s.name);
        if (!s.hadMask && st->hasBlendMask()) st->destroyBlendMask();
        if (s.hadMask && st->hasBlendMask())
            for (size_t b = 0; b < s.mask.size(); ++b) st->setBlendMaskEntry(b, s.mask[b]);
        st->setEnabled(s.enabled);
        st->setLoop(s.loop);
        st->setTimePosition(s.time);
        st->setWeight(s.weight);
    }
    e->getSkeleton()->setBlendMode(m_savedBlendMode);
    states->_notifyDirty();
    m_stateSaves.clear();
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

QString MotionGraphManager::sidecarPath(const QString& assetPath)
{
    const QFileInfo fi(assetPath);
    return fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".animgraph.json"));
}

bool MotionGraphManager::writeSidecar(const QString& assetPath, const QStringList& entities) const
{
    const QString path = sidecarPath(assetPath);
    QJsonArray graphs;
    QStringList names = m_graphs.keys();
    names.sort();   // byte-stable file
    for (const QString& n : names) {
        if (!entities.isEmpty() && !entities.contains(n)) continue;
        const Graph& g = m_graphs[n];
        if (g.states.empty()) continue;
        graphs.append(QJsonObject{{QStringLiteral("entity"), n}, {QStringLiteral("graph"), toJson(g)}});
    }
    if (graphs.isEmpty()) {
        if (QFile::exists(path)) QFile::remove(path);
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(QJsonObject{{QStringLiteral("schema"), docSchema()}, {QStringLiteral("graphs"), graphs}})
                .toJson(QJsonDocument::Indented));
    crumb("save", QStringLiteral("%1 graphs").arg(graphs.size()));
    return f.commit();
}

int MotionGraphManager::loadSidecar(const QString& assetPath, const QHash<QString, QString>& rename,
                                    const QString& fallbackEntity, QString* error)
{
    QFile f(sidecarPath(assetPath));
    if (!f.exists()) return 0;
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = f.errorString(); return -1; }
    const QJsonObject doc = QJsonDocument::fromJson(f.readAll()).object();
    if (doc.value(QStringLiteral("schema")).toString() != docSchema()) {
        if (error) *error = QStringLiteral("not a %1 file").arg(docSchema());
        return -1;
    }
    const QJsonArray arr = doc.value(QStringLiteral("graphs")).toArray();
    // Parse everything before committing anything.
    std::vector<std::pair<QString, Graph>> parsed;
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        Graph g;
        QString e;
        if (!fromJson(o.value(QStringLiteral("graph")).toObject(), &g, &e)) {
            if (error) *error = e;
            return -1;
        }
        QString name = o.value(QStringLiteral("entity")).toString();
        if (rename.contains(name)) name = rename.value(name);
        else if (arr.size() == 1 && !fallbackEntity.isEmpty()) name = fallbackEntity;
        parsed.emplace_back(name, g);
    }
    ensureSceneHook();
    for (const auto& [name, g] : parsed) m_graphs.insert(name, g);
    if (m_entity.isEmpty() && !parsed.empty()) m_entity = parsed.front().first;
    emit graphChanged();
    emit paramsChanged();
    crumb("load", QStringLiteral("%1 graphs").arg(parsed.size()));
    return int(parsed.size());
}

// ---------------------------------------------------------------------------
// QML rows
// ---------------------------------------------------------------------------

QVariantList MotionGraphManager::stateRows() const
{
    QVariantList rows;
    const Graph g = m_graphs.value(m_entity);
    for (const auto& s : g.states) {
        QVariantMap m;
        m[QStringLiteral("name")] = s.name;
        m[QStringLiteral("clip")] = s.clip;
        m[QStringLiteral("loop")] = s.loop;
        m[QStringLiteral("speed")] = s.speed;
        m[QStringLiteral("x")] = s.x;
        m[QStringLiteral("y")] = s.y;
        m[QStringLiteral("entry")] = s.name == g.entry;
        rows << m;
    }
    return rows;
}

QVariantList MotionGraphManager::transitionRows() const
{
    QVariantList rows;
    const Graph g = m_graphs.value(m_entity);
    for (const auto& t : g.transitions) {
        QStringList conds;
        for (const auto& c : t.conditions)
            conds << (c.op == Op::IsTrue || c.op == Op::IsFalse ? QStringLiteral("%1 is %2").arg(c.param, opId(c.op))
                                                                 : QStringLiteral("%1 %2 %3").arg(c.param, opId(c.op)).arg(c.value));
        if (t.exitTime >= 0.0) conds << QStringLiteral("after %1%").arg(int(std::lround(t.exitTime * 100)));
        QVariantMap m;
        m[QStringLiteral("id")] = t.id;
        m[QStringLiteral("from")] = t.from;
        m[QStringLiteral("to")] = t.to;
        m[QStringLiteral("label")] = displayName(t);
        m[QStringLiteral("summary")] = conds.isEmpty() ? QStringLiteral("always") : conds.join(QStringLiteral(" and "));
        m[QStringLiteral("duration")] = t.duration;
        m[QStringLiteral("curve")] = curveId(t.curve);
        m[QStringLiteral("masked")] = !t.mask.isEmpty();
        m[QStringLiteral("maskCount")] = int(t.mask.size());
        rows << m;
    }
    return rows;
}

QVariantMap MotionGraphManager::transitionDetails(const QString& id) const
{
    const Graph g = m_graphs.value(m_entity);
    for (const auto& t : g.transitions) {
        if (t.id != id) continue;
        QVariantList conds;
        for (const auto& c : t.conditions)
            conds << QVariantMap{{QStringLiteral("param"), c.param}, {QStringLiteral("op"), opId(c.op)},
                                 {QStringLiteral("value"), c.value}};
        return QVariantMap{{QStringLiteral("id"), t.id}, {QStringLiteral("from"), t.from}, {QStringLiteral("to"), t.to},
                           {QStringLiteral("label"), displayName(t)}, {QStringLiteral("duration"), t.duration},
                           {QStringLiteral("curve"), curveId(t.curve)},
                           {QStringLiteral("exitTime"), t.exitTime >= 0.0 ? QVariant(t.exitTime) : QVariant(QString())},
                           {QStringLiteral("conditions"), conds}, {QStringLiteral("mask"), t.mask}};
    }
    return {};
}

QVariantList MotionGraphManager::paramRows() const
{
    QVariantList rows;
    const std::vector<Param>& ps = m_playing && m_playEntity == m_entity ? m_runParams : m_graphs.value(m_entity).params;
    for (const auto& p : ps)
        rows << QVariantMap{{QStringLiteral("name"), p.name}, {QStringLiteral("type"), paramTypeId(p.type)},
                            {QStringLiteral("value"), p.value}};
    return rows;
}

QStringList MotionGraphManager::skinnedEntities() const
{
    QStringList out;
    if (Manager* m = Manager::getSingletonPtr())
        for (Ogre::MovableObject* obj : m->getEntities())
            if (obj && obj->getMovableType() == "Entity") {
                auto* e = static_cast<Ogre::Entity*>(obj);
                if (e->hasSkeleton() && e->getSkeleton()->getNumAnimations() > 0) out << QString::fromStdString(e->getName());
            }
    return out;
}

QString MotionGraphManager::entityFromSelection() const
{
    if (auto* acc = AnimationControlController::instance())
        if (!acc->selectedEntityName().isEmpty() && skinnedEntities().contains(acc->selectedEntityName()))
            return acc->selectedEntityName();
    SelectionSet* sel = SelectionSet::getSingletonPtr();
    Manager* m = Manager::getSingletonPtr();
    if (!sel || !m || !m->getSceneMgr()) return {};
    const auto& entities = m->getSceneMgr()->getMovableObjects("Entity");
    for (Ogre::Entity* e : sel->getResolvedEntities())
        for (const auto& kv : entities)
            if (kv.second == e && e->hasSkeleton()) return QString::fromStdString(e->getName());
    return {};
}

QStringList MotionGraphManager::clipsOf(const QString& entity) const
{
    QStringList out;
    Ogre::Entity* e = entityByName(entity);
    if (e && e->hasSkeleton())
        for (unsigned short i = 0; i < e->getSkeleton()->getNumAnimations(); ++i)
            out << QString::fromStdString(e->getSkeleton()->getAnimation(i)->getName());
    return out;
}

QStringList MotionGraphManager::bonesOf(const QString& entity) const
{
    QStringList out;
    Ogre::Entity* e = entityByName(entity);
    if (e && e->hasSkeleton())
        for (unsigned short i = 0; i < e->getSkeleton()->getNumBones(); ++i)
            out << QString::fromStdString(e->getSkeleton()->getBone(i)->getName());
    return out;
}
