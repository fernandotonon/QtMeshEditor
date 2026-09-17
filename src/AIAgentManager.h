#ifndef AIAGENTMANAGER_H
#define AIAGENTMANAGER_H

// AIAgentManager — the orchestration layer above AIChatManager/LLMManager
// (#1000 / #1001 / #1003, epic #818 Track C6).
//
//   User -> AIAgentManager -> Planner -> Capability router -> Executor
//        -> Observer -> (Verifier) -> Replan / Finish
//
// One bounded step at a time. ALL task state (plan, step status, attempts,
// observations) lives in this object, never in the prompt; the prompt is
// rebuilt from it on every planner call. The planner and the tool executor
// are injected interfaces so the whole state machine is unit-tested
// headless with scripted fakes — no LLM, no Ogre.
//
// Production wiring: `McpToolExecutor` (MCPServer::callTool) and
// `LlmPlannerBackend` (LLMManager::generateText). AIChatManager stays the
// conversation facade and delegates multi-step work here.

#include "AIAgentTypes.h"
#include "AICapabilityRegistry.h"

#include <QJSEngine>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVector>
#include <QHash>
#include <functional>
#include <memory>

class QUndoStack;
class MCPServer;

/// Executes tools. Production: MCPServer. Tests: a scripted fake.
class AgentToolExecutor
{
public:
    virtual ~AgentToolExecutor() = default;
    virtual QJsonArray  toolList() = 0;
    virtual QJsonObject callTool(const QString& name, const QJsonObject& args) = 0;
};

/// Asynchronous planner (LLM) backend. `request` must eventually emit exactly
/// one of completed / failed / stopped.
class AgentPlannerBackend : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    virtual bool    available() const = 0;
    virtual QString modelName() const { return {}; }
    virtual void    request(const QString& systemPrompt, const QString& userPrompt, int maxTokens) = 0;
    virtual void    stop() = 0;
    /// True while a request is outstanding (including after stop() until the
    /// backend has delivered its stopped/completed/failed signal).
    virtual bool    pending() const { return false; }
signals:
    void completed(const QString& text);
    void failed(const QString& error);
    void stopped();
};

/// Production executor over the live MCP server.
class McpToolExecutor : public AgentToolExecutor
{
public:
    explicit McpToolExecutor(MCPServer* server);   // defined in the .cpp: QPointer needs the full type
    ~McpToolExecutor() override;
    QJsonArray  toolList() override;
    QJsonObject callTool(const QString& name, const QJsonObject& args) override;
private:
    QPointer<MCPServer> m_server;
};

class AIAgentManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString state READ stateName NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString planTitle READ planTitle NOTIFY planChanged)
    Q_PROPERTY(QVariantList plan READ planModel NOTIFY planChanged)
    Q_PROPERTY(int currentStep READ currentStep NOTIFY planChanged)
    Q_PROPERTY(QString pendingConfirmation READ pendingConfirmation NOTIFY confirmationChanged)
    Q_PROPERTY(bool trustedMode READ trustedMode WRITE setTrustedMode NOTIFY trustedModeChanged)
    Q_PROPERTY(QString lastSummary READ lastSummary NOTIFY stateChanged)
    Q_PROPERTY(QString recommendedModelName READ recommendedModelName CONSTANT)

public:
    static AIAgentManager* instance();
    static AIAgentManager* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    // ---- dependency injection ----
    void setExecutor(std::shared_ptr<AgentToolExecutor> executor);
    /// Takes ownership (parented). Production creates an LlmPlannerBackend lazily.
    void setPlanner(AgentPlannerBackend* planner);
    void setLimits(const AIAgent::Limits& limits) { m_limits = limits; }
    /// Undo stack the task's mutating steps are grouped on (default: UndoManager's).
    void setUndoStack(QUndoStack* stack) { m_undoStack = stack; }
    /// Extra text appended to every planner prompt (the scene summary). The
    /// facade refreshes it per turn (#1021c).
    void setContextProvider(std::function<QString()> provider) { m_contextProvider = std::move(provider); }

    // ---- state ----
    AIAgent::State state() const { return m_state; }
    QString stateName() const { return AIAgent::stateName(m_state); }
    bool busy() const { return m_state != AIAgent::State::Idle && !AIAgent::isTerminal(m_state); }
    const AIAgent::Plan& plan() const { return m_plan; }
    const QVector<AIAgent::Observation>& observations() const { return m_observations; }
    QString planTitle() const { return m_plan.title; }
    QVariantList planModel() const;
    int currentStep() const { return m_currentStep; }
    QString pendingConfirmation() const { return m_pendingReason; }
    QString lastSummary() const { return m_lastSummary; }
    int replanCount() const { return m_replans; }
    QString lastError() const { return m_lastError; }
    bool trustedMode() const { return m_trustedMode; }
    void setTrustedMode(bool on);
    QString recommendedModelName() const;
    /// True while the planner backend still owns an LLM generation — after a
    /// cancel the stop is asynchronous, and the facade must keep ignoring
    /// LLMManager callbacks until that request drains (review finding).
    bool plannerPending() const;
    /// True when the loaded model is one we consider capable of the agent's
    /// multi-step tool protocol (drives the panel's "tip" banner).
    Q_INVOKABLE static bool modelIsRecommended(const QString& modelName);
    const AICapabilityRegistry& registry() const { return m_registry; }

    /// Conversation memory across tasks (#1021c): the last few requests and
    /// what came of them, injected into every planner prompt so "now make it
    /// red" resolves against the previous task. Cleared with the chat.
    Q_INVOKABLE void clearHistory();
    int historySize() const { return m_history.size(); }
    /// Path of the per-task trace (prompts, replies, tool results) — the
    /// thing to read when a task went wrong. Overwritten on every task.
    static QString traceLogPath();

    // ---- control ----
    /// Start a task. Returns false (with a chat error) when busy, no executor,
    /// or the planner is unavailable (no model loaded).
    Q_INVOKABLE bool startTask(const QString& request);
    Q_INVOKABLE void cancel();
    /// Answer a pending destructive-step confirmation. `alwaysAllow` also
    /// switches trusted mode on for the session.
    Q_INVOKABLE void confirmPendingStep(bool approve, bool alwaysAllow = false);

    // ---- pure helpers (unit-tested) ----
    static QString extractJsonObject(const QString& text);
    /// Parses a planner reply into `out`. `needCapabilities` receives the
    /// ids when the model asked for more docs instead of planning;
    /// `answer` receives a direct answer when the model returned a summary
    /// with no steps (a question about the scene).
    static bool parsePlanReply(const QString& text, AIAgent::Plan* out,
                               QStringList* needCapabilities, QString* answer, QString* error);
    static bool parseReplanReply(const QString& text, QVector<AIAgent::Step>* steps,
                                 bool* done, QString* summary, QString* error);

signals:
    void stateChanged();
    void planChanged();
    void confirmationChanged();
    void trustedModeChanged();
    /// A line for the chat transcript: role = "assistant" | "tool" | "plan".
    void chatMessage(const QString& role, const QString& text, bool isTool);
    void stepFinished(int index, bool ok);
    void taskFinished(bool ok, const QString& summary);

private slots:
    void onPlannerCompleted(const QString& text);
    void onPlannerFailed(const QString& error);
    void onPlannerStopped();

private:
    void handlePlanReply(const QString& text);
    void handleReplanReply(const QString& text);
    bool expandCapabilities(const QStringList& need);
    void appendRepairedTail(const QVector<AIAgent::Step>& steps);

private:
    explicit AIAgentManager(QObject* parent = nullptr);
    ~AIAgentManager() override;

    enum class Awaiting { None, Plan, Replan };

    void setState(AIAgent::State s);
    void ensurePlanner();
    void requestPlan(const QString& extraInstruction = QString());
    void requestReplan(int failedIndex);
    QString systemPrompt(const QStringList& capabilityIds) const;
    QString sceneContext() const;
    void adoptPlan(AIAgent::Plan plan);
    void executeNext();
    void runStep(int index);
    void handleStepFailure(int index, const AIAgent::Observation& ob);
    void openUndoGroupIfNeeded(const QString& tool);
    void closeUndoGroup();
    void finish(AIAgent::State terminal, const QString& plannerSummary = QString());
    void say(const QString& text);
    void sayTool(const QString& text);

    static AIAgentManager* s_instance;

    std::shared_ptr<AgentToolExecutor> m_executor;
    AgentPlannerBackend*  m_planner = nullptr;
    QUndoStack*           m_undoStack = nullptr;
    std::function<QString()> m_contextProvider;
    AICapabilityRegistry  m_registry;
    AIAgent::Limits       m_limits;

    AIAgent::State  m_state = AIAgent::State::Idle;
    Awaiting        m_awaiting = Awaiting::None;
    AIAgent::Plan   m_plan;
    QVector<AIAgent::Observation> m_observations;
    QStringList     m_docCapabilities;     // capabilities whose docs the planner has seen
    QHash<QString, int> m_failureCounts;   // step signature → failures
    int   m_currentStep = -1;
    int   m_pendingIndex = -1;
    QString m_pendingReason;
    int   m_replans = 0;
    int   m_plannerRetries = 0;
    int   m_replanFailedIndex = -1;
    bool  m_macroOpen = false;
    bool  m_cancelRequested = false;
    bool  m_trustedMode = false;
    QString m_lastSummary;
    QString m_lastError;

    struct Turn { QString request; QString outcome; QStringList objects; };
    QVector<Turn> m_history;
    QStringList   m_touchedObjects;   // entity/node/material names this task used or created
    QString conversationContext() const;
    void noteTouchedObjects(const AIAgent::Step& step, const AIAgent::Observation& ob);
    void trace(const QString& kind, const QString& text);
    bool m_traceFresh = false;
};

#endif // AIAGENTMANAGER_H
