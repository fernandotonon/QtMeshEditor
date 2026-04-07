#include "AIChatManager.h"
#include "LLMManager.h"
#include "MCPServer.h"
#include "SentryReporter.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <cstdio>
#include <QDateTime>

// ---- chat debug helpers ----
static void chatLog(const char* tag, const QString& text)
{
    qint64 ms = QDateTime::currentMSecsSinceEpoch() % 100000; // last 5 digits
    fprintf(stderr, "\n[CHAT/%s @%lld] %s\n", tag, ms, text.toUtf8().constData());
    fflush(stderr);
}

AIChatManager* AIChatManager::s_instance = nullptr;

AIChatManager* AIChatManager::instance()
{
    if (!s_instance)
        s_instance = new AIChatManager();
    return s_instance;
}

AIChatManager* AIChatManager::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine); Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void AIChatManager::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

AIChatManager::AIChatManager(QObject* parent) : QObject(parent)
{
    auto* llm = LLMManager::instance();
    connect(llm, &LLMManager::generationProgress,  this, &AIChatManager::onGenerationProgress);
    connect(llm, &LLMManager::generationCompleted, this, &AIChatManager::onGenerationCompleted);
    connect(llm, &LLMManager::generationError,     this, &AIChatManager::onGenerationError);
    connect(llm, &LLMManager::generationStopped,   this, &AIChatManager::onGenerationStopped);
    connect(llm, &LLMManager::modelLoadedChanged,      this, &AIChatManager::modelAvailableChanged);
    connect(llm, &LLMManager::currentModelNameChanged, this, &AIChatManager::currentModelNameChanged);
}

bool AIChatManager::modelAvailable() const
{
    return LLMManager::instance()->isModelLoaded();
}

QString AIChatManager::currentModelName() const
{
    return LLMManager::instance()->currentModelName();
}

// ---- public slots ----

void AIChatManager::sendMessage(const QString& text)
{
    if (text.trimmed().isEmpty() || m_isGenerating)
        return;

    SentryReporter::addBreadcrumb("ui.action", "AI Chat: user message");

    m_toolLoopDepth = 0;
    m_lastToolSignatures.clear();
    appendMessage("user", text.trimmed());
    chatLog("USER", text.trimmed());
    QString convPrompt = buildConversationPrompt();
    chatLog("CONV_PROMPT", convPrompt);
    startGeneration(buildSystemPrompt(), convPrompt);
}

void AIChatManager::clearHistory()
{
    if (m_isGenerating)
        LLMManager::instance()->stopGeneration();
    m_messages.clear();
    m_streamingText.clear();
    m_toolLoopDepth = 0;
    m_lastToolSignatures.clear();
    emit messagesChanged();
    emit streamingTextChanged();
}

void AIChatManager::stopGeneration()
{
    m_stopRequested = true;
    LLMManager::instance()->stopGeneration();
}

// ---- helpers ----

// Strip chat-template tokens that some models echo back (<|assistant|> etc.)
// and truncate at the first hallucinated "User:" continuation.
static QString cleanGeneratedText(const QString& raw)
{
    QString text = raw;

    // Remove <|...|> special tokens (Phi-3, Llama-3, Mistral, etc.)
    text.remove(QRegularExpression("<\\|[^|>]+\\|>"));

    // Truncate at the first point where the model starts hallucinating the next
    // user turn — "User:" or "Human:" at the start of a line.
    static const QRegularExpression nextUserRe(
        R"(\n(?:User|Human)\s*:)", QRegularExpression::CaseInsensitiveOption);
    int pos = nextUserRe.match(text).capturedStart();
    if (pos >= 0)
        text = text.left(pos);

    // Truncate if the model hallucinates a "RESULT:" continuation (history format)
    static const QRegularExpression resultRe(
        R"(\nRESULT\s*:)", QRegularExpression::CaseInsensitiveOption);
    int rpos = resultRe.match(text).capturedStart();
    if (rpos >= 0)
        text = text.left(rpos);

    // Truncate at "<after result>" or "EXAMPLE (" — model echoing its own examples
    static const QRegularExpression afterResultRe(
        R"(<after\s+result>|EXAMPLE\s*\()", QRegularExpression::CaseInsensitiveOption);
    int apos = afterResultRe.match(text).capturedStart();
    if (apos >= 0)
        text = text.left(apos);

    return text.trimmed();
}

// ---- generation callbacks ----

void AIChatManager::onGenerationProgress(const QString& partial, float /*progress*/)
{
    m_streamingText = cleanGeneratedText(partial);
    emit streamingTextChanged();
}

void AIChatManager::onGenerationCompleted(const QString& fullText)
{
    m_streamingText.clear();
    emit streamingTextChanged();

    chatLog("MODEL_RAW", fullText);
    executeToolCallsAndContinue(cleanGeneratedText(fullText));
}

void AIChatManager::onGenerationError(const QString& error)
{
    m_streamingText.clear();
    m_isGenerating = false;
    emit streamingTextChanged();
    emit isGeneratingChanged();
    appendMessage("assistant", QString("Error: %1").arg(error));
}

void AIChatManager::onGenerationStopped()
{
    if (!m_streamingText.isEmpty()) {
        appendMessage("assistant", m_streamingText);
        m_streamingText.clear();
        emit streamingTextChanged();
    }
    m_stopRequested = false;
    m_isGenerating = false;
    m_toolLoopDepth = 0;
    m_lastToolSignatures.clear();
    emit isGeneratingChanged();
}

// ---- private helpers ----

void AIChatManager::appendMessage(const QString& role, const QString& text, bool isTool)
{
    QVariantMap msg;
    msg["role"]   = role;
    msg["text"]   = text;
    msg["isTool"] = isTool;
    m_messages.append(msg);
    emit messagesChanged();
}

void AIChatManager::startGeneration(const QString& sysPrompt, const QString& userPrompt)
{
    if (m_stopRequested) {
        m_stopRequested = false;
        m_isGenerating = false;
        m_toolLoopDepth = 0;
        emit isGeneratingChanged();
        return;
    }
    m_isGenerating = true;
    emit isGeneratingChanged();
    chatLog("START_GEN", QString("sys=%1 user=%2 chars").arg(sysPrompt.size()).arg(userPrompt.size()));
    LLMManager::instance()->generateText(sysPrompt, userPrompt);
}

// Scan text for all top-level JSON objects that have both "name" and "arguments"
// keys — these are tool calls regardless of whatever tag the model put around them.
static QStringList extractToolJsonBlocks(const QString& text)
{
    QStringList results;

    // Primary: find {"name": "tool_name", "arguments": {...}} directly
    int len = text.length();
    for (int i = 0; i < len; ++i) {
        if (text[i] != QLatin1Char('{')) continue;
        int depth = 0, j = i;
        while (j < len) {
            if (text[j] == QLatin1Char('{'))      ++depth;
            else if (text[j] == QLatin1Char('}')) { if (--depth == 0) break; }
            ++j;
        }
        if (depth != 0) continue;
        QString candidate = text.mid(i, j - i + 1);
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(candidate.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject obj = doc.object();
            if (obj.contains("name") && obj.contains("arguments"))
                results << candidate;
        }
        i = j;
    }

    // Fallback: model sometimes mimics tool-result format:
    //   [Tool: tool_name]
    //   Arguments: {"param": "value"}
    // Reconstruct the canonical {"name":..., "arguments":...} from it.
    static const QRegularExpression altRe(
        R"(\[Tool:\s*([^\]]+)\]\s*(?:Arguments?:\s*)?(\{[^{}]*(?:\{[^{}]*\}[^{}]*)*\}))",
        QRegularExpression::DotMatchesEverythingOption | QRegularExpression::CaseInsensitiveOption);
    auto altIt = altRe.globalMatch(text);
    while (altIt.hasNext()) {
        auto m = altIt.next();
        QString toolName = m.captured(1).trimmed();
        QJsonParseError err;
        QJsonDocument argDoc = QJsonDocument::fromJson(m.captured(2).trimmed().toUtf8(), &err);
        if (err.error != QJsonParseError::NoError) continue;
        QJsonObject call;
        call["name"]      = toolName;
        call["arguments"] = argDoc.isObject() ? argDoc.object() : QJsonObject{};
        results << QString::fromUtf8(QJsonDocument(call).toJson(QJsonDocument::Compact));
    }

    return results;
}

void AIChatManager::executeToolCallsAndContinue(const QString& assistantText)
{
    QStringList toolBlocks = m_mcpServer ? extractToolJsonBlocks(assistantText) : QStringList{};

    // Enforce one-tool-per-response rule: discard everything after the first call.
    // The model must see the result before deciding the next step.
    if (toolBlocks.size() > 1) {
        chatLog("MULTI_TOOL_TRIMMED",
                QString("Model sent %1 calls — keeping only first").arg(toolBlocks.size()));
        toolBlocks = toolBlocks.mid(0, 1);
    }

    // Truncate the full text at end of first JSON block so stored history is clean
    // (model sometimes outputs future rounds after the closing } — discard that).
    QString cleanedAssistant = assistantText;
    if (!toolBlocks.isEmpty()) {
        int jsonEnd = assistantText.indexOf(toolBlocks.first())
                      + toolBlocks.first().length();
        if (jsonEnd > 0)
            cleanedAssistant = assistantText.left(jsonEnd).trimmed();
    }

    // Build visible text: everything that isn't a raw JSON tool block
    QString visibleText = cleanedAssistant;
    for (const QString& block : toolBlocks)
        visibleText.remove(block);
    visibleText = visibleText.simplified();

    if (toolBlocks.isEmpty() || m_toolLoopDepth >= kMaxToolLoops) {
        // Plain response — add to history and finish
        appendMessage("assistant", assistantText.trimmed());
        m_isGenerating = false;
        m_toolLoopDepth = 0;
        m_lastToolSignatures.clear();
        emit isGeneratingChanged();
        return;
    }

    // ---- Loop detection ----
    // Build canonical compact-JSON signatures for the current tool calls.
    QStringList currentSigs;
    for (const QString& block : toolBlocks) {
        QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8());
        if (doc.isObject())
            currentSigs << QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
    }
    currentSigs.sort();

    if (!m_lastToolSignatures.isEmpty() && m_lastToolSignatures == currentSigs) {
        // Same tool calls as the previous round — the model is stuck in a loop.
        // De-duplicate: remove consecutive duplicate assistant/tool message pairs
        // from history so only one copy is visible, then stop.
        chatLog("LOOP_DETECTED", currentSigs.join("; "));

        // Walk backwards and remove messages that duplicate the previous round:
        // pattern is [ assistant(JSON), tool(RESULT) ] repeated.
        // Keep walking back while we see the same signatures.
        while (m_messages.size() >= 2) {
            QVariantMap last = m_messages.last().toMap();
            QVariantMap prev = m_messages.at(m_messages.size() - 2).toMap();
            bool lastIsTool = last["isTool"].toBool();
            bool prevIsAssistant = (prev["role"].toString() == "assistant" && !prev["isTool"].toBool());
            if (lastIsTool && prevIsAssistant) {
                // Check if the assistant entry matches a known duplicate signature
                QString prevText = prev["text"].toString().trimmed();
                QJsonDocument pd = QJsonDocument::fromJson(prevText.toUtf8());
                bool isDupAssistant = pd.isObject() && currentSigs.contains(
                    QString::fromUtf8(pd.toJson(QJsonDocument::Compact)));
                if (isDupAssistant) {
                    m_messages.removeLast(); // remove tool result
                    m_messages.removeLast(); // remove assistant JSON
                    continue;
                }
            }
            break;
        }

        appendMessage("assistant", "Done.");
        m_isGenerating = false;
        m_toolLoopDepth = 0;
        m_lastToolSignatures.clear();
        emit messagesChanged();
        emit isGeneratingChanged();
        return;
    }
    m_lastToolSignatures = currentSigs;

    // Add an assistant history entry for this turn.
    // This is critical: without it the model's history shows RESULT with no
    // preceding assistant action, so it re-calls the same tool on the next turn.
    if (!visibleText.trimmed().isEmpty()) {
        appendMessage("assistant", visibleText.trimmed());
    } else {
        // Model output was tool JSON only.
        // Store the raw JSON so the model sees JSON→RESULT in history and
        // continues to output JSON (not the "[calling X]" text it would mimic).
        // The UI detects this format and renders it as "[calling X]".
        appendMessage("assistant", toolBlocks.join("\n"));
    }

    // Execute each tool call and record results
    bool anyToolError = false;
    for (const QString& block : toolBlocks) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            appendMessage("tool", QString("[parse error] %1").arg(err.errorString()), true);
            anyToolError = true;
            continue;
        }
        QJsonObject obj = doc.object();
        QString toolName = obj["name"].toString();
        QJsonObject toolArgs = obj["arguments"].toObject();

        SentryReporter::addBreadcrumb("ai.tool_call", toolName);
        chatLog("TOOL_CALL", QString("%1 args=%2").arg(toolName,
            QString::fromUtf8(QJsonDocument(toolArgs).toJson(QJsonDocument::Compact))));
        QJsonObject result = m_mcpServer->callTool(toolName, toolArgs);

        QString resultText;
        QJsonArray content = result["content"].toArray();
        if (!content.isEmpty())
            resultText = content.first().toObject()["text"].toString();
        else
            resultText = QJsonDocument(result).toJson(QJsonDocument::Compact);

        chatLog("TOOL_RESULT", resultText);

        // Detect tool errors so we can decide whether to allow recovery loops
        if (result["isError"].toBool() || resultText.contains("Error:"))
            anyToolError = true;

        // Store a truncated version of the result in history to keep context compact.
        // The first line (summary) is enough for the model; verbose scripts/dumps are noise.
        QString historyText = resultText.trimmed();
        int nlPos = historyText.indexOf('\n');
        if (nlPos > 0 && historyText.length() > 120)
            historyText = historyText.left(nlPos).trimmed(); // keep only first line

        QString toolEntry = QString("[Tool: %1]\n%2").arg(toolName, historyText);
        appendMessage("tool", toolEntry, true);
    }

    ++m_toolLoopDepth;

    const QString summaryPrompt =
        "You are a 3D editor assistant. The actions above have been executed. "
        "Write ONE short sentence confirming what was done. Do NOT call any tools.";

    if (m_toolLoopDepth >= kMaxToolLoops) {
        // Hard limit reached — force a plain summary.
        startGeneration(summaryPrompt, buildConversationPrompt(3));
    } else if (!anyToolError) {
        // Tool succeeded — give model full system prompt so it can plan next steps
        // or confirm completion. KV cache reuse makes this fast.
        startGeneration(buildSystemPrompt(), buildConversationPrompt());
    } else if (m_toolLoopDepth >= 2) {
        // Two rounds of errors — the model is stuck. Stop and surface the error.
        appendMessage("assistant",
            "I wasn't able to complete that action (the required resource doesn't exist or "
            "isn't accessible). Please check that the material/mesh/texture exists and try again.");
        m_isGenerating = false;
        m_toolLoopDepth = 0;
        m_lastToolSignatures.clear();
        emit isGeneratingChanged();
    } else {
        // One error — give the model the full system prompt so it can recover
        // (e.g. call list_materials and retry with a real name).
        startGeneration(buildSystemPrompt(), buildConversationPrompt());
    }
}

QString AIChatManager::buildSystemPrompt() const
{
    QString tools;
    if (m_mcpServer) {
        QJsonArray toolList = m_mcpServer->buildToolsList();
        for (const QJsonValue& tv : toolList) {
            QJsonObject t = tv.toObject();
            QString name  = t["name"].toString();
            QString desc  = t["description"].toString();
            QJsonObject schema = t["inputSchema"].toObject();
            QJsonObject props  = schema["properties"].toObject();
            QStringList paramLines;
            for (auto pit = props.begin(); pit != props.end(); ++pit) {
                QJsonObject pdef = pit.value().toObject();
                QString pdesc = pdef["description"].toString();
                paramLines << QString("    %1: %2").arg(pit.key(), pdesc);
            }
            tools += QString("- %1: %2\n").arg(name, desc);
            if (!paramLines.isEmpty())
                tools += paramLines.join("\n") + "\n";
        }
    }

    // Inject current scene + material state so the model can act without a discovery round.
    QString sceneSection;
    if (m_mcpServer) {
        // Objects and their current materials
        auto extractText = [](const QJsonObject& result) -> QString {
            QJsonArray content = result["content"].toArray();
            if (!content.isEmpty())
                return content.first().toObject()["text"].toString().trimmed();
            return {};
        };
        QString sceneInfo = extractText(m_mcpServer->callTool("get_scene_info", {}));
        QString matRaw    = extractText(m_mcpServer->callTool("list_materials", {}));
        // Filter material list: skip built-in Ogre materials (BaseWhite, Ogre/*, etc.)
        // and keep only short user-created names for the scene context.
        QStringList userMats;
        static const QStringList sysMatPrefixes = {
            "Available", "BaseWhite", "Ogre/", "RTSS/", "SdkTrays/",
            "Debug", "Default", "GUI_", "NormalVisualizer", "BoneWeight",
            "MeshInfo", "SelectionBox", "Procedural/", "Axes/"
        };
        for (const QString& line : matRaw.split('\n')) {
            QString m = line.trimmed();
            if (m.isEmpty()) continue;
            bool isSystem = false;
            for (const QString& prefix : sysMatPrefixes)
                if (m.startsWith(prefix)) { isSystem = true; break; }
            if (!isSystem) userMats << m;
        }
        if (userMats.size() > 20) userMats = userMats.mid(0, 20); // cap for prompt size
        if (!sceneInfo.isEmpty() || !userMats.isEmpty()) {
            sceneSection = "Current scene state:\n";
            if (!sceneInfo.isEmpty()) sceneSection += sceneInfo + "\n";
            if (!userMats.isEmpty())  sceneSection += "Available materials: " + userMats.join(", ") + "\n";
            sceneSection += "\n";
        }
    }

    // Static part first (header + instructions + tool list) so KV cache prefix
    // stays valid across calls — scene section is dynamic and goes at the end.
    return QString(
        "You are an AI assistant controlling QtMeshEditor, a 3D mesh editor.\n\n"
        "HOW TO RESPOND — choose exactly one format per response:\n\n"
        "If you need to call a tool:\n"
        "  Thought: <what you are doing and what still remains after this>\n"
        "  {\"name\": \"tool_name\", \"arguments\": {\"param\": \"value\"}}\n\n"
        "If all steps are complete:\n"
        "  Done: <one sentence confirming what was accomplished>\n\n"
        "CRITICAL RULES:\n"
        "1. Output EXACTLY ONE JSON tool call per response, then STOP. Do not write anything after the closing }.\n"
        "2. Do not output future tool calls. Wait for each result before deciding the next step.\n"
        "3. For requests like 'wooden box': you need 3 steps — create the primitive, create the material, apply it.\n"
        "   Continue calling tools until ALL steps are done, then write Done:.\n"
        "4. Never use a material not listed under 'Available materials' below. Use create_material first.\n"
        "5. Never invent tool names or parameter names. Only use what is listed below.\n"
        "6. If a tool returns an error, call list_materials or get_scene_info to find correct names.\n\n"
        "Available tools:\n%1\n"
        "%2"
    ).arg(tools, sceneSection);
}

QString AIChatManager::buildConversationPrompt(int maxHistory) const
{
    // Caller can pass an explicit window; 0 = use default based on loop depth.
    if (maxHistory == 0)
        maxHistory = (m_toolLoopDepth >= kMaxToolLoops - 1) ? 4 : 8;
    int start = qMax(0, m_messages.size() - maxHistory);

    QString conv;
    for (int i = start; i < m_messages.size(); ++i) {
        QVariantMap m = m_messages[i].toMap();
        QString role = m["role"].toString();
        QString text = m["text"].toString();
        bool isTool  = m["isTool"].toBool();

        if (role == "user")
            conv += "User: " + text + "\n";
        else if (role == "tool" || isTool)
            // Prefix with RESULT: so the model distinguishes past results from new calls
            conv += "RESULT: " + text + "\n";
        else
            conv += "Assistant: " + text + "\n";
    }
    conv += "Assistant:";
    return conv;
}
