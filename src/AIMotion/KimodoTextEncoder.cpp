#include "KimodoTextEncoder.h"
#include "KimodoMath.h"

#ifdef ENABLE_LOCAL_LLM
#include <llama.h>
#endif

#include <QByteArray>

#include <algorithm>
#include <memory>
#include <mutex>

namespace AIMotion {

bool KimodoTextEncoder::available()
{
#ifdef ENABLE_LOCAL_LLM
    return true;
#else
    return false;
#endif
}

#ifdef ENABLE_LOCAL_LLM
namespace {

constexpr int kMaxLength = 512;      // LLM2Vec max_length (tokenizer truncation)
constexpr int kDocMaxLength = 400;   // LLM2Vec doc_max_length: longer prompts are cut by words first

std::vector<llama_token> tokenize(const llama_vocab* vocab, const QString& text, bool addSpecial)
{
    const QByteArray utf8 = text.toUtf8();
    std::vector<llama_token> ids(size_t(utf8.size()) + 8);
    int n = llama_tokenize(vocab, utf8.constData(), int(utf8.size()), ids.data(), int(ids.size()), addSpecial, true);
    if (n < 0) {
        ids.resize(size_t(-n));
        n = llama_tokenize(vocab, utf8.constData(), int(utf8.size()), ids.data(), int(ids.size()), addSpecial, true);
    }
    ids.resize(size_t(std::max(n, 0)));
    return ids;
}

// LLM2Vec._convert_to_str: drop trailing words until the prompt fits doc_max_length tokens.
QString fitDocLength(const llama_vocab* vocab, QString text)
{
    int n = int(tokenize(vocab, text, false).size());
    while (n > kDocMaxLength) {
        const auto words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const auto keep = int(double(words.size()) * (double(kDocMaxLength) / double(n)));
        text = words.mid(0, keep).join(QLatin1Char(' '));
        n = int(tokenize(vocab, text, false).size());
    }
    return text;
}

struct ModelDeleter { void operator()(llama_model* m) const { if (m) llama_model_free(m); } };
struct ContextDeleter { void operator()(llama_context* c) const { if (c) llama_free(c); } };
struct BatchGuard {
    llama_batch batch;
    explicit BatchGuard(int n) : batch(llama_batch_init(n, 0, 1)) {}
    BatchGuard(const BatchGuard&) = delete;
    BatchGuard& operator=(const BatchGuard&) = delete;
    ~BatchGuard() { llama_batch_free(batch); }
};

// One prompt -> its LLM2Vec embedding: mean of the last hidden states over `{prompt}<|eot_id|>` (skip_instruction).
bool encodeOne(llama_context* ctx, const llama_vocab* vocab, int dim, const QString& raw, std::vector<float>* out, QString* error)
{
    out->assign(size_t(dim), 0.0f);
    if (raw.trimmed().isEmpty()) return true;   // Kimodo zeroes the features of an empty prompt
    const KimodoMath::LlmPrompt p = KimodoMath::llm2vecPrompt(fitDocLength(vocab, raw));
    std::vector<llama_token> ids = tokenize(vocab, p.full, true);
    if (int(ids.size()) > kMaxLength) ids.resize(size_t(kMaxLength));
    const int pooled = std::min<int>(int(tokenize(vocab, p.pooled, false).size()), int(ids.size()));
    if (pooled <= 0) {
        *error = QStringLiteral("the prompt tokenized to nothing");
        return false;
    }
    llama_memory_clear(llama_get_memory(ctx), true);
    BatchGuard guard(int(ids.size()));
    llama_batch& batch = guard.batch;
    for (size_t i = 0; i < ids.size(); ++i) {
        batch.token[i] = ids[i];
        batch.pos[i] = llama_pos(i);
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = 0;
        batch.logits[i] = true;   // per-token embeddings (pooling NONE) are produced for output tokens only
    }
    batch.n_tokens = int(ids.size());
    if (const int rc = llama_decode(ctx, batch); rc != 0) {
        *error = QStringLiteral("llama_decode failed (%1) in the Kimodo text encoder").arg(rc);
        return false;
    }
    std::vector<double> acc(size_t(dim), 0.0);
    for (int i = int(ids.size()) - pooled; i < int(ids.size()); ++i) {
        const float* e = llama_get_embeddings_ith(ctx, i);
        if (!e) {
            *error = QStringLiteral("llama.cpp returned no embedding for token %1").arg(i);
            return false;
        }
        for (int k = 0; k < dim; ++k) acc[size_t(k)] += double(e[k]);
    }
    for (int k = 0; k < dim; ++k) (*out)[size_t(k)] = float(acc[size_t(k)] / double(pooled));
    return true;
}

} // namespace
#endif

KimodoTextEncoder::Result KimodoTextEncoder::encode(const QString& ggufPath, const QStringList& texts, int gpuLayers,
                                                    const std::function<bool()>& cancelled)
{
    Result r;
#ifndef ENABLE_LOCAL_LLM
    Q_UNUSED(ggufPath); Q_UNUSED(texts); Q_UNUSED(gpuLayers); Q_UNUSED(cancelled);
    r.error = QStringLiteral("the native Kimodo text encoder needs llama.cpp (rebuild with -DENABLE_LOCAL_LLM=ON)");
    return r;
#else
    static std::once_flag backendOnce;
    std::call_once(backendOnce, [] { llama_backend_init(); });

    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = gpuLayers;
    std::unique_ptr<llama_model, ModelDeleter> model(llama_model_load_from_file(ggufPath.toUtf8().constData(), mp));
    if (!model) {
        r.error = QStringLiteral("could not load the Kimodo text encoder %1").arg(ggufPath);
        return r;
    }
    const llama_vocab* vocab = llama_model_get_vocab(model.get());
    const int dim = llama_model_n_embd(model.get());

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = kMaxLength;
    cp.n_batch = kMaxLength;
    cp.n_ubatch = kMaxLength;   // non-causal attention: the whole prompt must be one micro-batch
    cp.embeddings = true;
    cp.pooling_type = LLAMA_POOLING_TYPE_NONE;
    cp.attention_type = LLAMA_ATTENTION_TYPE_NON_CAUSAL;
    std::unique_ptr<llama_context, ContextDeleter> ctx(llama_init_from_model(model.get(), cp));
    if (!ctx) {
        r.error = QStringLiteral("could not create a llama.cpp context for the Kimodo text encoder");
        return r;
    }
    llama_set_causal_attn(ctx.get(), false);

    for (const QString& raw : texts) {
        if (cancelled && cancelled()) {
            r.error = QStringLiteral("cancelled");
            return r;
        }
        std::vector<float> out;
        if (!encodeOne(ctx.get(), vocab, dim, raw, &out, &r.error)) return r;
        r.embeddings.push_back(std::move(out));
    }
    r.ok = true;
    return r;
#endif
}

} // namespace AIMotion
