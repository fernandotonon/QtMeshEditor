#include "AIChatManager.h"
#include "LLMManager.h"
#include "MCPServer.h"
#include "SentryReporter.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>
#include <cstdio>
#include <QDateTime>
#include <QSettings>
#include <QFileInfo>

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
    m_jsonRetryCount = 0;
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

    // Strip trailing pipe characters — Qwen and some other models emit "|" as a
    // stop-token artifact at the end of their output.
    while (text.endsWith('|') || text.endsWith(" |"))
        text = text.left(text.lastIndexOf('|')).trimmed();

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
    m_jsonRetryCount = 0;
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
    // Structured JSON envelope is ~100–150 tokens; allow 500 for generous headroom.
    // Multi-call stuffing is no longer a risk since we parse a single top-level object.
    LLMManager::instance()->generateText(sysPrompt, userPrompt, 500);
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
    // ---- Parse structured JSON response ----
    // The conversation prompt ends with "Assistant: {" so the model may start with
    // "thought":... (missing the opening brace) or with text + {"thought":...}.
    // Strategy: try multiple extraction methods to find a valid structured object.

    auto tryParseStructured = [](const QString& json) -> QPair<bool, QJsonDocument> {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject()) {
            QJsonObject obj = doc.object();
            if (obj.contains("command") || obj.contains("response"))
                return {true, doc};
        }
        return {false, {}};
    };

    // Helper: extract the first balanced {...} substring from text
    auto extractFirstJsonBlock = [](const QString& text, int searchFrom = 0) -> QString {
        int start = text.indexOf('{', searchFrom);
        if (start < 0) return {};
        int depth = 0;
        for (int i = start; i < text.length(); ++i) {
            if (text[i] == '{') ++depth;
            else if (text[i] == '}') {
                if (--depth == 0) return text.mid(start, i - start + 1);
            }
        }
        return {};
    };

    QJsonDocument responseDoc;
    bool isStructured = false;

    // Method 1: parse the whole output directly (model included the opening {)
    auto [ok1, doc1] = tryParseStructured(assistantText.trimmed());
    if (ok1) { isStructured = true; responseDoc = doc1; }

    // Method 2: prepend { (model output continues from our "Assistant: {" primer)
    if (!isStructured) {
        auto [ok2, doc2] = tryParseStructured('{' + assistantText.trimmed());
        if (ok2) { isStructured = true; responseDoc = doc2; }
    }

    // Method 3: extract the first {...} block from anywhere in the output
    // (model may have outputted text before or after the JSON)
    if (!isStructured) {
        QString block = extractFirstJsonBlock(assistantText);
        if (!block.isEmpty()) {
            auto [ok3, doc3] = tryParseStructured(block);
            if (ok3) { isStructured = true; responseDoc = doc3; }
        }
    }

    // Method 4: prepend { and extract (missing opening brace + trailing text)
    if (!isStructured) {
        QString withBrace = '{' + assistantText.trimmed();
        QString block = extractFirstJsonBlock(withBrace);
        if (!block.isEmpty()) {
            auto [ok4, doc4] = tryParseStructured(block);
            if (ok4) { isStructured = true; responseDoc = doc4; }
        }
    }

    if (!isStructured) {
        chatLog("MALFORMED_JSON",
                QString("retry %1/%2 — raw: %3")
                    .arg(m_jsonRetryCount + 1).arg(kMaxJsonRetries)
                    .arg(assistantText.left(120)));
        ++m_jsonRetryCount;
        if (m_jsonRetryCount >= kMaxJsonRetries) {
            appendMessage("assistant",
                "I had trouble generating a valid response. Please try again.");
            m_isGenerating = false;
            m_toolLoopDepth = 0;
            m_jsonRetryCount = 0;
            m_lastToolSignatures.clear();
            emit isGeneratingChanged();
        } else {
            startGeneration(buildSystemPrompt(), buildConversationPrompt());
        }
        return;
    }
    m_jsonRetryCount = 0;

    QJsonObject resp    = responseDoc.object();
    QString command     = resp["command"].toString();          // empty string if null/absent
    QJsonObject toolArgs = resp["arguments"].toObject();
    QString userResponse = resp["response"].toString();
    bool hasCommand = !resp["command"].isNull() && !command.isEmpty();

    chatLog("STRUCTURED_RESP",
            QString("command=%1 remaining=%2").arg(
                command.isEmpty() ? "null" : command,
                QString::fromUtf8(
                    QJsonDocument(resp["remaining"].toArray())
                        .toJson(QJsonDocument::Compact))));

    // ---- No command → task is done ----
    if (!hasCommand || m_toolLoopDepth >= kMaxToolLoops) {
        QString doneText = userResponse.isEmpty() ? "Done." : userResponse;
        appendMessage("assistant", doneText);
        m_isGenerating = false;
        m_toolLoopDepth = 0;
        m_jsonRetryCount = 0;
        m_lastToolSignatures.clear();
        emit isGeneratingChanged();
        return;
    }

    // ---- Loop detection ----
    QString sig = QString::fromUtf8(QJsonDocument(QJsonObject{
        {"command", command}, {"arguments", toolArgs}
    }).toJson(QJsonDocument::Compact));
    QStringList currentSigs = {sig};

    if (!m_lastToolSignatures.isEmpty() && m_lastToolSignatures == currentSigs) {
        chatLog("LOOP_DETECTED", sig);
        appendMessage("assistant", "Done.");
        m_isGenerating = false;
        m_toolLoopDepth = 0;
        m_jsonRetryCount = 0;
        m_lastToolSignatures.clear();
        emit isGeneratingChanged();
        return;
    }
    m_lastToolSignatures = currentSigs;

    // Store compact JSON in history so the model sees its own format in context.
    // The UI renders messages with a "command" key as "[calling X]".
    appendMessage("assistant",
        QString::fromUtf8(responseDoc.toJson(QJsonDocument::Compact)));

    // ---- Execute the tool ----
    bool anyToolError = false;
    if (m_mcpServer) {
        SentryReporter::addBreadcrumb("ai.tool_call", command);
        chatLog("TOOL_CALL", QString("%1 args=%2").arg(command,
            QString::fromUtf8(QJsonDocument(toolArgs).toJson(QJsonDocument::Compact))));

        QJsonObject result = m_mcpServer->callTool(command, toolArgs);

        QString resultText;
        QJsonArray content = result["content"].toArray();
        if (!content.isEmpty())
            resultText = content.first().toObject()["text"].toString();
        else
            resultText = QJsonDocument(result).toJson(QJsonDocument::Compact);

        chatLog("TOOL_RESULT", resultText);

        if (result["isError"].toBool() || resultText.contains("Error:"))
            anyToolError = true;

        // Truncate result for history — first line is enough context for the model.
        QString historyText = resultText.trimmed();
        int nlPos = historyText.indexOf('\n');
        if (nlPos > 0 && historyText.length() > 120)
            historyText = historyText.left(nlPos).trimmed();

        appendMessage("tool", QString("[Tool: %1]\n%2").arg(command, historyText), true);
    }

    ++m_toolLoopDepth;

    // If the model declared remaining=[] (no more steps), force done now.
    // This prevents the 3B model from replaying the system-prompt example
    // (e.g. always re-creating a wooden box after every single tool call).
    QJsonArray remaining = resp["remaining"].toArray();
    bool noMoreSteps = remaining.isEmpty() && !anyToolError;

    if (noMoreSteps || m_toolLoopDepth >= kMaxToolLoops) {
        // Task complete (or hard limit).
        appendMessage("assistant", userResponse.isEmpty() ? "Done." : userResponse);
        m_isGenerating = false;
        m_toolLoopDepth = 0;
        m_jsonRetryCount = 0;
        m_lastToolSignatures.clear();
        emit isGeneratingChanged();
    } else if (anyToolError && m_toolLoopDepth >= 2) {
        // Two rounds of errors — give up and surface the problem.
        appendMessage("assistant",
            "I wasn't able to complete that action. The required resource may not exist. "
            "Please check that the material/mesh/texture name is correct and try again.");
        m_isGenerating = false;
        m_toolLoopDepth = 0;
        m_jsonRetryCount = 0;
        m_lastToolSignatures.clear();
        emit isGeneratingChanged();
    } else {
        // Continue — tool succeeded (or first error, allow recovery).
        startGeneration(buildSystemPrompt(), buildConversationPrompt());
    }
}

QString AIChatManager::buildSystemPrompt() const
{
    // Only expose the core tool subset to the AI chat — 39 tools overwhelm a 3B model.
    // The full set is still available via MCP/HTTP for external clients.
    static const QStringList chatTools = {
        "create_primitive", "create_material", "modify_material", "apply_material",
        "transform_mesh", "get_scene_info", "list_materials", "load_mesh",
        "export_mesh", "list_textures", "set_texture", "take_screenshot",
        "list_files", "search_files", "read_file",
        "camera_control", "get_camera_info"
    };

    QString tools;
    if (m_mcpServer) {
        QJsonArray toolList = m_mcpServer->buildToolsList();
        for (const QJsonValue& tv : toolList) {
            QJsonObject t = tv.toObject();
            QString name  = t["name"].toString();
            if (!chatTools.contains(name))
                continue;  // skip tools not in the AI chat subset
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
        // Recent files from QSettings — include full paths so the model can pass them to load_mesh
        QSettings settings;
        QStringList recentFiles = settings.value("RecentFiles/files").toStringList();
        QStringList recentEntries;
        for (const QString& path : recentFiles) {
            QFileInfo fi(path);
            if (fi.exists())
                recentEntries << path;  // full absolute path
        }
        if (recentEntries.size() > 10) recentEntries = recentEntries.mid(0, 10);

        if (!sceneInfo.isEmpty() || !userMats.isEmpty() || !recentEntries.isEmpty()) {
            sceneSection = "Current scene state:\n";
            if (!sceneInfo.isEmpty())    sceneSection += sceneInfo + "\n";
            if (!userMats.isEmpty())     sceneSection += "Available materials: " + userMats.join(", ") + "\n";
            if (!recentEntries.isEmpty()) {
                sceneSection += "Recent files (use with load_mesh):\n";
                for (const QString& path : recentEntries)
                    sceneSection += "  " + path + "\n";
            }
            sceneSection += "\n";
        }
    }

    // Static part first (header + instructions + tool list) so KV cache prefix
    // stays valid across calls — scene section is dynamic and goes at the end.
    return QString(
        "You are an AI assistant controlling QtMeshEditor, a 3D mesh editor.\n\n"
        "RESPONSE FORMAT — you MUST always reply with a single valid JSON object:\n"
        "{\n"
        "  \"thought\": \"one sentence, max 10 words\",\n"
        "  \"command\": \"tool_name or null\",\n"
        "  \"arguments\": {\"param\": \"value\"},\n"
        "  \"remaining\": [\"next_step\", \"after_that\"],\n"
        "  \"response\": \"shown to user — only set when command is null\"\n"
        "}\n\n"
        "Field rules:\n"
        "- \"command\": tool to run now, or null when the whole task is finished.\n"
        "- \"arguments\": params for the command ({} when command is null).\n"
        "- \"remaining\": tool names you still need to call AFTER this one.\n"
        "- \"response\": final user-facing message; omit or set null while commands remain.\n\n"
        "CRITICAL RULES:\n"
        "1. Output valid JSON only. No text outside the JSON object. One command per response.\n"
        "2. Check RESULT messages: 'Created X' or 'Applied' means that step is DONE — advance.\n"
        "   Never call the same command twice for the same object.\n"
        "3. transform_mesh requires \"name\" = exact node name (use get_scene_info if unsure).\n"
        "   Never call create_primitive to recover from a transform error.\n"
        "   POSITION EXAMPLES from origin [0,0,0]:\n"
        "     'move left 2'   → position: [-2, 0, 0]\n"
        "     'move right 2'  → position: [2, 0, 0]\n"
        "     'move up 3'     → position: [0, 3, 0]    ← Y is up/down\n"
        "     'on the floor'  → position: [0, 0, 0]    ← Y=0 is ground\n"
        "     'move forward 1'→ position: [0, 0, -1]   ← Z- is forward/front\n"
        "     'move back 1'   → position: [0, 0, 1]    ← Z+ is back\n"
        "   UP/DOWN/FLOOR = always Y axis. NEVER use Z for vertical movement.\n"
        "   For relative moves: call get_scene_info first to read current position.\n"
        "4. Each user message is a SEPARATE task. ONLY do what that message asks.\n"
        "   Do NOT repeat actions from previous messages (e.g. do not re-apply old materials).\n"
        "5. Use simple names for create_primitive: 'box', 'sphere', 'cylinder', 'cone', 'plane'.\n"
        "   ONLY create what the user asked for. ONE primitive per request. Never create extras.\n"
        "6. When remaining is empty [], your next response MUST have command=null with a response.\n"
        "   After the last step succeeds, the task is DONE. Do NOT add extra steps.\n"
        "7. Never use a material not in 'Available materials'. Call create_material first.\n"
        "8. Never invent tool names or parameter names. Only use tools listed below.\n\n"
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

    auto formatMsg = [](const QVariantMap& m) -> QString {
        QString role = m["role"].toString();
        QString text = m["text"].toString();
        bool isTool  = m["isTool"].toBool();
        if (role == "user")
            return "User: " + text + "\n";
        if (role == "tool" || isTool)
            return "RESULT: " + text + "\n";
        return "Assistant: " + text + "\n";
    };

    QString conv;

    // Always include the first user message so the model knows the original request,
    // even if it has scrolled out of the sliding history window.
    if (start > 0) {
        for (int i = 0; i < m_messages.size(); ++i) {
            QVariantMap m = m_messages[i].toMap();
            if (m["role"].toString() == "user") {
                conv += formatMsg(m);
                conv += "...\n"; // indicate intervening history was omitted
                break;
            }
        }
    }

    for (int i = start; i < m_messages.size(); ++i)
        conv += formatMsg(m_messages[i].toMap());

    // Prime with "{" so the model continues in structured-JSON mode.
    conv += "Assistant: {";
    return conv;
}
