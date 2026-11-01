#pragma once

// Kimodo's text encoder (LLM2Vec over Meta-Llama-3-8B-Instruct) run through the editor's bundled llama.cpp:
// the "Llama-3-LLM2Vec-8B-Kimodo-TextEncoder" GGUF (scripts/export-kimodo-text-encoder-gguf.py) evaluated with
// bidirectional attention, per-token embeddings, and LLM2Vec's mean pooling over the prompt's own tokens
// (skip_instruction). Measured against the PyTorch bf16 encoder: cosine 0.9999 on every test prompt.
//
// Encode-then-free: the ~8.5 GB model is loaded for one call and released before the motion model runs.

#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace AIMotion {

class KimodoTextEncoder {
public:
    struct Result {
        bool ok = false;
        QString error;
        std::vector<std::vector<float>> embeddings;   ///< one [4096] vector per text; all zeros for an empty text
    };

    /// Built with llama.cpp (ENABLE_LOCAL_LLM).
    static bool available();

    /// `texts` are already sanitized (KimodoMath::sanitizeText). `cancelled` is polled between prompts.
    static Result encode(const QString& ggufPath, const QStringList& texts, int gpuLayers = 99,
                         const std::function<bool()>& cancelled = {});
};

} // namespace AIMotion
