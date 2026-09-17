// Headless tests for the AI agent state machine (#1001). A scripted planner
// stands in for the LLM and a scripted executor for the MCP server, so the
// whole plan → execute → observe → replan → finish loop runs without a
// model, Ogre, or a GUI. The custom test main owns the QApplication.
#include <gtest/gtest.h>

#include "AIAgentManager.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTimer>
#include <QUndoCommand>
#include <QUndoStack>

using namespace AIAgent;

namespace {

QJsonObject prop(const char* type, const char* desc, QJsonArray enumVals = {})
{
    QJsonObject p{{"type", type}, {"description", desc}};
    if (!enumVals.isEmpty()) p["enum"] = enumVals;
    return p;
}
QJsonObject tool(const char* name, const char* desc, QJsonObject props, QJsonArray required = {})
{
    QJsonObject schema{{"type", "object"}, {"properties", props}};
    if (!required.isEmpty()) schema["required"] = required;
    return {{"name", name}, {"description", desc}, {"inputSchema", schema}};
}
QJsonObject ok(const QString& text)
{
    return {{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}};
}
QJsonObject err(const QString& text)
{
    return {{"isError", true}, {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}};
}

/// Scripted MCP stand-in: per-tool queues of results (default: success),
/// a call log, and an optional undo stack it pushes a command onto for
/// every mutating call (so undo grouping is observable).
class FakeExecutor : public AgentToolExecutor
{
public:
    QJsonArray toolList() override
    {
        return {
            tool("get_scene_info", "Scene info.", {}),
            tool("create_primitive", "Create a primitive.", {{"type", prop("string", "kind", {"box", "sphere"})}, {"name", prop("string", "name")}}, {"type"}),
            tool("transform_mesh", "Transform.", {{"name", prop("string", "node")}, {"scale", prop("array", "xyz")}}, {"name"}),
            tool("apply_material", "Apply.", {{"mesh", prop("string", "m")}, {"material", prop("string", "mat")}}, {"mesh", "material"}),
            tool("auto_rig", "Rig.", {{"template", prop("string", "t")}}),
            tool("delete_entity", "Delete.", {{"entity_name", prop("string", "e")}}, {"entity_name"}),
            tool("export_mesh", "Export.", {{"output_path", prop("string", "p")}}, {"output_path"}),
        };
    }
    QJsonObject callTool(const QString& name, const QJsonObject& args) override
    {
        calls << name;
        callArgs << args;
        if (onCall) onCall(name);
        if (undoStack && !AICapabilityRegistry::isReadOnly(name))
            undoStack->push(new QUndoCommand(name));
        auto& q = scripted[name];
        if (!q.isEmpty()) return q.takeFirst();
        return ok(QStringLiteral("Created %1").arg(args.value("name").toString("thing")));
    }
    QStringList calls;
    QList<QJsonObject> callArgs;
    QHash<QString, QList<QJsonObject>> scripted;
    QUndoStack* undoStack = nullptr;
    std::function<void(const QString&)> onCall;
};

/// Scripted LLM: replies are dequeued in order and delivered asynchronously
/// (a queued call, like the real worker thread). Records every prompt.
class FakePlanner : public AgentPlannerBackend
{
public:
    using AgentPlannerBackend::AgentPlannerBackend;
    bool available() const override { return isAvailable; }
    void request(const QString& sys, const QString& user, int) override
    {
        systemPrompts << sys; userPrompts << user;
        if (replies.isEmpty()) { QTimer::singleShot(0, this, [this]() { emit failed("no scripted reply"); }); return; }
        const QString r = replies.takeFirst();
        QTimer::singleShot(0, this, [this, r]() { if (!stoppedFlag) emit completed(r); });
    }
    void stop() override { stoppedFlag = true; QTimer::singleShot(0, this, [this]() { emit stopped(); }); }
    bool isAvailable = true;
    bool stoppedFlag = false;
    QStringList replies, systemPrompts, userPrompts;
};

QString planJson(const QList<QPair<QString, QJsonObject>>& steps, const QString& title = "test plan")
{
    QJsonArray arr;
    for (const auto& s : steps) arr.append(QJsonObject{{"tool", s.first}, {"arguments", s.second}, {"why", "because"}});
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"title", title}, {"steps", arr}}).toJson(QJsonDocument::Compact));
}
QString replanJson(const QList<QPair<QString, QJsonObject>>& steps, bool done = false, const QString& summary = {})
{
    QJsonArray arr;
    for (const auto& s : steps) arr.append(QJsonObject{{"tool", s.first}, {"arguments", s.second}});
    QJsonObject o{{"steps", arr}, {"done", done}};
    if (!summary.isEmpty()) o["summary"] = summary;
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

/// Pump the event loop until the manager reaches a terminal state or the
/// given one (confirmation waits), with a hard timeout.
bool pumpUntil(AIAgentManager* m, std::function<bool()> pred, int ms = 3000)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (pred()) return true;
    }
    return pred();
}
bool pumpToEnd(AIAgentManager* m) { return pumpUntil(m, [m]() { return isTerminal(m->state()); }); }

struct AgentFixture : public ::testing::Test {
    AIAgentManager* m = nullptr;
    std::shared_ptr<FakeExecutor> exec;
    FakePlanner* planner = nullptr;   // owned by the manager
    QUndoStack undo;
    QStringList transcript;

    void SetUp() override
    {
        AIAgentManager::kill();
        m = AIAgentManager::instance();
        exec = std::make_shared<FakeExecutor>();
        exec->undoStack = &undo;
        m->setExecutor(exec);
        planner = new FakePlanner();
        m->setPlanner(planner);
        m->setUndoStack(&undo);
        m->setTrustedMode(false);
        QObject::connect(m, &AIAgentManager::chatMessage, [this](const QString& role, const QString& text, bool) {
            transcript << role + ": " + text;
        });
    }
    void TearDown() override { AIAgentManager::kill(); }
};

} // namespace

// ---------------------------------------------------------------------------

TEST(AIAgentManagerParse, ExtractsTheFirstBalancedObjectEvenWithoutTheOpeningBrace)
{
    EXPECT_EQ(AIAgentManager::extractJsonObject("Sure! {\"a\": {\"b\": 1}} trailing"), "{\"a\": {\"b\": 1}}");
    EXPECT_EQ(AIAgentManager::extractJsonObject("\"title\": \"x\", \"steps\": []}"), "{\"title\": \"x\", \"steps\": []}");
    EXPECT_EQ(AIAgentManager::extractJsonObject("{\"s\": \"a } inside a string\"}"), "{\"s\": \"a } inside a string\"}");
    EXPECT_TRUE(AIAgentManager::extractJsonObject("no json here").isEmpty());
}

TEST(AIAgentManagerParse, PlanReplyVariants)
{
    Plan p; QStringList need; QString answer, err;
    ASSERT_TRUE(AIAgentManager::parsePlanReply(planJson({{"get_scene_info", {}}, {"create_primitive", {{"type", "box"}}}}), &p, &need, &answer, &err));
    EXPECT_EQ(p.steps.size(), 2);
    EXPECT_EQ(p.steps[1].arguments["type"].toString(), "box");

    ASSERT_TRUE(AIAgentManager::parsePlanReply("{\"need_capabilities\": [\"rigging\", \"uv\"]}", &p, &need, &answer, &err));
    EXPECT_EQ(need, QStringList({"rigging", "uv"}));

    ASSERT_TRUE(AIAgentManager::parsePlanReply("{\"summary\": \"There are 2 entities.\"}", &p, &need, &answer, &err));
    EXPECT_EQ(answer, "There are 2 entities.");

    EXPECT_FALSE(AIAgentManager::parsePlanReply("I think we should...", &p, &need, &answer, &err));
    EXPECT_FALSE(AIAgentManager::parsePlanReply("{\"title\": \"t\", \"steps\": [{\"arguments\": {}}]}", &p, &need, &answer, &err)) << "a step without a tool";
    // legacy v1 field names are accepted
    ASSERT_TRUE(AIAgentManager::parsePlanReply("{\"steps\": [{\"command\": \"get_scene_info\", \"args\": {}}]}", &p, &need, &answer, &err));
    EXPECT_EQ(p.steps[0].tool, "get_scene_info");
}

TEST_F(AgentFixture, FiveDependentStepsRunInOrderWithObservableStateAndOneUndoGroup)
{
    planner->replies << planJson({
        {"get_scene_info", {}},
        {"create_primitive", {{"type", "box"}, {"name", "Crate"}}},
        {"transform_mesh", {{"name", "Crate"}, {"scale", QJsonArray{2, 2, 2}}}},
        {"apply_material", {{"mesh", "Crate"}, {"material", "Wood"}}},
        {"auto_rig", {{"template", "generic"}}},
    }, "build a crate");
    exec->scripted["get_scene_info"] << ok("Scene Information:\n- Scene Nodes: 1\n- Entities: 1\n  - Floor (material: BaseWhite)");
    exec->scripted["auto_rig"] << ok("{\"applied\":true,\"boneCount\":7,\"template\":\"generic\",\"skinned\":false}");

    QSignalSpy stateSpy(m, &AIAgentManager::stateChanged);
    ASSERT_TRUE(m->startTask("build a crate and rig it"));
    EXPECT_EQ(m->state(), State::Planning);
    ASSERT_TRUE(pumpToEnd(m));

    EXPECT_EQ(m->state(), State::Completed);
    EXPECT_EQ(exec->calls, QStringList({"get_scene_info", "create_primitive", "transform_mesh", "apply_material", "auto_rig"}));
    ASSERT_EQ(m->plan().steps.size(), 5);
    for (const Step& s : m->plan().steps) EXPECT_EQ(s.status, Step::Succeeded);
    EXPECT_EQ(m->observations().size(), 5);
    EXPECT_DOUBLE_EQ(m->observations()[0].facts["entities"].toDouble(), 1.0) << "facts parsed from the tool text";
    EXPECT_EQ(m->observations()[4].facts["boneCount"].toInt(), 7) << "facts lifted from JSON results";
    EXPECT_EQ(m->plan().title, "AI: build a crate");
    // ONE undo group named after the task, covering the 4 mutating steps.
    EXPECT_EQ(undo.count(), 1);
    EXPECT_EQ(undo.text(0), "AI: build a crate");
    EXPECT_TRUE(m->lastSummary().startsWith("Done — 5 of 5 steps succeeded.")) << m->lastSummary().toStdString();
    EXPECT_GT(stateSpy.count(), 4) << "state transitions are observable";
    // transcript: the plan card, one line per tool, the summary
    EXPECT_TRUE(transcript.first().startsWith("plan: Plan — build a crate"));
    EXPECT_EQ(transcript.filter(QRegularExpression("^tool: ")).size(), 5);
    // the planner saw the compact capability index, not 170 tool docs
    EXPECT_TRUE(planner->systemPrompts.first().contains("Capabilities (groups of tools):"));
    EXPECT_TRUE(planner->systemPrompts.first().contains("- auto_rig:")) << "keyword routing exposed rigging docs";
}

TEST_F(AgentFixture, RecoverableErrorIsRetriedOnceThenSucceeds)
{
    planner->replies << planJson({{"create_primitive", {{"type", "box"}, {"name", "A"}}}, {"transform_mesh", {{"name", "A"}}}});
    exec->scripted["transform_mesh"] << err("Error: node 'A' busy") << ok("Transformed A");
    ASSERT_TRUE(m->startTask("make a box and move it"));
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(m->state(), State::Completed);
    EXPECT_EQ(exec->calls, QStringList({"create_primitive", "transform_mesh", "transform_mesh"}));
    EXPECT_EQ(m->plan().steps[1].attempts, 2);
    EXPECT_EQ(m->replanCount(), 0) << "a retry is not a replan";
}

TEST_F(AgentFixture, PersistentFailureTriggersOneReplanAndTheRepairedTailSucceeds)
{
    planner->replies << planJson({{"create_primitive", {{"type", "box"}, {"name", "A"}}}, {"apply_material", {{"mesh", "A"}, {"material", "Gold"}}}});
    // Replan: the model realises Gold does not exist and applies Wood instead.
    planner->replies << replanJson({{"apply_material", {{"mesh", "A"}, {"material", "Wood"}}}});
    exec->scripted["apply_material"] << err("Error: material 'Gold' not found") << err("Error: material 'Gold' not found") << ok("Applied Wood to A");
    ASSERT_TRUE(m->startTask("golden box"));
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(m->state(), State::Completed) << m->lastSummary().toStdString();
    EXPECT_EQ(m->replanCount(), 1);
    EXPECT_EQ(exec->calls.size(), 4);
    EXPECT_EQ(exec->callArgs.last()["material"].toString(), "Wood");
    // the replan prompt carried the structured observations, not raw dumps
    ASSERT_EQ(planner->userPrompts.size(), 2);
    EXPECT_TRUE(planner->userPrompts[1].contains("\"status\":\"error\""));
    EXPECT_TRUE(planner->userPrompts[1].contains("Step 2 (apply_material) failed"));
    // old pending step marked skipped, repaired step appended
    ASSERT_EQ(m->plan().steps.size(), 3);
    EXPECT_EQ(m->plan().steps[1].status, Step::Failed);
    EXPECT_EQ(m->plan().steps[2].status, Step::Succeeded);
}

TEST_F(AgentFixture, RepeatedIdenticalFailingActionIsDetectedAndStopsTheTask)
{
    Limits lim; lim.maxAttemptsPerStep = 1; lim.maxReplans = 5; lim.maxRepeatedFailures = 2;
    m->setLimits(lim);
    planner->replies << planJson({{"apply_material", {{"mesh", "A"}, {"material", "Gold"}}}});
    // The model stubbornly re-issues the exact same call.
    planner->replies << replanJson({{"apply_material", {{"mesh", "A"}, {"material", "Gold"}}}});
    planner->replies << replanJson({{"apply_material", {{"mesh", "A"}, {"material", "Gold"}}}});
    exec->scripted["apply_material"] << err("Error: no") << err("Error: no") << err("Error: no") << err("Error: no");
    ASSERT_TRUE(m->startTask("gold"));
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(m->state(), State::Failed);
    EXPECT_EQ(exec->calls.size(), 2) << "stopped after the second identical failure, not after 5 replans";
    EXPECT_TRUE(m->lastError().contains("same action failed 2 times")) << m->lastError().toStdString();
}

TEST_F(AgentFixture, ReplanBudgetExhaustedFailsCleanly)
{
    Limits lim; lim.maxAttemptsPerStep = 1; lim.maxReplans = 1; lim.maxRepeatedFailures = 10;
    m->setLimits(lim);
    planner->replies << planJson({{"apply_material", {{"mesh", "A"}, {"material", "X"}}}});
    planner->replies << replanJson({{"apply_material", {{"mesh", "A"}, {"material", "Y"}}}});
    exec->scripted["apply_material"] << err("Error: no X") << err("Error: no Y");
    ASSERT_TRUE(m->startTask("paint"));
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(m->state(), State::Failed);
    EXPECT_TRUE(m->lastError().contains("replan budget")) << m->lastError().toStdString();
    // The failing calls still pushed commands (the fake mimics tools that
    // partially mutate before erroring), so ONE group exists — and it must be
    // CLOSED: a command pushed after the task lands as a new entry, not
    // inside a dangling macro.
    EXPECT_EQ(undo.count(), 1);
    undo.push(new QUndoCommand("later"));
    EXPECT_EQ(undo.count(), 2) << "the undo group was left open";
    EXPECT_EQ(undo.text(1), "later");
}

TEST_F(AgentFixture, CancellationStopsBetweenStepsAndDuringPlanning)
{
    planner->replies << planJson({{"create_primitive", {{"type", "box"}, {"name", "A"}}}, {"create_primitive", {{"type", "box"}, {"name", "B"}}}, {"create_primitive", {{"type", "box"}, {"name", "C"}}}});
    exec->onCall = [this](const QString&) { if (exec->calls.size() == 1) m->cancel(); };
    ASSERT_TRUE(m->startTask("three boxes"));
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(m->state(), State::Cancelled);
    EXPECT_EQ(exec->calls.size(), 1) << "no further tool calls after cancel";
    EXPECT_TRUE(m->lastSummary().startsWith("Cancelled after")) << m->lastSummary().toStdString();
    EXPECT_FALSE(m->busy());

    // cancel while the planner is thinking: the reply that arrives later is ignored
    AIAgentManager::kill(); SetUp();
    planner->replies << planJson({{"create_primitive", {{"type", "box"}}}});
    ASSERT_TRUE(m->startTask("a box"));
    m->cancel();
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(m->state(), State::Cancelled);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    EXPECT_TRUE(exec->calls.isEmpty());
}

TEST_F(AgentFixture, InvalidArgumentsNeverReachTheToolAndTriggerAReplan)
{
    planner->replies << planJson({{"create_primitive", {{"type", "pyramid"}}}});   // not in the enum
    planner->replies << replanJson({{"create_primitive", {{"type", "box"}}}});
    ASSERT_TRUE(m->startTask("a pyramid"));
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(m->state(), State::Completed);
    EXPECT_EQ(exec->calls, QStringList({"create_primitive"})) << "the invalid call was never executed";
    EXPECT_EQ(m->observations().first().status, "invalid_arguments");
    EXPECT_TRUE(planner->userPrompts[1].contains("not one of box|sphere"));
}

TEST_F(AgentFixture, UnknownToolFromThePlannerIsRejectedWithoutExecution)
{
    Limits lim; lim.maxReplans = 0; m->setLimits(lim);
    planner->replies << planJson({{"make_it_pretty", {}}});
    ASSERT_TRUE(m->startTask("pretty"));
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(m->state(), State::Failed);
    EXPECT_TRUE(exec->calls.isEmpty());
}

TEST_F(AgentFixture, DestructiveStepWaitsForConfirmationUnlessTrusted)
{
    planner->replies << planJson({{"delete_entity", {{"entity_name", "Cube"}}}, {"create_primitive", {{"type", "box"}}}});
    ASSERT_TRUE(m->startTask("replace the cube"));
    ASSERT_TRUE(pumpUntil(m, [this]() { return m->state() == State::AwaitingConfirmation; }));
    EXPECT_TRUE(exec->calls.isEmpty()) << "nothing runs before the answer";
    EXPECT_TRUE(m->pendingConfirmation().contains("deletes 'Cube'")) << m->pendingConfirmation().toStdString();

    m->confirmPendingStep(false);   // deny → skipped, the rest continues
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(exec->calls, QStringList({"create_primitive"}));
    EXPECT_EQ(m->plan().steps[0].status, Step::Skipped);
    EXPECT_EQ(m->observations().first().status, "denied");
    EXPECT_EQ(m->state(), State::Completed);

    // approve path
    AIAgentManager::kill(); SetUp();
    planner->replies << planJson({{"delete_entity", {{"entity_name", "Cube"}}}});
    ASSERT_TRUE(m->startTask("delete the cube"));
    ASSERT_TRUE(pumpUntil(m, [this]() { return m->state() == State::AwaitingConfirmation; }));
    m->confirmPendingStep(true);
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(exec->calls, QStringList({"delete_entity"}));
    EXPECT_EQ(m->state(), State::Completed);

    // trusted mode: no pause at all
    AIAgentManager::kill(); SetUp();
    m->setTrustedMode(true);
    planner->replies << planJson({{"delete_entity", {{"entity_name", "Cube"}}}});
    ASSERT_TRUE(m->startTask("delete the cube"));
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(m->state(), State::Completed);
    EXPECT_EQ(exec->calls, QStringList({"delete_entity"}));
    m->setTrustedMode(false);
}

TEST_F(AgentFixture, PlannerCanAskForMoreCapabilitiesBeforePlanning)
{
    // "make it shiny" routes to materials/scene; the model asks for rigging docs first.
    planner->replies << "{\"need_capabilities\": [\"rigging\"]}";
    planner->replies << planJson({{"auto_rig", {{"template", "humanoid"}}}});
    ASSERT_TRUE(m->startTask("make it shiny"));
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(m->state(), State::Completed);
    ASSERT_EQ(planner->systemPrompts.size(), 2);
    EXPECT_FALSE(planner->systemPrompts[0].contains("- auto_rig:")) << "first prompt: only routed capabilities";
    EXPECT_TRUE(planner->systemPrompts[1].contains("- auto_rig:")) << "second prompt: expanded on request";
    EXPECT_EQ(exec->calls, QStringList({"auto_rig"}));
}

TEST_F(AgentFixture, QuestionIsAnsweredWithoutRunningTools)
{
    planner->replies << "{\"summary\": \"The scene holds one entity, Floor.\"}";
    ASSERT_TRUE(m->startTask("what is in the scene?"));
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(m->state(), State::Completed);
    EXPECT_TRUE(exec->calls.isEmpty());
    EXPECT_EQ(m->lastSummary(), "The scene holds one entity, Floor.");
    EXPECT_EQ(undo.count(), 0);
}

TEST_F(AgentFixture, MalformedPlannerOutputIsRetriedThenFails)
{
    planner->replies << "I would love to help!" << "still no json" << "nope";
    ASSERT_TRUE(m->startTask("do things"));
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(m->state(), State::Failed);
    EXPECT_EQ(planner->userPrompts.size(), 3) << "1 attempt + maxPlannerRetries";
    EXPECT_TRUE(planner->userPrompts[1].contains("previous reply was not valid"));
    EXPECT_TRUE(exec->calls.isEmpty());
}

TEST_F(AgentFixture, RefusesToStartWithoutAModelOrWhileBusy)
{
    planner->isAvailable = false;
    EXPECT_FALSE(m->startTask("anything"));
    EXPECT_EQ(m->state(), State::Idle);
    EXPECT_TRUE(transcript.last().contains("No AI model is loaded"));

    planner->isAvailable = true;
    planner->replies << planJson({{"get_scene_info", {}}});
    ASSERT_TRUE(m->startTask("scene?"));
    EXPECT_FALSE(m->startTask("another")) << "busy";
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_EQ(m->state(), State::Completed);
    EXPECT_EQ(undo.count(), 0) << "read-only steps never open an undo group";
}

TEST_F(AgentFixture, SceneContextIsInjectedIntoEveryPlannerPrompt)
{
    m->setContextProvider([]() { return QStringLiteral("Entities: Floor, Crate"); });
    planner->replies << planJson({{"get_scene_info", {}}});
    ASSERT_TRUE(m->startTask("x"));
    ASSERT_TRUE(pumpToEnd(m));
    EXPECT_TRUE(planner->systemPrompts.first().contains("Scene state:\nEntities: Floor, Crate"));
}

TEST(AIAgentObservation, ParsesFactsArtifactsAndErrorsFromToolText)
{
    Observation ob = observationFromToolResult(0, "get_mesh_info",
        ok("Mesh Information for Wolf:\n- Vertices: 12,345\n- Triangles: 20000\n- Bones: 24\nHas skeleton: yes"));
    EXPECT_EQ(ob.status, "success");
    EXPECT_DOUBLE_EQ(ob.facts["vertices"].toDouble(), 12345.0);
    EXPECT_DOUBLE_EQ(ob.facts["triangles"].toDouble(), 20000.0);
    EXPECT_DOUBLE_EQ(ob.facts["bones"].toDouble(), 24.0);
    EXPECT_TRUE(ob.facts["hasSkeleton"].toBool());

    ob = observationFromToolResult(1, "export_mesh", ok("Exported mesh to /tmp/out/wolf.glb (1 skin)"));
    EXPECT_EQ(ob.artifacts, QStringList({"/tmp/out/wolf.glb"}));

    ob = observationFromToolResult(2, "apply_material", err("Error: material 'Gold' not found\nAvailable: Wood"));
    EXPECT_EQ(ob.status, "error");
    EXPECT_EQ(ob.error, "Error: material 'Gold' not found");
    EXPECT_TRUE(ob.raw.contains("Available: Wood")) << "raw is kept for the transcript";
    EXPECT_FALSE(ob.toPromptLine().contains("Available: Wood")) << "but never copied into the prompt";

    ob = observationFromToolResult(3, "auto_rig", ok("{\"applied\":true,\"boneCount\":19,\"fallbackReason\":\"UniRig unavailable\"}"));
    EXPECT_EQ(ob.facts["boneCount"].toInt(), 19);
    ASSERT_EQ(ob.warnings.size(), 1);
    EXPECT_TRUE(ob.warnings.first().startsWith("fallback:"));
}

TEST(AIAgentManagerModels, RecommendedModelCheckIsCaseInsensitiveAndSizeAware)
{
    EXPECT_TRUE(AIAgentManager::modelIsRecommended("Qwen3-4B-Instruct-2507-Q4_K_M.gguf"));
    EXPECT_TRUE(AIAgentManager::modelIsRecommended("Qwen2.5-7B-Instruct-Q4_K_M.gguf"));
    EXPECT_TRUE(AIAgentManager::modelIsRecommended("google_gemma-3-12b-it-Q4_K_M.gguf"));
    EXPECT_TRUE(AIAgentManager::modelIsRecommended("Qwen3-30B-A3B-Instruct-2507-Q4_K_M.gguf"));
    EXPECT_FALSE(AIAgentManager::modelIsRecommended("gemma-3-1b-it-Q4_K_M.gguf"));
    EXPECT_FALSE(AIAgentManager::modelIsRecommended("qwen2.5-3b-instruct-q4_k_m.gguf"));
    EXPECT_FALSE(AIAgentManager::modelIsRecommended(""));
}
