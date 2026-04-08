#ifndef AICHATMANAGER_H
#define AICHATMANAGER_H

#include <QObject>
#include <QVariantList>
#include <QJsonObject>
#include <QQmlEngine>
#include <QJSEngine>
#include <QPointer>

class MCPServer;

class AIChatManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QVariantList messages    READ messages          NOTIFY messagesChanged)
    Q_PROPERTY(bool isGenerating        READ isGenerating      NOTIFY isGeneratingChanged)
    Q_PROPERTY(bool modelAvailable      READ modelAvailable    NOTIFY modelAvailableChanged)
    Q_PROPERTY(QString streamingText    READ streamingText     NOTIFY streamingTextChanged)
    Q_PROPERTY(QString currentModelName READ currentModelName  NOTIFY currentModelNameChanged)

public:
    static AIChatManager* instance();
    static AIChatManager* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    QVariantList messages()     const { return m_messages; }
    bool isGenerating()         const { return m_isGenerating; }
    bool modelAvailable()       const;
    QString streamingText()     const { return m_streamingText; }
    QString currentModelName()  const;

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
    void currentModelNameChanged();

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
    QString buildConversationPrompt(int maxHistory = 0) const;
    void startGeneration(const QString& sysPrompt, const QString& userPrompt);

    static AIChatManager* s_instance;

    QVariantList m_messages;  // {role, text, isTool}
    bool m_isGenerating   = false;
    bool m_stopRequested  = false;   // set by stopGeneration(), checked before follow-up starts
    QString m_streamingText;
    QPointer<MCPServer> m_mcpServer;

    // Agentic loop state
    int m_toolLoopDepth  = 0;
    int m_jsonRetryCount = 0;
    static const int kMaxToolLoops  = 10;
    static const int kMaxJsonRetries = 2;  // retry if model outputs malformed JSON
    QStringList m_lastToolSignatures; // compact JSON of tool calls from previous round
};

#endif // AICHATMANAGER_H
