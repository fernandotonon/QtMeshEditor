#ifndef AICHATMANAGER_H
#define AICHATMANAGER_H

#include <QObject>
#include <QVariantList>
#include <QJsonObject>
#include <QQmlEngine>
#include <QJSEngine>

class MCPServer;

class AIChatManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QVariantList messages   READ messages        NOTIFY messagesChanged)
    Q_PROPERTY(bool isGenerating       READ isGenerating    NOTIFY isGeneratingChanged)
    Q_PROPERTY(bool modelAvailable     READ modelAvailable  NOTIFY modelAvailableChanged)
    Q_PROPERTY(QString streamingText   READ streamingText   NOTIFY streamingTextChanged)

public:
    static AIChatManager* instance();
    static AIChatManager* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    QVariantList messages()  const { return m_messages; }
    bool isGenerating()      const { return m_isGenerating; }
    bool modelAvailable()    const;
    QString streamingText()  const { return m_streamingText; }

    Q_INVOKABLE void sendMessage(const QString& text);
    Q_INVOKABLE void clearHistory();
    Q_INVOKABLE void stopGeneration();

    // Called by MainWindow after MCPServer is created
    void setMcpServer(MCPServer* server) { m_mcpServer = server; }

signals:
    void messagesChanged();
    void isGeneratingChanged();
    void modelAvailableChanged();
    void streamingTextChanged();

private slots:
    void onGenerationProgress(const QString& partial, float progress);
    void onGenerationCompleted(const QString& fullText);
    void onGenerationError(const QString& error);
    void onGenerationStopped();

private:
    explicit AIChatManager(QObject* parent = nullptr);

    void appendMessage(const QString& role, const QString& text, bool isTool = false);
    void executeToolCallsAndContinue(const QString& assistantText);
    QString buildSystemPrompt() const;
    QString buildConversationPrompt(const QString& nextUserMessage = QString()) const;
    void startGeneration(const QString& sysPrompt, const QString& userPrompt);

    static AIChatManager* s_instance;

    QVariantList m_messages;  // {role, text, isTool}
    bool m_isGenerating  = false;
    QString m_streamingText;
    MCPServer* m_mcpServer = nullptr;

    // Agentic loop state
    int m_toolLoopDepth = 0;
    static const int kMaxToolLoops = 5;
};

#endif // AICHATMANAGER_H
