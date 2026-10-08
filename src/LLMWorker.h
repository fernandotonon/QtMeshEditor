#ifndef LLMWORKER_H
#define LLMWORKER_H

#include <QObject>
#include <QThread>
#include <QString>
#include <QMutex>
#include <QWaitCondition>
#include <atomic>

#ifdef ENABLE_LOCAL_LLM
#include "llama.h"
#endif

struct LLMSettings {
    int contextSize = 8192;   // the AI agent's prompts (tool docs + scene state) need more than 4k
    int maxTokens = 2048;
    float temperature = 0.7f;
    int gpuLayers = 99;
    int threads = 0; // 0 = auto
    float topP = 0.9f;
    int topK = 40;
    float repeatPenalty = 1.1f;
};

class LLMWorker : public QObject
{
    Q_OBJECT

public:
    explicit LLMWorker(QObject *parent = nullptr);
    ~LLMWorker();

    void initBackend();  // Must be called after moving to worker thread
    bool loadModel(const QString &modelPath);
    void unloadModel();
    bool isModelLoaded() const;
    QString getLoadedModelPath() const { return m_modelPath; }

    void setSettings(const LLMSettings &settings);
    LLMSettings getSettings() const { return m_settings; }

    void requestStop();
    bool isGenerating() const { return m_isGenerating.load(); }

    /// Prompt families we format by hand, detected from the GGUF's embedded
    /// chat template. llama.cpp's built-in llama_chat_apply_template does not
    /// run Jinja, knows no Gemma 4, and cannot switch a hybrid model's
    /// thinking off — so the families we ship are formatted here.
    enum class PromptFamily { Unknown, ChatML, Gemma, Gemma4, Llama3 };
    static PromptFamily detectPromptFamily(const QString &embeddedTemplate);
    /// The model's own chat format for one system + user turn, ending at the
    /// start of the assistant reply. Hybrid "thinking" models (Qwen 3.5/3.6)
    /// get an empty <think></think> block so they answer directly — the
    /// agent's token budget is for JSON, not reasoning. Empty for Unknown.
    static QString formatChatPrompt(PromptFamily family, const QString &embeddedTemplate,
                                    const QString &systemPrompt, const QString &userPrompt);
    /// Remove reasoning a model emitted anyway (<think>…</think>, Gemma 4's
    /// <|channel>thought…<channel|>), including an unterminated block that
    /// ran into the token limit.
    static QString stripReasoning(const QString &text);

public slots:
    void generate(const QString &systemPrompt, const QString &userPrompt, int maxTokensOverride = 0);

signals:
    void modelLoaded(const QString &modelPath);
    /// The context window actually created — the requested size clamped to
    /// the model's training limit. Prompts are budgeted against THIS.
    void contextReady(int nCtx);
    void modelLoadError(const QString &error);
    void modelUnloaded();

    void generationStarted();
    void generationProgress(const QString &partialText, float progress);
    void generationCompleted(const QString &fullText);
    void generationError(const QString &error);
    void generationStopped();

private:
    QString m_modelPath;
    LLMSettings m_settings;
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_isGenerating{false};
    std::atomic<bool> m_isModelLoaded{false};  // Atomic flag for lock-free checking
    bool m_backendInitialized{false};
    mutable QMutex m_mutex;

    // Internal unload that doesn't acquire mutex (for use when mutex is already held)
    void unloadModelInternal();

#ifdef ENABLE_LOCAL_LLM
    llama_model *m_model = nullptr;
    llama_context *m_ctx = nullptr;
    int m_nCtx = 0;   // llama_n_ctx(m_ctx) after initialization
    const llama_vocab *m_vocab = nullptr;
    std::vector<llama_token> m_prevTokens; // cached input tokens for KV-prefix reuse
    QString m_chatTemplate;                // the GGUF's tokenizer.chat_template (may be empty)
    QString buildPrompt(const QString &systemPrompt, const QString &userPrompt) const;

    bool initializeContext();
    void cleanupContext();
    std::vector<llama_token> tokenize(const QString &text, bool addBos = false);
    QString detokenize(const std::vector<llama_token> &tokens);
#endif
};

#endif // LLMWORKER_H
