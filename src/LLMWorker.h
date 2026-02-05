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
    int contextSize = 4096;
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

public slots:
    void generate(const QString &systemPrompt, const QString &userPrompt);

signals:
    void modelLoaded(const QString &modelPath);
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
    const llama_vocab *m_vocab = nullptr;

    bool initializeContext();
    void cleanupContext();
    std::vector<llama_token> tokenize(const QString &text, bool addBos = false);
    QString detokenize(const std::vector<llama_token> &tokens);
#endif
};

#endif // LLMWORKER_H
