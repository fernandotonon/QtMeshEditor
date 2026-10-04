#ifndef AIAGENTTYPES_H
#define AIAGENTTYPES_H

// Pure-data types for the AI agent harness (#1000 / #1001).
//
// Everything the agent knows about a task lives in these structures, NOT in
// the LLM prompt: the plan, each step's status and attempts, and one
// structured observation per executed step. The prompt is rebuilt from this
// state on every planner call, so nothing is lost to a sliding history
// window and tests can drive the whole state machine without a model.
//
// Ogre-free and Qt-only so it compiles into the headless unit tests.

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

namespace AIAgent {

enum class State {
    Idle,
    Planning,               // waiting for the planner's capability pick / plan
    Executing,              // a tool call is running
    Observing,              // turning the tool result into an observation
    Verifying,              // all steps done, building the final summary
    Replanning,             // waiting for the planner to repair the plan
    AwaitingConfirmation,   // a destructive step needs the user's OK
    Completed,
    Failed,
    Cancelled
};

inline QString stateName(State s)
{
    switch (s) {
    case State::Idle:                 return QStringLiteral("idle");
    case State::Planning:             return QStringLiteral("planning");
    case State::Executing:            return QStringLiteral("executing");
    case State::Observing:            return QStringLiteral("observing");
    case State::Verifying:            return QStringLiteral("verifying");
    case State::Replanning:           return QStringLiteral("replanning");
    case State::AwaitingConfirmation: return QStringLiteral("awaiting_confirmation");
    case State::Completed:            return QStringLiteral("completed");
    case State::Failed:               return QStringLiteral("failed");
    case State::Cancelled:            return QStringLiteral("cancelled");
    }
    return QStringLiteral("unknown");
}

inline bool isTerminal(State s)
{
    return s == State::Completed || s == State::Failed || s == State::Cancelled;
}

struct Step {
    // Repaired = it failed, and a replan appended a replacement that takes
    // over; it is neither a pending failure nor an intentional skip.
    enum Status { Pending, Running, Succeeded, Failed, Skipped, Repaired };

    QString     tool;          // MCP tool name
    QJsonObject arguments;     // validated/coerced before execution
    QString     why;           // the planner's one-line rationale (shown in the UI)
    Status      status   = Pending;
    int         attempts = 0;
    QString     error;         // last error text when status == Failed

    static QString statusName(Status s)
    {
        switch (s) {
        case Pending:   return QStringLiteral("pending");
        case Running:   return QStringLiteral("running");
        case Succeeded: return QStringLiteral("succeeded");
        case Failed:    return QStringLiteral("failed");
        case Skipped:   return QStringLiteral("skipped");
        case Repaired:  return QStringLiteral("repaired");
        }
        return QStringLiteral("unknown");
    }

    /// Stable identity of "the same action": tool + compact arguments. Two
    /// consecutive failures with the same signature mean the planner is
    /// stuck, not unlucky.
    QString signature() const;

    QJsonObject toJson() const;
};

struct Artifact {
    QString kind;   // "file" | "entity" | "material" | "skeleton" | ...
    QString name;   // path for files, scene name otherwise
    QJsonObject toJson() const { return {{"kind", kind}, {"name", name}}; }
};

/// What a tool call produced, in a form the planner can reason about
/// without reading the tool's raw text. `facts` holds numbers/flags parsed
/// out of the result (triangles, vertices, bones, hasSkeleton, ...); `raw`
/// keeps the untruncated tool text for debugging and the chat transcript
/// but is NEVER copied into a prompt wholesale.
struct Observation {
    int         stepIndex = -1;
    QString     tool;
    QString     status;     // "success" | "error" | "denied" | "invalid_arguments"
    QStringList artifacts;
    QJsonObject facts;
    QStringList warnings;
    QString     error;      // one line, when status != success
    QString     raw;

    QJsonObject toJson(bool includeRaw = false) const;
    /// Compact one-line form for the replan prompt.
    QString toPromptLine() const;
};

struct Plan {
    QString        title;          // "AI: segment + rig wolf" — also the undo group name
    QString        goal;           // the user's request, verbatim
    QStringList    capabilities;   // capability ids the planner selected
    QVector<Step>  steps;

    int  nextPendingIndex() const;
    bool allDone() const;
    QJsonObject toJson() const;
};

struct Limits {
    int maxSteps            = 12;   // hard cap on plan length (incl. repairs)
    int maxAttemptsPerStep  = 2;    // 1 retry of the same step on a recoverable error
    int maxReplans          = 2;    // planner repair rounds per task
    int maxPlannerRetries   = 2;    // malformed planner output → re-ask, then fail
    int maxRepeatedFailures = 2;    // identical failing action N times → stop
};

/// Build an observation from an MCP tool result (`{content:[{text}], isError}`).
/// Pure: parses `Vertices: N` / `Triangles: N` / `Bones: N` / `Created X` /
/// file paths with a 3D or image extension, and flags "Error:" text.
Observation observationFromToolResult(int stepIndex, const QString& tool,
                                      const QJsonObject& toolResult);

/// Deterministic, model-free summary of a finished task — used as the final
/// chat message so a flaky model cannot misreport what actually happened.
QString summarize(const Plan& plan, const QVector<Observation>& observations,
                  State finalState);

} // namespace AIAgent

#endif // AIAGENTTYPES_H
