#include "AIAgentManager.h"

#include "LLMManager.h"
#include "MCPServer.h"
#include "SentryReporter.h"
#include "UndoManager.h"

#include <QJsonDocument>
#include <QSettings>
#include <QTimer>
#include <QUndoStack>

using namespace AIAgent;

namespace {
constexpr const char* kTrustedModeKey = "ai/agentTrustedMode";
constexpr const char* kRecommendedModel = "Qwen3 4B Instruct 2507 Q4_K_M";

/// LLMManager-backed planner. Forwards LLMManager's generation signals only
/// while it has an outstanding request, so a material-generation run by
/// another component never lands in the agent.
class LlmPlannerBackend : public AgentPlannerBackend
{
public:
    explicit LlmPlannerBackend(QObject* parent) : AgentPlannerBackend(parent)
    {
        auto* llm = LLMManager::instance();
        connect(llm, &LLMManager::generationCompleted, this, [this](const QString& t) {
            if (!m_pending) return; m_pending = false; emit completed(t);
        });
        connect(llm, &LLMManager::generationError, this, [this](const QString& e) {
            if (!m_pending) return; m_pending = false; emit failed(e);
        });
        connect(llm, &LLMManager::generationStopped, this, [this]() {
            if (!m_pending) return; m_pending = false; emit stopped();
        });
    }
    bool available() const override { return LLMManager::instance()->isModelLoaded(); }
    QString modelName() const override { return LLMManager::instance()->currentModelName(); }
    void request(const QString& systemPrompt, const QString& userPrompt, int maxTokens) override
    {
        m_pending = true;
        LLMManager::instance()->generateText(systemPrompt, userPrompt, maxTokens);
    }
    void stop() override { if (m_pending) LLMManager::instance()->stopGeneration(); }
private:
    bool m_pending = false;
};
} // namespace

// ---------------------------------------------------------------------------
// McpToolExecutor

McpToolExecutor::McpToolExecutor(MCPServer* server) : m_server(server) {}
McpToolExecutor::~McpToolExecutor() = default;

QJsonArray McpToolExecutor::toolList()
{
    return m_server ? m_server->buildToolsList() : QJsonArray{};
}

QJsonObject McpToolExecutor::callTool(const QString& name, const QJsonObject& args)
{
    if (!m_server) return QJsonObject{{"isError", true},
        {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "Error: MCP server not available"}}}}};
    return m_server->callTool(name, args);
}

// ---------------------------------------------------------------------------

AIAgentManager* AIAgentManager::s_instance = nullptr;

AIAgentManager* AIAgentManager::instance()
{
    if (!s_instance) s_instance = new AIAgentManager();
    return s_instance;
}

AIAgentManager* AIAgentManager::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine); Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void AIAgentManager::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

AIAgentManager::AIAgentManager(QObject* parent) : QObject(parent)
{
    QSettings settings;
    m_trustedMode = settings.value(QLatin1String(kTrustedModeKey), false).toBool();
}

AIAgentManager::~AIAgentManager()
{
    closeUndoGroup();
}

void AIAgentManager::setExecutor(std::shared_ptr<AgentToolExecutor> executor)
{
    m_executor = std::move(executor);
    m_registry = AICapabilityRegistry(m_executor ? m_executor->toolList() : QJsonArray{});
}

void AIAgentManager::setPlanner(AgentPlannerBackend* planner)
{
    if (m_planner) { m_planner->disconnect(this); m_planner->deleteLater(); }
    m_planner = planner;
    if (!m_planner) return;
    m_planner->setParent(this);
    connect(m_planner, &AgentPlannerBackend::completed, this, &AIAgentManager::onPlannerCompleted);
    connect(m_planner, &AgentPlannerBackend::failed,    this, &AIAgentManager::onPlannerFailed);
    connect(m_planner, &AgentPlannerBackend::stopped,   this, &AIAgentManager::onPlannerStopped);
}

void AIAgentManager::ensurePlanner()
{
    if (!m_planner) setPlanner(new LlmPlannerBackend(this));
}

void AIAgentManager::setTrustedMode(bool on)
{
    if (m_trustedMode == on) return;
    m_trustedMode = on;
    QSettings settings;
    settings.setValue(QLatin1String(kTrustedModeKey), on);
    SentryReporter::addBreadcrumb("ai.agent.confirm", on ? "trusted mode ON" : "trusted mode OFF");
    emit trustedModeChanged();
}

QString AIAgentManager::recommendedModelName() const
{
    return QLatin1String(kRecommendedModel);
}

bool AIAgentManager::modelIsRecommended(const QString& modelName)
{
    // Models known to hold a multi-step JSON tool protocol together: the
    // Qwen Instruct line at 4B+ and anything 7B+/MoE. Substring match on the
    // file/name the user loaded (case-insensitive).
    static const QStringList markers = {
        "qwen3-4b-instruct", "qwen3-30b", "qwen2.5-7b", "qwen 2.5 7b", "qwen3 4b", "qwen3 30b",
        "7b", "8b", "12b", "14b", "27b", "30b", "32b", "70b",
    };
    const QString n = modelName.toLower();
    for (const QString& m : markers) if (n.contains(m)) return true;
    return false;
}

QVariantList AIAgentManager::planModel() const
{
    QVariantList out;
    for (int i = 0; i < m_plan.steps.size(); ++i) {
        const Step& s = m_plan.steps[i];
        out << QVariantMap{
            {"index", i}, {"tool", s.tool}, {"why", s.why},
            {"status", Step::statusName(s.status)}, {"error", s.error}, {"attempts", s.attempts},
        };
    }
    return out;
}

void AIAgentManager::setState(State s)
{
    if (m_state == s) return;
    m_state = s;
    emit stateChanged();
}

void AIAgentManager::say(const QString& text)     { emit chatMessage(QStringLiteral("assistant"), text, false); }
void AIAgentManager::sayTool(const QString& text) { emit chatMessage(QStringLiteral("tool"), text, true); }

// ---------------------------------------------------------------------------
// control

bool AIAgentManager::startTask(const QString& request)
{
    const QString goal = request.trimmed();
    if (goal.isEmpty()) return false;
    if (busy()) { say(QStringLiteral("A task is already running — cancel it first.")); return false; }
    if (!m_executor) { say(QStringLiteral("The tool server is not available.")); return false; }
    ensurePlanner();
    if (!m_planner->available()) {
        say(QStringLiteral("No AI model is loaded (AI → AI Model Settings). Recommended for multi-step tasks: %1.")
                .arg(recommendedModelName()));
        return false;
    }
    if (m_registry.isEmpty()) m_registry = AICapabilityRegistry(m_executor->toolList());

    m_plan = Plan{};
    m_plan.goal = goal;
    m_observations.clear();
    m_failureCounts.clear();
    m_docCapabilities = m_registry.routeByKeywords(goal);
    m_currentStep = -1; m_pendingIndex = -1; m_pendingReason.clear();
    m_replans = 0; m_plannerRetries = 0; m_replanFailedIndex = -1;
    m_cancelRequested = false; m_lastSummary.clear(); m_lastError.clear();
    emit planChanged(); emit confirmationChanged();

    SentryReporter::addBreadcrumb("ai.agent.plan", QStringLiteral("task started (%1 routed capabilities)").arg(m_docCapabilities.size()));
    requestPlan();
    return true;
}

void AIAgentManager::cancel()
{
    if (!busy()) return;
    m_cancelRequested = true;
    SentryReporter::addBreadcrumb("ai.agent.cancel", QStringLiteral("cancelled in state %1").arg(stateName()));
    if (m_state == State::Planning || m_state == State::Replanning) {
        m_awaiting = Awaiting::None;
        if (m_planner) m_planner->stop();
    }
    if (m_state == State::AwaitingConfirmation && m_pendingIndex >= 0 && m_pendingIndex < m_plan.steps.size()) {
        m_plan.steps[m_pendingIndex].status = Step::Skipped;
        m_pendingIndex = -1; m_pendingReason.clear(); emit confirmationChanged();
    }
    finish(State::Cancelled);
}

void AIAgentManager::confirmPendingStep(bool approve, bool alwaysAllow)
{
    if (m_state != State::AwaitingConfirmation || m_pendingIndex < 0) return;
    const int idx = m_pendingIndex;
    m_pendingIndex = -1;
    const QString reason = m_pendingReason;
    m_pendingReason.clear();
    emit confirmationChanged();
    if (alwaysAllow) setTrustedMode(true);
    if (approve) {
        SentryReporter::addBreadcrumb("ai.agent.confirm", QStringLiteral("approved: %1").arg(reason));
        runStep(idx);
        return;
    }
    SentryReporter::addBreadcrumb("ai.agent.confirm", QStringLiteral("denied: %1").arg(reason));
    Step& s = m_plan.steps[idx];
    s.status = Step::Skipped;
    s.error = QStringLiteral("denied by user");
    Observation ob; ob.stepIndex = idx; ob.tool = s.tool; ob.status = QStringLiteral("denied"); ob.error = s.error;
    m_observations << ob;
    sayTool(QStringLiteral("[%1] skipped — %2").arg(s.tool, reason));
    emit planChanged();
    emit stepFinished(idx, false);
    QTimer::singleShot(0, this, &AIAgentManager::executeNext);
}

// ---------------------------------------------------------------------------
// planning

QString AIAgentManager::sceneContext() const
{
    return m_contextProvider ? m_contextProvider() : QString();
}

QString AIAgentManager::systemPrompt(const QStringList& capabilityIds) const
{
    QString s = QStringLiteral(
        "You are the planner of QtMeshEditor's AI agent. You control a 3D mesh editor by choosing tool calls.\n"
        "Reply with ONE JSON object and nothing else.\n\n"
        "Capabilities (groups of tools):\n%1\n"
        "Tools you may use now (capabilities: %2):\n%3\n"
        "If the task needs a capability whose tools are NOT listed above, reply exactly:\n"
        "{\"need_capabilities\": [\"capability_id\"]}\n"
        "If the task is a question you can answer from the scene state, reply:\n"
        "{\"summary\": \"the answer\"}\n"
        "Otherwise reply with the plan:\n"
        "{\"title\": \"short task title\", \"steps\": [{\"tool\": \"tool_name\", \"arguments\": {\"param\": \"value\"}, \"why\": \"few words\"}]}\n\n"
        "Rules:\n"
        "1. Only tool names and parameter names listed above. Never invent either.\n"
        "2. 1 to %4 steps, in execution order, minimal — only what the user asked for.\n"
        "3. Up/down is +Y/-Y; forward is -Z; ground is Y=0. 'twice as large' = scale [2,2,2].\n"
        "4. Use exact object/material names from the scene state. Call get_scene_info first only when a needed name is unknown.\n"
        "5. Never repeat a step.\n")
        .arg(m_registry.promptIndex(), capabilityIds.join(", "), m_registry.promptToolsFor(capabilityIds))
        .arg(m_limits.maxSteps);
    const QString ctx = sceneContext();
    if (!ctx.isEmpty()) s += QStringLiteral("\nScene state:\n%1\n").arg(ctx);
    return s;
}

void AIAgentManager::requestPlan(const QString& extraInstruction)
{
    setState(State::Planning);
    m_awaiting = Awaiting::Plan;
    QString user = QStringLiteral("Task: %1\n").arg(m_plan.goal);
    if (!extraInstruction.isEmpty()) user += extraInstruction + '\n';
    user += QStringLiteral("JSON:");
    m_planner->request(systemPrompt(m_docCapabilities), user, 700);
}

void AIAgentManager::requestReplan(int failedIndex)
{
    setState(State::Replanning);
    m_awaiting = Awaiting::Replan;
    m_replanFailedIndex = failedIndex;
    ++m_replans;
    SentryReporter::addBreadcrumb("ai.agent.replan", QStringLiteral("replan %1 after step %2").arg(m_replans).arg(failedIndex + 1));

    QStringList obsLines;
    for (const Observation& ob : m_observations) obsLines << ob.toPromptLine();
    QJsonArray planArr;
    for (const Step& s : m_plan.steps) planArr.append(s.toJson());
    const Step& failed = m_plan.steps[failedIndex];
    QString user = QStringLiteral(
        "Original task: %1\n"
        "Plan so far: %2\n"
        "Observations:\n%3\n"
        "Step %4 (%5) failed: %6\n"
        "Reply with ONE JSON object: {\"steps\": [remaining steps to run now, fixed], \"done\": false}\n"
        "or {\"done\": true, \"summary\": \"what was achieved / why it cannot be completed\"}.\n"
        "Do not repeat steps that already succeeded. Do not repeat the failing call unchanged.\n"
        "JSON:")
        .arg(m_plan.goal, QString::fromUtf8(QJsonDocument(planArr).toJson(QJsonDocument::Compact)),
             obsLines.join('\n'))
        .arg(failedIndex + 1).arg(failed.tool, failed.error.left(300));
    m_planner->request(systemPrompt(m_docCapabilities), user, 700);
}

QString AIAgentManager::extractJsonObject(const QString& text)
{
    // First balanced {...}; tolerate a missing opening brace (models primed
    // with "{" sometimes omit it) by trying the prefixed variant second.
    auto balanced = [](const QString& t) -> QString {
        const int start = t.indexOf('{');
        if (start < 0) return {};
        int depth = 0; bool inStr = false; bool esc = false;
        for (int i = start; i < t.size(); ++i) {
            const QChar c = t[i];
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') inStr = true;
            else if (c == '{') ++depth;
            else if (c == '}') { if (--depth == 0) return t.mid(start, i - start + 1); }
        }
        return {};
    };
    QString block = balanced(text);
    if (block.isEmpty()) block = balanced('{' + text.trimmed());
    return block;
}

bool AIAgentManager::parsePlanReply(const QString& text, Plan* out, QStringList* needCapabilities,
                                    QString* answer, QString* error)
{
    const QString block = extractJsonObject(text);
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &perr);
    if (block.isEmpty() || perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("reply is not a JSON object");
        return false;
    }
    const QJsonObject o = doc.object();
    if (o.contains("need_capabilities")) {
        QStringList ids;
        for (const QJsonValue& v : o["need_capabilities"].toArray()) ids << v.toString();
        if (ids.isEmpty() && o["need_capabilities"].isString()) ids << o["need_capabilities"].toString();
        if (needCapabilities) *needCapabilities = ids;
        return true;
    }
    const QJsonArray steps = o["steps"].toArray();
    if (steps.isEmpty()) {
        const QString summary = o["summary"].toString().isEmpty() ? o["response"].toString() : o["summary"].toString();
        if (summary.isEmpty()) { if (error) *error = QStringLiteral("plan has no steps"); return false; }
        if (answer) *answer = summary;
        return true;
    }
    Plan p;
    p.title = o["title"].toString().simplified();
    for (const QJsonValue& v : steps) {
        const QJsonObject so = v.toObject();
        Step s;
        s.tool = so["tool"].toString().isEmpty() ? so["command"].toString() : so["tool"].toString();
        s.tool = s.tool.trimmed();
        s.arguments = so["arguments"].toObject();
        if (s.arguments.isEmpty()) s.arguments = so["args"].toObject();
        s.why = so["why"].toString().simplified();
        if (s.tool.isEmpty()) { if (error) *error = QStringLiteral("a step has no tool name"); return false; }
        p.steps.push_back(s);
    }
    if (out) *out = p;
    return true;
}

bool AIAgentManager::parseReplanReply(const QString& text, QVector<Step>* steps, bool* done,
                                      QString* summary, QString* error)
{
    const QString block = extractJsonObject(text);
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &perr);
    if (block.isEmpty() || perr.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("reply is not a JSON object");
        return false;
    }
    const QJsonObject o = doc.object();
    const bool isDone = o["done"].toBool(false);
    QVector<Step> out;
    for (const QJsonValue& v : o["steps"].toArray()) {
        const QJsonObject so = v.toObject();
        Step s;
        s.tool = (so["tool"].toString().isEmpty() ? so["command"].toString() : so["tool"].toString()).trimmed();
        s.arguments = so["arguments"].toObject();
        if (s.arguments.isEmpty()) s.arguments = so["args"].toObject();
        s.why = so["why"].toString().simplified();
        if (!s.tool.isEmpty()) out.push_back(s);
    }
    if (!isDone && out.isEmpty()) { if (error) *error = QStringLiteral("replan has no steps and is not done"); return false; }
    if (steps) *steps = out;
    if (done) *done = isDone;
    if (summary) *summary = o["summary"].toString();
    return true;
}

void AIAgentManager::onPlannerCompleted(const QString& text)
{
    if (m_awaiting == Awaiting::None) return;
    if (m_cancelRequested) { m_awaiting = Awaiting::None; return; }
    const Awaiting what = m_awaiting;
    m_awaiting = Awaiting::None;

    if (what == Awaiting::Plan) {
        Plan plan; QStringList need; QString answer; QString err;
        if (!parsePlanReply(text, &plan, &need, &answer, &err)) {
            if (++m_plannerRetries <= m_limits.maxPlannerRetries) {
                SentryReporter::addBreadcrumb("ai.agent.plan", QStringLiteral("malformed plan, retry %1").arg(m_plannerRetries));
                requestPlan(QStringLiteral("Your previous reply was not valid: %1. Reply with the JSON object only.").arg(err));
                return;
            }
            m_lastError = QStringLiteral("the model could not produce a valid plan (%1)").arg(err);
            say(QStringLiteral("I could not turn that into a plan: %1.").arg(err));
            finish(State::Failed);
            return;
        }
        if (!need.isEmpty()) {
            // Dynamic discovery: the planner asked for docs it did not have.
            QStringList added;
            for (const QString& id : need)
                if (m_registry.capability(id) && !m_docCapabilities.contains(id)) { m_docCapabilities << id; added << id; }
            if (added.isEmpty() || ++m_plannerRetries > m_limits.maxPlannerRetries + 1) {
                m_lastError = QStringLiteral("the model asked for unknown capabilities: %1").arg(need.join(", "));
                say(QStringLiteral("I could not find tools for: %1.").arg(need.join(", ")));
                finish(State::Failed);
                return;
            }
            SentryReporter::addBreadcrumb("ai.agent.plan", QStringLiteral("expanded capabilities: %1").arg(added.join(", ")));
            requestPlan();
            return;
        }
        if (!answer.isEmpty()) {
            // A question answered from context — no tools needed.
            m_plan.title = QStringLiteral("AI: answer");
            finish(State::Completed, answer);
            return;
        }
        plan.goal = m_plan.goal;
        plan.capabilities = m_docCapabilities;
        if (plan.title.isEmpty()) plan.title = plan.goal.left(48);
        if (!plan.title.startsWith(QLatin1String("AI:"))) plan.title = QStringLiteral("AI: ") + plan.title;
        if (plan.steps.size() > m_limits.maxSteps) plan.steps.resize(m_limits.maxSteps);
        adoptPlan(std::move(plan));
        return;
    }

    // ---- replan ----
    QVector<Step> steps; bool done = false; QString summary; QString err;
    if (!parseReplanReply(text, &steps, &done, &summary, &err)) {
        if (++m_plannerRetries <= m_limits.maxPlannerRetries) {
            requestReplan(m_replanFailedIndex);
            --m_replans;   // a retry of the same replan round does not count
            return;
        }
        m_lastError = QStringLiteral("the model could not repair the plan (%1)").arg(err);
        finish(State::Failed);
        return;
    }
    if (done) { finish(steps.isEmpty() && m_plan.allDone() ? State::Completed : State::Failed, summary); return; }
    // Replace the remaining pending steps with the repaired tail.
    for (Step& s : m_plan.steps) if (s.status == Step::Pending) s.status = Step::Skipped;
    int room = m_limits.maxSteps - m_plan.steps.size();
    for (Step& s : steps) { if (room-- <= 0) break; m_plan.steps.push_back(s); }
    emit planChanged();
    say(QStringLiteral("Adjusted the plan: %1 new step(s).").arg(steps.size()));
    QTimer::singleShot(0, this, &AIAgentManager::executeNext);
}

void AIAgentManager::onPlannerFailed(const QString& error)
{
    if (m_awaiting == Awaiting::None) return;
    m_awaiting = Awaiting::None;
    m_lastError = QStringLiteral("planner error: %1").arg(error);
    say(QStringLiteral("The AI model failed: %1").arg(error));
    finish(State::Failed);
}

void AIAgentManager::onPlannerStopped()
{
    if (m_awaiting == Awaiting::None) return;
    m_awaiting = Awaiting::None;
    if (!AIAgent::isTerminal(m_state)) finish(State::Cancelled);
}

void AIAgentManager::adoptPlan(Plan plan)
{
    m_plan = std::move(plan);
    emit planChanged();
    QStringList lines{QStringLiteral("Plan — %1").arg(m_plan.title.mid(4))};
    for (int i = 0; i < m_plan.steps.size(); ++i) {
        const Step& s = m_plan.steps[i];
        lines << QStringLiteral("%1. %2%3").arg(i + 1).arg(s.tool, s.why.isEmpty() ? QString() : QStringLiteral(" — %1").arg(s.why));
    }
    emit chatMessage(QStringLiteral("plan"), lines.join('\n'), false);
    SentryReporter::addBreadcrumb("ai.agent.plan", QStringLiteral("%1 steps: %2").arg(m_plan.steps.size()).arg(m_plan.title));
    QTimer::singleShot(0, this, &AIAgentManager::executeNext);
}

// ---------------------------------------------------------------------------
// execution

void AIAgentManager::executeNext()
{
    if (AIAgent::isTerminal(m_state)) return;
    if (m_cancelRequested) { finish(State::Cancelled); return; }
    const int idx = m_plan.nextPendingIndex();
    if (idx < 0) {
        setState(State::Verifying);
        SentryReporter::addBreadcrumb("ai.agent.verify", QStringLiteral("all %1 steps done").arg(m_plan.steps.size()));
        finish(State::Completed);
        return;
    }
    if (idx >= m_limits.maxSteps) {
        m_lastError = QStringLiteral("step limit (%1) reached").arg(m_limits.maxSteps);
        finish(State::Failed);
        return;
    }
    Step& s = m_plan.steps[idx];
    m_currentStep = idx;

    // ---- constrained protocol: validate before the call ever reaches the server ----
    QJsonObject coerced; QString err; QStringList warnings;
    if (!m_registry.validateArguments(s.tool, s.arguments, &coerced, &err, &warnings)) {
        Observation ob; ob.stepIndex = idx; ob.tool = s.tool;
        ob.status = QStringLiteral("invalid_arguments"); ob.error = err;
        m_observations << ob;
        s.attempts++; s.status = Step::Failed; s.error = err;
        sayTool(QStringLiteral("[%1] rejected before running: %2").arg(s.tool, err));
        emit planChanged();
        emit stepFinished(idx, false);
        handleStepFailure(idx, ob);
        return;
    }
    s.arguments = coerced;

    // ---- safety rail: destructive steps need a confirmation unless trusted ----
    const QString reason = AICapabilityRegistry::destructiveReason(s.tool, s.arguments);
    if (!reason.isEmpty() && !m_trustedMode) {
        m_pendingIndex = idx;
        m_pendingReason = QStringLiteral("Step %1 (%2) %3").arg(idx + 1).arg(s.tool, reason);
        setState(State::AwaitingConfirmation);
        emit confirmationChanged();
        SentryReporter::addBreadcrumb("ai.agent.confirm", QStringLiteral("waiting: %1").arg(m_pendingReason));
        return;
    }
    runStep(idx);
}

void AIAgentManager::openUndoGroupIfNeeded(const QString& tool)
{
    if (m_macroOpen || AICapabilityRegistry::isReadOnly(tool)) return;
    QUndoStack* stack = m_undoStack ? m_undoStack : UndoManager::getSingleton()->stack();
    if (!stack) return;
    stack->beginMacro(m_plan.title);
    m_macroOpen = true;
}

void AIAgentManager::closeUndoGroup()
{
    if (!m_macroOpen) return;
    m_macroOpen = false;
    QUndoStack* stack = m_undoStack ? m_undoStack : UndoManager::getSingleton()->stack();
    if (stack) stack->endMacro();
}

void AIAgentManager::runStep(int idx)
{
    Step& s = m_plan.steps[idx];
    s.status = Step::Running;
    s.attempts++;
    setState(State::Executing);
    emit planChanged();
    SentryReporter::addBreadcrumb("ai.agent.step", QStringLiteral("%1/%2 %3 (attempt %4)").arg(idx + 1).arg(m_plan.steps.size()).arg(s.tool).arg(s.attempts));

    openUndoGroupIfNeeded(s.tool);
    const QJsonObject result = m_executor->callTool(s.tool, s.arguments);

    // Cancel may have been requested from inside the tool call (GUI event
    // processing) — honour it before observing.
    setState(State::Observing);
    Observation ob = observationFromToolResult(idx, s.tool, result);
    m_observations << ob;

    if (ob.status == QLatin1String("success")) {
        s.status = Step::Succeeded;
        s.error.clear();
        QString line = ob.raw.section('\n', 0, 0).trimmed();
        if (line.size() > 160) line = line.left(157) + QLatin1String("...");
        sayTool(QStringLiteral("[%1] %2").arg(s.tool, line.isEmpty() ? QStringLiteral("ok") : line));
        emit planChanged();
        emit stepFinished(idx, true);
        QTimer::singleShot(0, this, &AIAgentManager::executeNext);
        return;
    }

    s.error = ob.error;
    sayTool(QStringLiteral("[%1] failed: %2").arg(s.tool, ob.error.left(200)));
    SentryReporter::addBreadcrumb("ai.agent.step", QStringLiteral("%1 failed: %2").arg(s.tool, ob.error.left(120)), "error");

    const bool fatal = ob.error.contains(QLatin1String("Unknown tool"), Qt::CaseInsensitive)
                    || ob.error.contains(QLatin1String("could not be initialized"), Qt::CaseInsensitive);
    if (!fatal && s.attempts < m_limits.maxAttemptsPerStep) {
        // Recoverable: try the same call once more (transient failures).
        s.status = Step::Pending;
        SentryReporter::addBreadcrumb("ai.agent.retry", QStringLiteral("%1 retry").arg(s.tool));
        emit planChanged();
        QTimer::singleShot(0, this, &AIAgentManager::executeNext);
        return;
    }
    s.status = Step::Failed;
    emit planChanged();
    emit stepFinished(idx, false);
    handleStepFailure(idx, ob);
}

void AIAgentManager::handleStepFailure(int idx, const Observation& ob)
{
    Q_UNUSED(ob);
    const Step& s = m_plan.steps[idx];
    const int n = ++m_failureCounts[s.signature()];
    if (n >= m_limits.maxRepeatedFailures) {
        m_lastError = QStringLiteral("the same action failed %1 times: %2 — %3").arg(n).arg(s.tool, s.error.left(160));
        say(QStringLiteral("Stopping: %1").arg(m_lastError));
        finish(State::Failed);
        return;
    }
    if (m_cancelRequested) { finish(State::Cancelled); return; }
    if (m_replans >= m_limits.maxReplans) {
        m_lastError = QStringLiteral("step %1 (%2) failed and the replan budget is spent: %3").arg(idx + 1).arg(s.tool, s.error.left(160));
        finish(State::Failed);
        return;
    }
    m_plannerRetries = 0;
    requestReplan(idx);
}

void AIAgentManager::finish(State terminal, const QString& plannerSummary)
{
    closeUndoGroup();
    m_awaiting = Awaiting::None;
    m_currentStep = -1;
    QString summary;
    if (!m_plan.steps.isEmpty()) summary = summarize(m_plan, m_observations, terminal);
    if (!plannerSummary.isEmpty()) summary = summary.isEmpty() ? plannerSummary : plannerSummary + '\n' + summary;
    if (summary.isEmpty()) {
        switch (terminal) {
        case State::Cancelled: summary = QStringLiteral("Cancelled."); break;
        case State::Failed:    summary = m_lastError.isEmpty() ? QStringLiteral("The task could not be completed.") : m_lastError; break;
        default:               summary = QStringLiteral("Done."); break;
        }
    } else if (terminal == State::Failed && !m_lastError.isEmpty() && !summary.contains(m_lastError)) {
        summary += '\n' + m_lastError;
    }
    m_lastSummary = summary;
    setState(terminal);
    SentryReporter::addBreadcrumb(terminal == State::Completed ? "ai.agent.done" : (terminal == State::Cancelled ? "ai.agent.cancel" : "ai.agent.fail"),
                                  QStringLiteral("%1 steps, %2 replans").arg(m_plan.steps.size()).arg(m_replans),
                                  terminal == State::Failed ? "error" : "info");
    say(summary);
    emit planChanged();
    emit taskFinished(terminal == State::Completed, summary);
}
