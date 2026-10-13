/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef ANIMGRAPH_H
#define ANIMGRAPH_H

// Motion graph (#526, epic #517 Slice I) — pure data + runtime.
//
// A small state machine for PREVIEWING clip logic inside the editor
// (idle → walk → run, weapon draw → idle). It is authoring-only: the graph is
// saved with the project (sidecar), but export writes only the constituent
// clips — engine-native graphs (Mecanim / AnimBP / AnimationTree) are out of
// scope.
//
//   State       a clip on the entity (loop, speed)
//   Transition  from a state (or "*" = any state) to another, firing when ALL
//               its conditions hold and (optionally) the source clip passed
//               its exit time; blends over `duration` with a curve
//               (linear / ease / step). An optional BONE MASK makes it a
//               partial transition: the target drives only the masked bones
//               while the clip that was playing carries on for the rest
//               ("upper-body wave over a running lower body").
//   Parameter   bool / float / trigger values the conditions read; a trigger
//               is consumed by the transition it fires.
//
// The runtime is pure: step() advances clocks, picks transitions and
// returns per-clip CONTRIBUTIONS (clip, time, weight, optional bone mask).
// Within one pose the weights of every bone sum to 1; a cross-fade scales the
// outgoing pose by (1-w) and the incoming one by w, so per-bone sums stay 1.
// MotionGraphManager turns contributions into Ogre AnimationState weights +
// blend masks.

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace AnimGraph {

enum class ParamType { Bool, Float, Trigger };
QString paramTypeId(ParamType t);                    // bool|float|trigger
bool paramTypeFromId(const QString& id, ParamType* out);

struct Param {
    QString name;
    ParamType type = ParamType::Float;
    double value = 0.0;   ///< bool/trigger: 0 or 1
};

enum class Op { Greater, Less, GreaterEq, LessEq, Equal, NotEqual, IsTrue, IsFalse };
QString opId(Op o);                                  // > < >= <= == != true false
bool opFromId(const QString& id, Op* out);
QStringList opIds();

struct Condition {
    QString param;
    Op op = Op::Greater;
    double value = 0.0;
};

enum class Curve { Linear, Ease, Step };
QString curveId(Curve c);                            // linear|ease|step
bool curveFromId(const QString& id, Curve* out);
/// Blend weight of the incoming pose at normalised progress t ∈ [0,1].
double curveWeight(Curve c, double t);

struct State {
    QString name;
    QString clip;
    bool loop = true;
    double speed = 1.0;
    double x = 0.0, y = 0.0;   ///< panel position
};

struct Transition {
    QString id;
    QString from;              ///< state name, or "*" (any state)
    QString to;
    std::vector<Condition> conditions;
    double exitTime = -1.0;    ///< normalised source time [0..1] to wait for; < 0 = none
    double duration = 0.25;    ///< seconds
    Curve curve = Curve::Linear;
    QStringList mask;          ///< bone names; empty = whole body
};

struct Graph {
    QString entry;
    std::vector<State> states;
    std::vector<Transition> transitions;
    std::vector<Param> params;

    const State* state(const QString& name) const;
    const Param* param(const QString& name) const;
    Param* param(const QString& name);
};

QString displayName(const Transition& t);

/// Structural check: unique non-empty state names, entry exists (when there
/// are states), transitions reference existing states and parameters,
/// unique parameter names, finite numbers.
bool validate(const Graph& g, QString* error = nullptr);

// ---------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------

/// One clip being played inside a pose.
struct Item {
    QString state;
    QString clip;
    double time = 0.0;
    double length = 0.0;
    double speed = 1.0;
    bool loop = true;
    bool wrapped = false;   ///< a looping clip passed its end during the last step
};

/// What drives the skeleton: `primary` on the masked bones (all bones when
/// `mask` is empty), `base` on the rest.
struct Pose {
    Item primary;
    QStringList mask;
    bool hasBase = false;
    Item base;
};

struct Runtime {
    bool started = false;
    Pose current;
    bool blending = false;
    Pose previous;
    double blendElapsed = 0.0;
    double blendDuration = 0.0;
    Curve blendCurve = Curve::Linear;
    QString lastTransition;   ///< id of the most recent transition
};

/// One clip's share of the final pose.
struct Contribution {
    QString clip;
    double time = 0.0;
    double weight = 0.0;
    QStringList mask;     ///< empty = all bones
    bool invertMask = false;   ///< true: every bone EXCEPT `mask`
};

/// Clip length lookup (seconds); unknown clips report 0.
using ClipLength = double (*)(const QString& clip, const void* ctx);

/// Enter the entry state (time 0). False when the graph has no entry.
bool start(const Graph& g, Runtime* rt, ClipLength len, const void* ctx);

/// Advance by dt seconds: advance clocks, progress the active blend, then
/// fire the first matching transition (in list order). Triggers consumed by
/// the fired transition are reset in `params`. Returns the id of the
/// transition fired this step, or "".
QString step(const Graph& g, std::vector<Param>* params, Runtime* rt, double dt, ClipLength len, const void* ctx);

/// The per-clip contributions for the runtime's current pose(s).
std::vector<Contribution> contributions(const Runtime& rt);

/// Normalised [0..1] progress of an item (looping clips wrap).
double normalisedTime(const Item& it);

/// Would `t`'s conditions hold right now (ignores exit time)?
bool conditionsHold(const Transition& t, const std::vector<Param>& params);

// ---------------------------------------------------------------------------
// JSON — schema `qtmesh-anim-graph-v1`
// ---------------------------------------------------------------------------

QString schemaId();
QJsonObject toJson(const Graph& g);
bool fromJson(const QJsonObject& o, Graph* out, QString* error = nullptr);

QString uniqueTransitionId(const Graph& g);
QString uniqueStateName(const Graph& g, const QString& base);

} // namespace AnimGraph

#endif // ANIMGRAPH_H
