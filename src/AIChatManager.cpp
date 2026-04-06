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
    LLMManager::instance()->stopGeneration();
}

// ---- generation callbacks ----

void AIChatManager::onGenerationProgress(const QString& partial, float /*progress*/)
{
    m_streamingText = partial;
    emit streamingTextChanged();
}

void AIChatManager::onGenerationCompleted(const QString& fullText)
{
    m_streamingText.clear();
    emit streamingTextChanged();

    executeToolCallsAndContinue(fullText);
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
    m_isGenerating = true;
    emit isGeneratingChanged();
    LLMManager::instance()->generateText(sysPrompt, userPrompt);
}

void AIChatManager::executeToolCallsAndContinue(const QString& assistantText)
{
    // Extract <tool_call>...</tool_call> blocks
    static const QRegularExpression re(
        R"(<tool_call>\s*(.*?)\s*</tool_call>)",
        QRegularExpression::DotMatchesEverythingOption);

    // Separate visible text from tool call markers
    QString visibleText = assistantText;
    visibleText.replace(re, QString()).simplified();

    QRegularExpressionMatchIterator it = re.globalMatch(assistantText);
    QStringList toolBlocks;
    while (it.hasNext())
        toolBlocks << it.next().captured(1).trimmed();

    if (toolBlocks.isEmpty() || !m_mcpServer || m_toolLoopDepth >= kMaxToolLoops) {
        // Plain response — add to history and finish
        appendMessage("assistant", assistantText.trimmed());
        m_isGenerating = false;
        m_toolLoopDepth = 0;
        emit isGeneratingChanged();
        return;
    }

    // Show the assistant narration (without raw XML tags) if any
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

    // Feed tool results back to the LLM for a follow-up response
    ++m_toolLoopDepth;
    startGeneration(buildSystemPrompt(), buildConversationPrompt());
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
        "IMPORTANT RULES:\n"
        "1. For editor actions (create objects, move things, change materials, etc.) use a tool call.\n"
        "2. For questions about yourself, general knowledge, or anything not requiring editor state, answer directly WITHOUT tool calls.\n"
        "3. After a tool result arrives, give a SHORT plain-text summary of what happened. Do NOT call more tools unless the user asks for something additional.\n"
        "4. Never invent tool names. Only use the tools listed below.\n"
        "5. Keep responses concise.\n\n"
        "Tool call format (emit this block when you need to run a tool):\n"
        "<tool_call>\n"
        "{\"name\": \"tool_name\", \"arguments\": {\"param\": \"value\"}}\n"
        "</tool_call>\n\n"
        "Available tools:\n%1"
    ).arg(tools);
}

QString AIChatManager::buildConversationPrompt(const QString& /*unused*/) const
{
    // Build the full conversation as a text block so any llama.cpp model can follow it
    QString conv;
    for (const QVariant& v : m_messages) {
        QVariantMap m = v.toMap();
        QString role = m["role"].toString();
        QString text = m["text"].toString();
        bool isTool  = m["isTool"].toBool();

        if (role == "user")
            conv += "User: " + text + "\n";
        else if (role == "tool" || isTool)
            conv += text + "\n";         // already has [Tool: name] prefix
        else
            conv += "Assistant: " + text + "\n";
    }
    conv += "Assistant:";
    return conv;
}
