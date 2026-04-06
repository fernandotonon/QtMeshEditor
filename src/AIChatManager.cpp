#include "AIChatManager.h"
#include "LLMManager.h"
#include "MCPServer.h"
#include "SentryReporter.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QRegularExpression>

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
    appendMessage("user", text.trimmed());
    startGeneration(buildSystemPrompt(), buildConversationPrompt());
}

void AIChatManager::clearHistory()
{
    if (m_isGenerating)
        LLMManager::instance()->stopGeneration();
    m_messages.clear();
    m_streamingText.clear();
    m_toolLoopDepth = 0;
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

    // Build visible text: everything that isn't a raw JSON tool block
    QString visibleText = assistantText;
    for (const QString& block : toolBlocks)
        visibleText.remove(block);
    visibleText = visibleText.simplified();

    if (toolBlocks.isEmpty() || m_toolLoopDepth >= kMaxToolLoops) {
        // Plain response — add to history and finish
        appendMessage("assistant", assistantText.trimmed());
        m_isGenerating = false;
        m_toolLoopDepth = 0;
        emit isGeneratingChanged();
        return;
    }

    // Show the assistant narration (without the raw JSON blocks) if any
    if (!visibleText.trimmed().isEmpty())
        appendMessage("assistant", visibleText.trimmed());

    // Execute each tool call and record results
    for (const QString& block : toolBlocks) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(block.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            appendMessage("tool", QString("[parse error] %1").arg(err.errorString()), true);
            continue;
        }
        QJsonObject obj = doc.object();
        QString toolName = obj["name"].toString();
        QJsonObject toolArgs = obj["arguments"].toObject();

        SentryReporter::addBreadcrumb("ai.tool_call", toolName);
        QJsonObject result = m_mcpServer->callTool(toolName, toolArgs);

        QString resultText;
        QJsonArray content = result["content"].toArray();
        if (!content.isEmpty())
            resultText = content.first().toObject()["text"].toString();
        else
            resultText = QJsonDocument(result).toJson(QJsonDocument::Compact);

        QString toolEntry = QString("[Tool: %1]\n%2").arg(toolName, resultText.trimmed());
        appendMessage("tool", toolEntry, true);
    }

    // Follow-up generation: summarise what happened.
    // Use a minimal system prompt (no tool list) to save context tokens and
    // prevent the model re-entering an action loop after every tool call.
    ++m_toolLoopDepth;
    const QString summaryPrompt =
        "You are a 3D editor assistant. The tool calls above have been executed. "
        "Write ONE short sentence confirming what was done. "
        "Do NOT call any tools. Do NOT use <tool_call> blocks.";
    startGeneration(summaryPrompt, buildConversationPrompt());
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

    return QString(
        "You are an AI assistant embedded in QtMeshEditor, a 3D mesh editor.\n"
        "You help users control the editor using natural language.\n\n"
        "RULES:\n"
        "1. For editor actions output a bare JSON object on its own line:\n"
        "{\"name\": \"tool_name\", \"arguments\": {\"param\": \"value\"}}\n"
        "2. For questions or general chat, answer directly — no JSON.\n"
        "3. Never invent tool names or parameters. Only use the tools and params listed below.\n"
        "4. Keep responses brief.\n\n"
        "Available tools:\n%1"
    ).arg(tools);
}

QString AIChatManager::buildConversationPrompt(const QString& /*unused*/) const
{
    // Limit history to avoid context overflow.
    // Follow-ups after tool calls use a shorter window (6) to save tokens.
    const int kMaxHistory = (m_toolLoopDepth > 0) ? 6 : 10;
    int start = qMax(0, m_messages.size() - kMaxHistory);

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
