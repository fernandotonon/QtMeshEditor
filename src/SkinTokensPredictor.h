#ifndef SKINTOKENS_PREDICTOR_H
#define SKINTOKENS_PREDICTOR_H

#include <QString>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

// SkinTokens / TokenRig ML skin-weight prediction (issue #819 Slice C
// follow-up) — the ML path behind SkinWeights::Algorithm::SkinTokens
// (the default skinning algorithm; "unirig" is a deprecated alias).
// Ogre-free + unit-testable for its pure-data parts.
//
// SkinTokens (VAST-AI-Research/SkinTokens, MIT code + MIT weights on
// HF VAST-AI/SkinTokens; AR backbone Qwen3-0.6B, Apache-2.0) learns a
// compact DISCRETE representation of skin weights (an FSQ-CVAE
// "skin token" codebook) and models the whole rig autoregressively.
// It replaced UniRig's own skin head in our plan because that head
// runs PTv3 on spconv sparse convolutions — no ONNX lowering exists
// (see THIRD_PARTY_AI_MODELS.md decision record).
//
// **Pipeline (weights-only, skeleton teacher-forced).** We always
// have a skeleton (ours or #407/#408's), so the skeleton stream is
// TOKENIZED and fed as the prefix; the model only generates the skin
// tokens:
//   1. Surface-sample exactly `numPoints` points + normals,
//      normalise into the model's frame (manifest `continuous_range`).
//   2. mesh_cond.onnx  : points+normals → the LLM's mesh-conditioning
//      embedding prefix (Michelangelo + projection).
//   3. vae_cond.onnx   : [points|normals] → the skin decoder's
//      conditioning latents.
//   4. Tokenize the skeleton (DFS order, per-bone branch/coord
//      records discretised to `num_discrete` bins — the same
//      TokenizerPart stream UniRigPredictor::detokenize inverts),
//      embed via embed.onnx, run decoder.onnx over
//      [mesh_cond | bos, cls, skeleton…, switch-EOS] once to fill the
//      KV cache, then GREEDILY decode J×tokens_per_skin skin-token
//      ids constrained to the FSQ vocabulary range.
//   5. skin_decode.onnx (once per joint): that joint's skin-token
//      ids + the sampled cond + cond latents → per-sampled-point
//      weight column.
//   6. Transfer sampled→full-res vertices by 8-NN inverse-distance
//      (the upstream Asset.from_data recipe), top-K + normalise.
//
// **Model files** (AppData/ai_models/skintokens/): mesh_cond.onnx,
// vae_cond.onnx, embed.onnx, decoder.onnx, skin_decode.onnx +
// skintokens.json (the config manifest the export script writes —
// vocab layout, tokens_per_skin, point count, normalisation…). All
// produced by scripts/export-skintokens-onnx.py (offline dev tool)
// and downloaded on first use; base URL override
// QTMESH_SKINTOKENS_MODEL_BASE_URL / QSettings ai/skintokensModelBaseUrl,
// offline guard QTMESH_SKINTOKENS_NO_DOWNLOAD.
class SkinTokensPredictor {
public:
    struct Options {
        Options();          // out-of-line (same idiom as UniRigPredictor)
        // Bones per vertex kept after the transfer (hardware
        // convention 4; the Slice-B post-passes prune again anyway).
        int maxInfluencesPerVertex = 4;
    };

    // One joint of the skeleton to skin, in DFS order (parent always
    // before child, chains contiguous where possible).
    struct Joint {
        std::array<double, 3> pos = {0, 0, 0};   // mesh-local
        int parent = -1;                          // index into joints
    };

    struct Result {
        bool ok = false;
        QString error;
        // Per full-res vertex: up to maxInfluences (jointIndex,
        // weight) pairs, weights normalised. Joint indices refer to
        // the INPUT joints order.
        struct VertexWeights {
            int    jointIndices[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
            double weights[8]      = { 0, 0, 0, 0, 0, 0, 0, 0 };
            int    count           = 0;
        };
        std::vector<VertexWeights> weights;
    };

    // True only when built with ENABLE_ONNX.
    static bool isAvailable();

    static QString modelDir();          // AppData/ai_models/skintokens
    static QString manifestPath();      // …/skintokens.json
    static bool    modelsPresent();     // all 5 graphs + manifest on disk

    // Ensure every model file exists, downloading missing ones on
    // first use (blocking; QTMESH_SKINTOKENS_NO_DOWNLOAD guard).
    // Returns the manifest path on success, empty otherwise.
    static QString ensureModelBlocking();

    // Called once per decode step / per joint decode with
    // (stepsDone, maxSteps); return false to cancel.
    using ProgressFn = std::function<bool(int stepsDone, int maxSteps)>;

    // Predict per-vertex skin weights for `joints` on the mesh
    // (positions xyz-packed, triangle indices). Runs the five ONNX
    // graphs; returns ok=false with a reason on any failure (missing
    // models, non-ONNX build, degenerate input, cancel) — the caller
    // (SkinWeights) falls back to GeodesicVoxel.
    static Result predict(const float* positions, int vertexCount,
                          const std::uint32_t* indices, std::size_t indexCount,
                          const std::vector<Joint>& joints,
                          const Options& opts = {},
                          const ProgressFn& progress = {});

    // ── Pure-data helpers (unit-tested without ONNX) ────────────────
    // Discretize a coordinate into [0, numDiscrete) over `range`
    // (mirrors the upstream tokenizer's `discretize`).
    static int discretize(double t, double lo, double hi, int numDiscrete);

    // Serialize a DFS-ordered skeleton into the TokenizerPart stream:
    // [bos, cls] + per-bone records ([branch, p3, j3] on chain starts,
    // [j3] on continuations) + [eos]. Vocab ids per the manifest
    // values passed in. Returns empty on invalid input (parents out
    // of order / range).
    struct TokenizerLayout {
        int numDiscrete   = 256;
        double rangeLo    = -1.0, rangeHi = 1.0;
        int tokBranch = 256, tokBos = 257, tokEos = 258, tokPad = 259;
        // Class head after bos: tokCls when >= 0 (we use the
        // "articulation" class — the general Articulation-XL bucket
        // the demo feeds for arbitrary meshes), else tokClsNone,
        // else nothing.
        int tokCls     = -1;
        int tokClsNone = -1;
    };
    static std::vector<std::int64_t> tokenizeSkeleton(
        const std::vector<Joint>& joints, const TokenizerLayout& layout);

    // 8-NN inverse-distance transfer of per-sample weight columns to
    // full-res vertices (upstream Asset.from_data recipe).
    // sampleWeights is [numSamples × numJoints], row-major.
    static void transferWeights(
        const float* vertices, int vertexCount,
        const std::vector<std::array<float, 3>>& samplePositions,
        const std::vector<float>& sampleWeights, int numJoints,
        int maxInfluences,
        std::vector<Result::VertexWeights>& out);
};

#endif // SKINTOKENS_PREDICTOR_H
