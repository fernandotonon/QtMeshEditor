/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "AnimGraph.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cmath>

namespace AnimGraph {

namespace {

struct OpRow { Op op; const char* id; };
const OpRow kOps[] = {
    {Op::Greater, ">"}, {Op::Less, "<"}, {Op::GreaterEq, ">="}, {Op::LessEq, "<="},
    {Op::Equal, "=="}, {Op::NotEqual, "!="}, {Op::IsTrue, "true"}, {Op::IsFalse, "false"},
};

bool finite(double v) { return std::isfinite(v); }

Item makeItem(const State& s, ClipLength len, const void* ctx)
{
    Item it;
    it.state = s.name;
    it.clip = s.clip;
    it.length = len ? std::max(0.0, len(s.clip, ctx)) : 0.0;
    it.speed = s.speed;
    it.loop = s.loop;
    return it;
}

void advance(Item* it, double dt)
{
    it->time += dt * it->speed;
    it->wrapped = false;
    if (it->length <= 0.0) { it->time = 0.0; return; }
    if (it->loop) {
        // Remember a pass over the end: an exit time the wrap jumped over
        // (a frame hitch, a fast state) has still been reached.
        it->wrapped = it->time >= it->length;
        it->time = std::fmod(it->time, it->length);
        if (it->time < 0) it->time += it->length;
    } else {
        it->time = std::clamp(it->time, 0.0, it->length);
    }
}

void advance(Pose* p, double dt)
{
    advance(&p->primary, dt);
    if (p->hasBase) advance(&p->base, dt);
}

void addPose(const Pose& p, double w, std::vector<Contribution>* out)
{
    if (w <= 0.0) return;
    if (p.mask.isEmpty() || !p.hasBase) {
        out->push_back({p.primary.clip, p.primary.time, w, p.mask, false});
        return;
    }
    out->push_back({p.primary.clip, p.primary.time, w, p.mask, false});
    out->push_back({p.base.clip, p.base.time, w, p.mask, true});
}

double num(const QJsonValue& v, double def) { return v.isDouble() && finite(v.toDouble()) ? v.toDouble() : def; }

} // namespace

// ---------------------------------------------------------------------------
// Enums
// ---------------------------------------------------------------------------

QString paramTypeId(ParamType t)
{
    switch (t) {
    case ParamType::Bool: return QStringLiteral("bool");
    case ParamType::Trigger: return QStringLiteral("trigger");
    case ParamType::Float: break;
    }
    return QStringLiteral("float");
}

bool paramTypeFromId(const QString& id, ParamType* out)
{
    const QString s = id.trimmed().toLower();
    if (s == QLatin1String("bool") || s == QLatin1String("boolean")) { *out = ParamType::Bool; return true; }
    if (s == QLatin1String("float") || s == QLatin1String("number")) { *out = ParamType::Float; return true; }
    if (s == QLatin1String("trigger")) { *out = ParamType::Trigger; return true; }
    return false;
}

QString opId(Op o)
{
    for (const auto& r : kOps) if (r.op == o) return QString::fromLatin1(r.id);
    return QStringLiteral(">");
}

bool opFromId(const QString& id, Op* out)
{
    const QString s = id.trimmed().toLower();
    for (const auto& r : kOps) if (s == QLatin1String(r.id)) { *out = r.op; return true; }
    if (s == QLatin1String("set")) { *out = Op::IsTrue; return true; }
    return false;
}

QStringList opIds()
{
    QStringList l;
    for (const auto& r : kOps) l << QString::fromLatin1(r.id);
    return l;
}

QString curveId(Curve c)
{
    switch (c) {
    case Curve::Ease: return QStringLiteral("ease");
    case Curve::Step: return QStringLiteral("step");
    case Curve::Linear: break;
    }
    return QStringLiteral("linear");
}

bool curveFromId(const QString& id, Curve* out)
{
    const QString s = id.trimmed().toLower();
    if (s == QLatin1String("linear")) { *out = Curve::Linear; return true; }
    if (s == QLatin1String("ease") || s == QLatin1String("smooth")) { *out = Curve::Ease; return true; }
    if (s == QLatin1String("step")) { *out = Curve::Step; return true; }
    return false;
}

double curveWeight(Curve c, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    switch (c) {
    case Curve::Ease: return t * t * (3.0 - 2.0 * t);
    case Curve::Step: return t >= 1.0 ? 1.0 : 0.0;
    case Curve::Linear: break;
    }
    return t;
}

// ---------------------------------------------------------------------------
// Graph
// ---------------------------------------------------------------------------

const State* Graph::state(const QString& name) const
{
    for (const auto& s : states) if (s.name == name) return &s;
    return nullptr;
}

const Param* Graph::param(const QString& name) const
{
    for (const auto& p : params) if (p.name == name) return &p;
    return nullptr;
}

Param* Graph::param(const QString& name)
{
    for (auto& p : params) if (p.name == name) return &p;
    return nullptr;
}

QString displayName(const Transition& t)
{
    return QStringLiteral("%1 → %2").arg(t.from == QLatin1String("*") ? QStringLiteral("Any") : t.from, t.to);
}

bool validate(const Graph& g, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    QSet<QString> names;
    for (const auto& s : g.states) {
        if (s.name.trimmed().isEmpty()) return fail(QStringLiteral("a state has no name"));
        if (s.name == QLatin1String("*")) return fail(QStringLiteral("'*' is reserved for 'any state'"));
        if (names.contains(s.name)) return fail(QStringLiteral("duplicate state '%1'").arg(s.name));
        if (!finite(s.speed) || !finite(s.x) || !finite(s.y)) return fail(QStringLiteral("state '%1' has a non-finite number").arg(s.name));
        names.insert(s.name);
    }
    if (!g.states.empty() && !names.contains(g.entry))
        return fail(QStringLiteral("entry state '%1' does not exist").arg(g.entry));
    QSet<QString> params;
    for (const auto& p : g.params) {
        if (p.name.trimmed().isEmpty()) return fail(QStringLiteral("a parameter has no name"));
        if (params.contains(p.name)) return fail(QStringLiteral("duplicate parameter '%1'").arg(p.name));
        if (!finite(p.value)) return fail(QStringLiteral("parameter '%1' is not a finite number").arg(p.name));
        params.insert(p.name);
    }
    QSet<QString> ids;
    for (const auto& t : g.transitions) {
        if (t.id.isEmpty() || ids.contains(t.id)) return fail(QStringLiteral("duplicate or empty transition id '%1'").arg(t.id));
        ids.insert(t.id);
        if (t.from != QLatin1String("*") && !names.contains(t.from))
            return fail(QStringLiteral("transition %1: no state '%2'").arg(t.id, t.from));
        if (!names.contains(t.to)) return fail(QStringLiteral("transition %1: no state '%2'").arg(t.id, t.to));
        if (!finite(t.duration) || t.duration < 0) return fail(QStringLiteral("transition %1: bad duration").arg(t.id));
        if (!finite(t.exitTime)) return fail(QStringLiteral("transition %1: bad exit time").arg(t.id));
        for (const auto& c : t.conditions) {
            if (!params.contains(c.param))
                return fail(QStringLiteral("transition %1: no parameter '%2'").arg(t.id, c.param));
            if (!finite(c.value)) return fail(QStringLiteral("transition %1: bad condition value").arg(t.id));
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

double normalisedTime(const Item& it)
{
    if (it.length <= 0.0) return 1.0;
    return std::clamp(it.time / it.length, 0.0, 1.0);
}

bool conditionsHold(const Transition& t, const std::vector<Param>& params)
{
    for (const auto& c : t.conditions) {
        const Param* p = nullptr;
        for (const auto& x : params) if (x.name == c.param) p = &x;
        if (!p) return false;
        const double v = p->value;
        bool ok = false;
        switch (c.op) {
        case Op::Greater: ok = v > c.value; break;
        case Op::Less: ok = v < c.value; break;
        case Op::GreaterEq: ok = v >= c.value; break;
        case Op::LessEq: ok = v <= c.value; break;
        case Op::Equal: ok = std::abs(v - c.value) < 1e-9; break;
        case Op::NotEqual: ok = std::abs(v - c.value) >= 1e-9; break;
        case Op::IsTrue: ok = v != 0.0; break;
        case Op::IsFalse: ok = v == 0.0; break;
        }
        if (!ok) return false;
    }
    return true;
}

bool start(const Graph& g, Runtime* rt, ClipLength len, const void* ctx)
{
    *rt = Runtime{};
    const State* entry = g.state(g.entry);
    if (!entry) return false;
    rt->current.primary = makeItem(*entry, len, ctx);
    rt->started = true;
    return true;
}

QString step(const Graph& g, std::vector<Param>* params, Runtime* rt, double dt, ClipLength len, const void* ctx)
{
    if (!rt->started) return {};
    dt = finite(dt) ? std::max(0.0, dt) : 0.0;

    // Clocks.
    advance(&rt->current, dt);
    if (rt->blending) {
        advance(&rt->previous, dt);
        rt->blendElapsed += dt;
        if (rt->blendElapsed >= rt->blendDuration) rt->blending = false;
    }

    // Transitions: the first whose source matches, conditions hold and exit
    // time (if any) has passed. A transition into the state we are already in
    // is ignored for "any state" sources (otherwise "* → run while speed>2"
    // would restart run every frame).
    const QString cur = rt->current.primary.state;
    for (const auto& t : g.transitions) {
        if (t.from != QLatin1String("*") && t.from != cur) continue;
        if (t.to == cur && t.from == QLatin1String("*")) continue;
        if (t.exitTime >= 0.0 && !rt->current.primary.wrapped
            && normalisedTime(rt->current.primary) < std::min(t.exitTime, 1.0))
            continue;
        if (!conditionsHold(t, *params)) continue;
        const State* target = g.state(t.to);
        if (!target) continue;

        Pose next;
        next.primary = makeItem(*target, len, ctx);
        // Ogre has ONE AnimationState per clip: when the target plays a clip
        // that is still playing (run → wave(partial) → run), continue its
        // clock instead of restarting it, or the blend would jump phase.
        const Pose& cp = rt->current;
        if (cp.hasBase && !cp.mask.isEmpty() && cp.base.clip == target->clip) next.primary.time = cp.base.time;
        else if (cp.primary.clip == target->clip) next.primary.time = cp.primary.time;
        if (!t.mask.isEmpty()) {
            // Partial transition: the target drives only the mask; whatever
            // drove the remaining bones keeps playing there.
            next.mask = t.mask;
            next.hasBase = true;
            next.base = rt->current.mask.isEmpty() || !rt->current.hasBase ? rt->current.primary : rt->current.base;
        }
        rt->previous = rt->current;
        rt->current = next;
        rt->blendElapsed = 0.0;
        rt->blendDuration = t.duration;
        rt->blendCurve = t.curve;
        rt->blending = t.duration > 0.0 && t.curve != Curve::Step;
        rt->lastTransition = t.id;
        // Consume the triggers this transition read.
        for (const auto& c : t.conditions)
            for (auto& p : *params)
                if (p.name == c.param && p.type == ParamType::Trigger) p.value = 0.0;
        return t.id;
    }
    return {};
}

std::vector<Contribution> contributions(const Runtime& rt)
{
    std::vector<Contribution> out;
    if (!rt.started) return out;
    double w = 1.0;
    if (rt.blending && rt.blendDuration > 0.0)
        w = curveWeight(rt.blendCurve, rt.blendElapsed / rt.blendDuration);
    addPose(rt.current, w, &out);
    if (rt.blending) addPose(rt.previous, 1.0 - w, &out);
    return out;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

QString schemaId() { return QStringLiteral("qtmesh-anim-graph-v1"); }

QJsonObject toJson(const Graph& g)
{
    QJsonArray states, transitions, params;
    for (const auto& s : g.states)
        states.append(QJsonObject{{QStringLiteral("name"), s.name}, {QStringLiteral("clip"), s.clip},
                                  {QStringLiteral("loop"), s.loop}, {QStringLiteral("speed"), s.speed},
                                  {QStringLiteral("x"), s.x}, {QStringLiteral("y"), s.y}});
    for (const auto& t : g.transitions) {
        QJsonArray conds;
        for (const auto& c : t.conditions)
            conds.append(QJsonObject{{QStringLiteral("param"), c.param}, {QStringLiteral("op"), opId(c.op)},
                                     {QStringLiteral("value"), c.value}});
        QJsonObject o{{QStringLiteral("id"), t.id}, {QStringLiteral("from"), t.from}, {QStringLiteral("to"), t.to},
                      {QStringLiteral("conditions"), conds}, {QStringLiteral("duration"), t.duration},
                      {QStringLiteral("curve"), curveId(t.curve)}};
        if (t.exitTime >= 0.0) o[QStringLiteral("exitTime")] = t.exitTime;
        if (!t.mask.isEmpty()) o[QStringLiteral("mask")] = QJsonArray::fromStringList(t.mask);
        transitions.append(o);
    }
    for (const auto& p : g.params)
        params.append(QJsonObject{{QStringLiteral("name"), p.name}, {QStringLiteral("type"), paramTypeId(p.type)},
                                  {QStringLiteral("value"), p.value}});
    return QJsonObject{{QStringLiteral("schema"), schemaId()}, {QStringLiteral("entry"), g.entry},
                       {QStringLiteral("states"), states}, {QStringLiteral("transitions"), transitions},
                       {QStringLiteral("params"), params}};
}

bool fromJson(const QJsonObject& o, Graph* out, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    if (o.value(QStringLiteral("schema")).toString() != schemaId())
        return fail(QStringLiteral("not a %1 document").arg(schemaId()));
    Graph g;
    g.entry = o.value(QStringLiteral("entry")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("states")).toArray()) {
        const QJsonObject s = v.toObject();
        State st;
        st.name = s.value(QStringLiteral("name")).toString();
        st.clip = s.value(QStringLiteral("clip")).toString();
        st.loop = s.value(QStringLiteral("loop")).toBool(true);
        st.speed = num(s.value(QStringLiteral("speed")), 1.0);
        st.x = num(s.value(QStringLiteral("x")), 0.0);
        st.y = num(s.value(QStringLiteral("y")), 0.0);
        g.states.push_back(st);
    }
    for (const QJsonValue& v : o.value(QStringLiteral("params")).toArray()) {
        const QJsonObject p = v.toObject();
        Param pr;
        pr.name = p.value(QStringLiteral("name")).toString();
        if (!paramTypeFromId(p.value(QStringLiteral("type")).toString(), &pr.type))
            return fail(QStringLiteral("parameter '%1': unknown type").arg(pr.name));
        pr.value = num(p.value(QStringLiteral("value")), 0.0);
        g.params.push_back(pr);
    }
    for (const QJsonValue& v : o.value(QStringLiteral("transitions")).toArray()) {
        const QJsonObject t = v.toObject();
        Transition tr;
        tr.id = t.value(QStringLiteral("id")).toString();
        tr.from = t.value(QStringLiteral("from")).toString();
        tr.to = t.value(QStringLiteral("to")).toString();
        tr.duration = num(t.value(QStringLiteral("duration")), 0.25);
        tr.exitTime = num(t.value(QStringLiteral("exitTime")), -1.0);
        if (t.contains(QStringLiteral("curve")) && !curveFromId(t.value(QStringLiteral("curve")).toString(), &tr.curve))
            return fail(QStringLiteral("transition %1: unknown curve").arg(tr.id));
        for (const QJsonValue& cv : t.value(QStringLiteral("conditions")).toArray()) {
            const QJsonObject c = cv.toObject();
            Condition cond;
            cond.param = c.value(QStringLiteral("param")).toString();
            if (!opFromId(c.value(QStringLiteral("op")).toString(), &cond.op))
                return fail(QStringLiteral("transition %1: unknown operator").arg(tr.id));
            cond.value = num(c.value(QStringLiteral("value")), 0.0);
            tr.conditions.push_back(cond);
        }
        for (const QJsonValue& m : t.value(QStringLiteral("mask")).toArray()) tr.mask << m.toString();
        g.transitions.push_back(tr);
    }
    if (!validate(g, error)) return false;
    *out = g;
    return true;
}

QString uniqueTransitionId(const Graph& g)
{
    for (int n = 1;; ++n) {
        const QString id = QStringLiteral("t%1").arg(n);
        if (std::none_of(g.transitions.begin(), g.transitions.end(), [&](const Transition& t) { return t.id == id; }))
            return id;
    }
}

QString uniqueStateName(const Graph& g, const QString& base)
{
    const QString b = base.trimmed().isEmpty() ? QStringLiteral("State") : base.trimmed();
    if (!g.state(b) && b != QLatin1String("*")) return b;
    for (int n = 2;; ++n) {
        const QString s = QStringLiteral("%1 %2").arg(b).arg(n);
        if (!g.state(s)) return s;
    }
}

} // namespace AnimGraph
