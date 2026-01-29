#include "LLMWorker.h"
#include <QDebug>
#include <QThread>

LLMWorker::LLMWorker(QObject *parent)
    : QObject(parent)
{
#ifdef ENABLE_LOCAL_LLM
    // Initialize llama backend
    llama_backend_init();
    qDebug() << "LLMWorker: llama.cpp backend initialized";
#endif
}

LLMWorker::~LLMWorker()
{
    unloadModel();
#ifdef ENABLE_LOCAL_LLM
    llama_backend_free();
    qDebug() << "LLMWorker: llama.cpp backend freed";
#endif
}

bool LLMWorker::loadModel(const QString &modelPath)
{
#ifdef ENABLE_LOCAL_LLM
    // Stop any ongoing generation first
    if (m_isGenerating.load()) {
        m_stopRequested.store(true);
        // Wait briefly for generation to notice the stop request
        for (int i = 0; i < 100 && m_isGenerating.load(); ++i) {
            QThread::msleep(10);
        }
    }

    bool success = false;
    bool wasModelLoaded = false;
    QString error;

    {
        QMutexLocker locker(&m_mutex);

        // Use internal unload (doesn't try to reacquire mutex, doesn't emit signals)
        if (m_model) {
            wasModelLoaded = true;
            cleanupContext();
            llama_model_free(m_model);
            m_model = nullptr;
            m_vocab = nullptr;
            m_modelPath.clear();
            m_isModelLoaded.store(false);
            qDebug() << "LLMWorker: Previous model unloaded";
        }

        qDebug() << "LLMWorker: Loading model from" << modelPath;

        // Model parameters
        llama_model_params model_params = llama_model_default_params();
        model_params.n_gpu_layers = m_settings.gpuLayers;

        // Load the model
        m_model = llama_model_load_from_file(modelPath.toUtf8().constData(), model_params);
        if (!m_model) {
            error = QString("Failed to load model from: %1").arg(modelPath);
            qWarning() << "LLMWorker:" << error;
        } else {
            // Get the vocabulary
            m_vocab = llama_model_get_vocab(m_model);
            if (!m_vocab) {
                error = "Failed to get vocabulary from model";
                qWarning() << "LLMWorker:" << error;
                llama_model_free(m_model);
                m_model = nullptr;
            } else {
                // Initialize context
                if (!initializeContext()) {
                    llama_model_free(m_model);
                    m_model = nullptr;
                    m_vocab = nullptr;
                    error = "Failed to initialize context";
                } else {
                    m_modelPath = modelPath;
                    m_isModelLoaded.store(true);
                    success = true;
                    qDebug() << "LLMWorker: Model loaded successfully";
                }
            }
        }
    }
    // Mutex released - now safe to emit signals

    if (wasModelLoaded) {
        emit modelUnloaded();
    }

    if (success) {
        emit modelLoaded(modelPath);
    } else if (!error.isEmpty()) {
        emit modelLoadError(error);
    }

    return success;
#else
    Q_UNUSED(modelPath);
    emit modelLoadError("Local LLM support is not enabled. Rebuild with ENABLE_LOCAL_LLM=ON");
    return false;
#endif
}

void LLMWorker::unloadModel()
{
#ifdef ENABLE_LOCAL_LLM
    // Stop any ongoing generation first
    if (m_isGenerating.load()) {
        m_stopRequested.store(true);
        // Wait briefly for generation to notice the stop request
        for (int i = 0; i < 100 && m_isGenerating.load(); ++i) {
            QThread::msleep(10);
        }
    }

    bool wasLoaded = false;

    {
        QMutexLocker locker(&m_mutex);
        wasLoaded = (m_model != nullptr);
        unloadModelInternal();
    }
    // Mutex released - now safe to emit signals

    if (wasLoaded) {
        emit modelUnloaded();
    }
#endif
}

void LLMWorker::unloadModelInternal()
{
#ifdef ENABLE_LOCAL_LLM
    // This method assumes mutex is already held by caller
    // Does NOT emit signals - caller is responsible for that after releasing mutex
    cleanupContext();

    if (m_model) {
        llama_model_free(m_model);
        m_model = nullptr;
        m_vocab = nullptr;
        qDebug() << "LLMWorker: Model unloaded";
    }

    m_modelPath.clear();
    m_isModelLoaded.store(false);
#endif
}

bool LLMWorker::isModelLoaded() const
{
    // Use atomic flag for lock-free checking to avoid deadlocks
    return m_isModelLoaded.load();
}

void LLMWorker::setSettings(const LLMSettings &settings)
{
    QMutexLocker locker(&m_mutex);
    m_settings = settings;

#ifdef ENABLE_LOCAL_LLM
    // Reinitialize context if model is loaded and context size changed
    if (m_model) {
        cleanupContext();
        initializeContext();
    }
#endif
}

void LLMWorker::requestStop()
{
    m_stopRequested.store(true);
}

void LLMWorker::generate(const QString &systemPrompt, const QString &userPrompt)
{
#ifdef ENABLE_LOCAL_LLM
    if (!isModelLoaded()) {
        emit generationError("No model loaded");
        return;
    }

    m_stopRequested.store(false);
    m_isGenerating.store(true);
    emit generationStarted();

    QMutexLocker locker(&m_mutex);

    // Double-check model is still loaded after acquiring mutex
    // (model might have been unloaded while waiting for mutex)
    if (!m_model || !m_ctx || !m_vocab) {
        m_isGenerating.store(false);
        emit generationError("Model was unloaded");
        return;
    }

    // Build the full prompt with chat template
    QString fullPrompt;

    // Use a simple chat format that works with most models
    if (!systemPrompt.isEmpty()) {
        fullPrompt = QString("<|system|>\n%1\n<|user|>\n%2\n<|assistant|>\n")
                         .arg(systemPrompt)
                         .arg(userPrompt);
    } else {
        fullPrompt = QString("<|user|>\n%1\n<|assistant|>\n").arg(userPrompt);
    }

    qDebug() << "LLMWorker: Generating with prompt length:" << fullPrompt.length();

    // Tokenize input
    std::vector<llama_token> tokens = tokenize(fullPrompt, true);
    if (tokens.empty()) {
        m_isGenerating.store(false);
        emit generationError("Failed to tokenize prompt");
        return;
    }

    // Check if prompt fits in context
    if (static_cast<int>(tokens.size()) > m_settings.contextSize - 4) {
        m_isGenerating.store(false);
        emit generationError(QString("Prompt too long: %1 tokens, max: %2")
                                 .arg(tokens.size())
                                 .arg(m_settings.contextSize - 4));
        return;
    }

    // Clear the KV cache
    llama_memory_t mem = llama_get_memory(m_ctx);
    if (mem) {
        llama_memory_clear(mem, false);
    }

    // Process prompt in batches (n_batch = 512)
    const int n_batch = 512;
    int n_tokens = static_cast<int>(tokens.size());

    for (int i = 0; i < n_tokens; i += n_batch) {
        if (m_stopRequested.load()) {
            m_isGenerating.store(false);
            emit generationStopped();
            return;
        }

        int n_eval = std::min(n_batch, n_tokens - i);
        llama_batch batch = llama_batch_get_one(tokens.data() + i, n_eval);

        if (llama_decode(m_ctx, batch) != 0) {
            m_isGenerating.store(false);
            emit generationError("Failed to decode prompt");
            return;
        }
    }

    // Sampling parameters
    llama_sampler *sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(m_settings.temperature));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_k(m_settings.topK));
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(m_settings.topP, 1));
    llama_sampler_chain_add(sampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    // Generate tokens
    QString generatedText;
    std::vector<llama_token> generatedTokens;
    int n_cur = static_cast<int>(tokens.size());

    llama_token eosToken = llama_vocab_eot(m_vocab);
    if (eosToken == LLAMA_TOKEN_NULL) {
        eosToken = llama_vocab_eos(m_vocab);
    }

    for (int i = 0; i < m_settings.maxTokens; ++i) {
        if (m_stopRequested.load()) {
            qDebug() << "LLMWorker: Generation stopped by user";
            emit generationStopped();
            llama_sampler_free(sampler);
            m_isGenerating.store(false);
            return;
        }

        // Sample next token
        llama_token newToken = llama_sampler_sample(sampler, m_ctx, -1);

        // Check for end of generation
        if (llama_vocab_is_eog(m_vocab, newToken)) {
            break;
        }

        generatedTokens.push_back(newToken);

        // Decode token to text
        char buf[256];
        int n = llama_token_to_piece(m_vocab, newToken, buf, sizeof(buf), 0, true);
        if (n > 0) {
            QString piece = QString::fromUtf8(buf, n);
            generatedText += piece;

            // Emit progress
            float progress = static_cast<float>(i + 1) / m_settings.maxTokens;
            emit generationProgress(generatedText, progress);
        }

        // Prepare next batch
        llama_batch nextBatch = llama_batch_get_one(&newToken, 1);
        if (llama_decode(m_ctx, nextBatch) != 0) {
            emit generationError("Failed to decode token");
            break;
        }

        n_cur++;
    }

    llama_sampler_free(sampler);
    m_isGenerating.store(false);

    qDebug() << "LLMWorker: Generation completed, tokens:" << generatedTokens.size();
    emit generationCompleted(generatedText.trimmed());
#else
    Q_UNUSED(systemPrompt);
    Q_UNUSED(userPrompt);
    emit generationError("Local LLM support is not enabled");
#endif
}

#ifdef ENABLE_LOCAL_LLM
bool LLMWorker::initializeContext()
{
    if (!m_model) {
        return false;
    }

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = m_settings.contextSize;
    ctx_params.n_batch = 512;
    ctx_params.n_threads = m_settings.threads > 0 ? m_settings.threads : QThread::idealThreadCount();
    ctx_params.n_threads_batch = ctx_params.n_threads;

    m_ctx = llama_init_from_model(m_model, ctx_params);
    if (!m_ctx) {
        qWarning() << "LLMWorker: Failed to create context";
        emit modelLoadError("Failed to create context");
        return false;
    }

    qDebug() << "LLMWorker: Context initialized with" << m_settings.contextSize << "tokens,"
             << ctx_params.n_threads << "threads";
    return true;
}

void LLMWorker::cleanupContext()
{
    if (m_ctx) {
        llama_free(m_ctx);
        m_ctx = nullptr;
    }
}

std::vector<llama_token> LLMWorker::tokenize(const QString &text, bool addBos)
{
    if (!m_vocab) {
        return {};
    }

    QByteArray utf8 = text.toUtf8();
    int n_tokens = utf8.length() + (addBos ? 1 : 0);
    std::vector<llama_token> tokens(n_tokens);

    n_tokens = llama_tokenize(m_vocab, utf8.constData(), utf8.length(),
                              tokens.data(), static_cast<int>(tokens.size()),
                              addBos, true);

    if (n_tokens < 0) {
        tokens.resize(-n_tokens);
        n_tokens = llama_tokenize(m_vocab, utf8.constData(), utf8.length(),
                                  tokens.data(), static_cast<int>(tokens.size()),
                                  addBos, true);
    }

    if (n_tokens >= 0) {
        tokens.resize(n_tokens);
    } else {
        tokens.clear();
    }

    return tokens;
}

QString LLMWorker::detokenize(const std::vector<llama_token> &tokens)
{
    if (!m_vocab || tokens.empty()) {
        return QString();
    }

    QString result;
    char buf[256];

    for (llama_token token : tokens) {
        int n = llama_token_to_piece(m_vocab, token, buf, sizeof(buf), 0, true);
        if (n > 0) {
            result += QString::fromUtf8(buf, n);
        }
    }

    return result;
}
#endif
