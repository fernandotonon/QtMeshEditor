#ifndef UNIRIG_PREDICTOR_H
#define UNIRIG_PREDICTOR_H

#include <QString>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

// UniRig ML skeleton prediction (issue #408 retarget — replaces RigNetPredictor),
// the second ONNX consumer after PbrMapSynth (#404). Ogre-free + unit-testable:
// takes a mesh (positions + triangle indices), runs the UniRig encoder+decoder
// ONNX models, and returns predicted joint positions with a parent-index
// hierarchy.
//
// UniRig (Zhang et al., SIGGRAPH 2025, "One Model to Rig Them All: Diverse
// Skeleton Rigging with UniRig", VAST-AI-Research/UniRig — MIT code + MIT
// weights on HF VAST-AI/UniRig, trained on Articulation-XL2.0 / CC-BY-4.0)
// predicts an articulated skeleton from raw geometry — no template, so it
// handles arbitrary topology / non-humanoid shapes better than the #407
// Pinocchio-style template embedding. We port only its SKELETON-PREDICTION
// stage.
//
// **Architecture (skeleton stage).**
//   * Encoder (encoder.onnx): a Michelangelo SAL perceiver (3DShape2VecSet).
//     Inputs are a surface point cloud `pc [1,N,3]` and per-point features
//     `feats [1,N,3]` (the point normals). Internally: FourierEmbedder(pc)
//     concatenated with feats → Linear(input_proj) → a ResidualCrossAttention
//     block with learned latent queries → `num_latents` latent tokens of the
//     encoder width, projected (nn.Linear) into the decoder's hidden size.
//     Output is the latent PREFIX used to condition the autoregressive decoder.
//   * Decoder (decoder.onnx): a HuggingFace AutoModelForCausalLM (~350M,
//     config unirig_ar_350m_1024_81920) driven autoregressively with a manual
//     KV-cache. The reference repo decodes with beam search + sampling
//     (num_beams=15, top_k=5, top_p=0.95, repetition_penalty=3.0,
//     temperature=1.5); for the C++ port we use a GREEDY / CONSTRAINED decode
//     (argmax over the next-possible-token validity mask from the tokenizer
//     FSM) for determinism + exportability — a documented simplification that
//     still yields a valid skeleton tree.
//
// **Two model files.** Unlike RigNet's single graph, UniRig ships an encoder
// and a decoder. Both live under AppData/ai_models/unirig/ as encoder.onnx and
// decoder.onnx; `ensureModelBlocking()` returns the encoder path only when BOTH
// are present (downloading whichever is missing). The whole file is
// `ENABLE_ONNX`-guarded; without it `isAvailable()` is false and `predict()`
// fails with a "rebuild with -DENABLE_ONNX" message.
//
// Coordinate space: positions are mesh-local (the same space AutoRig::fitTemplate
// works in); predicted joints come back in that space. predict() normalises into
// a centred unit box [-1,1] for the model and de-normalises the result, so
// callers never see the model's internal scale.
class UniRigPredictor {
public:
    /// #1013: how predicted joints are NAMED.
    ///   Humanoid — always the geometric humanoid labeller (Hips/Spine/Left…),
    ///              the pre-#1013 behaviour; stamps humanoid names on cars.
    ///   Generic  — hierarchical neutral names (root, bone_01, …).
    ///   Auto     — humanoid names only when the labeller finds a plausible
    ///              humanoid (detected up == the given up axis, exactly one
    ///              left + one right arm chain and leg chain); else Generic.
    enum class Labeling { Auto, Humanoid, Generic };

    struct Options {
        // Out-of-line ctor so the `{}` default arg on predict() resolves to a
        // constructor call, not class-definition-time aggregate init of this
        // nested struct while UniRigPredictor is still incomplete (which the
        // compiler rejects). Same idiom as AutoRig::Options / RigNet's.
        Options();
        // Coarse cap on emitted joints. UniRig's decode terminates on the EOS
        // token; this is a safety bound (decode also stops at 2048 tokens).
        int maxJoints = 256;
        // Surface sample count for the encoder (0 = auto — full budget on small
        // meshes, scaled down on very large ones to reduce encoder peak RAM).
        int numSamples = 0;
        // Up axis: 0=X, 1=Y, 2=Z (default +Y). UniRig is trained +Y-up; a
        // non-Y up axis is rotated into +Y for inference and back out after.
        int upAxis = 1;
        // #1046: which points occupy the encoder's frozen query slots.
        //   Fps    — farthest-point sample front-loaded (the paper's sampling;
        //            rescued a biped rat 6→24 joints and a car's wheel clusters)
        //   Random — the surface sample as drawn (a tree got 64 branch joints
        //            here where Fps collapsed it to a 7-joint stick)
        //   Both   — run both and keep the richer skeleton (tie → Fps). Costs a
        //            second decode; the default because neither ordering wins
        //            across categories. Env override: QTMESH_UNIRIG_QUERIES=fps|random|both.
        enum class QuerySampling { Random, Fps, Both };
        QuerySampling querySampling = QuerySampling::Both;
        // #1013: joint naming policy (see Labeling). AutoRig maps --skeleton
        // humanoid/biped → Auto and quadruped/generic → Generic.
        Labeling labeling = Labeling::Auto;
    };

    struct Joint {
        QString name;                            // synthesised ("joint_0", "root", …)
        int     parent = -1;                     // index into joints (-1 = root)
        std::array<double, 3> pos = {0, 0, 0};   // mesh-local position
        int     part   = 0;                      // UniRig part: 0=body,1=hand,-1=none
    };

    struct Result {
        bool ok = false;
        QString error;                           // populated when !ok
        std::vector<Joint> joints;               // parent-ordered (root first)
        QString querySampling;                   // "fps" | "random" — which ordering produced it (#1046)
        int alternativeJoints = -1;              // joint count of the discarded ordering under Both, else -1
        QString labeling;                        // "humanoid" | "generic" — naming actually applied (#1013)
    };
    /// #1046: choose between the two query-sampling runs — the one that decoded
    /// (if only one did), else the richer skeleton, tie → `fps` (the paper's
    /// sampling). On every mesh measured the visibly better rig was the one
    /// with more joints; the decode under-produces rather than over-produces.
    static const Result& pickRicher(const Result& fps, const Result& random);

    // True only when built with ENABLE_ONNX. (Model presence is checked per
    // call against the two model paths.)
    static bool isAvailable();

    // Absolute path the UniRig encoder ONNX model is expected at
    // (AppData/ai_models/unirig/encoder.onnx). Same per-user cache convention
    // as the #404 PBR models. modelPath() is kept as an alias of the encoder
    // path so callers written against the single-path RigNet API still link.
    // UniRig's exported skeleton stage is THREE ONNX files (verified against the
    // VAST-AI/UniRig checkpoint): the Michelangelo encoder, the OPT-350m decoder
    // (inputs_embeds + KV-cache), and a token-embedding lookup the decode loop
    // uses to embed each generated token (the decoder takes embeddings, not ids).
    static QString modelPath();          // == encoderModelPath() (legacy alias)
    static QString encoderModelPath();
    static QString decoderModelPath();   // AppData/ai_models/unirig/decoder.onnx
    static QString embedModelPath();     // AppData/ai_models/unirig/embed.onnx
    // True when all three model files already exist on disk (no download needed).
    static bool modelsPresent();
    /// #1025: published SHA-256 (HF LFS oid) of one of the three default-hosted
    /// files ("encoder.onnx" / "decoder.onnx" / "embed.onnx"); empty for any
    /// other name. Applied by ensureModelBlocking() only when the base URL is
    /// the default hosting.
    static QString expectedSha256(const QString& fileName);

    // Encoder surface-sample budget. `requested` > 0 clamps to [4096, 65536];
    // 0 picks an automatic budget that scales down on very large meshes.
    static int sampleBudgetForMesh(int vertexCount, int requested = 0);
    /// #1046: reorder a point cloud (xyz triples in `pts`, parallel `nrm`) so
    /// its first `k` points are a greedy farthest-point sample of the whole
    /// cloud (FPS order), the rest following in their original order. The
    /// hosted encoder export froze its farthest-point-sampling indices at
    /// trace time — 4096 indices, 1794 unique, all < 2048 — so the perceiver's
    /// QUERY points are whatever happens to sit in the first 2048 input slots.
    /// Front-loading an FPS subset there restores the paper's coverage-
    /// maximising queries without a re-export. Pure; unit-tested.
    /// `pool` (default: all) limits the FPS CANDIDATES to the first `pool`
    /// points — upstream FPS-selects from ~8192 surface samples, and selecting
    /// from a 65536-point cloud pushes queries to extremities far harder than
    /// the model saw in training. Every point is kept either way.
    static void frontLoadFarthestPoints(std::vector<float>& pts, std::vector<float>& nrm,
                                        int count, int k, int pool = -1);


    // Ensure ALL THREE models exist on disk, downloading whichever is missing on
    // first use (blocks via a local event loop, like
    // AIAssistManager::ensureModelBlocking). Returns the encoder path on
    // success (all present), or empty when offline / disabled / a download
    // failed. Honours QTMESH_UNIRIG_NO_DOWNLOAD (tests/offline) and the
    // base-URL override QTMESH_UNIRIG_MODEL_BASE_URL / QSettings
    // ai/unirigModelBaseUrl.
    static QString ensureModelBlocking();

    // Progress/cancel callback for the (long) autoregressive decode. Invoked
    // once per generated token with (stepsDone, maxSteps); return false to
    // CANCEL the decode (predict then returns ok=false, error="cancelled").
    // The encode + decode run on whatever thread calls predict() — the GUI
    // runs it on a worker so the callback marshals progress to the UI thread.
    using ProgressFn = std::function<bool(int stepsDone, int maxSteps)>;

    // Run UniRig against the encoder + decoder + embed .onnx files. `positions`
    // is tightly packed xyz (3 floats/vertex); `indices` is triangle vertex
    // indices (3/face). `progress` (optional) is called per decode step and can
    // cancel. Returns predicted joints in mesh-local space, or ok=false with a
    // reason (missing/failed model, degenerate mesh, ONNX-disabled build,
    // cancelled) so the caller can fall back. Never throws.
    static Result predict(const float* positions, int vertexCount,
                          const uint32_t* indices, int indexCount,
                          const QString& encoderModelPath,
                          const QString& decoderModelPath,
                          const QString& embedModelPath,
                          const Options& opts = {},
                          const ProgressFn& progress = {});
private:
    /// #1046 Both mode: predict() once with querySampling forced to Fps and
    /// once to Random, keep the richer skeleton (pickRicher); a cancellation
    /// ends the whole call. Defined for every build; predict() handles the
    /// inference under ENABLE_ONNX.
    static Result predictBoth(const float* positions, int vertexCount,
                              const uint32_t* indices, int indexCount,
                              const QString& encoderModelPath,
                              const QString& decoderModelPath,
                              const QString& embedModelPath,
                              const Options& opts, const ProgressFn& progress);
public:
    /// Options::querySampling with the QTMESH_UNIRIG_QUERIES override applied —
    /// the env only NARROWS the default Both (an explicit Fps/Random wins). Pure.
    static Options::QuerySampling resolveQuerySampling(const Options& opts);

    // ---- Pure-data tokenizer helpers (no ONNX / no Ogre — unit-testable) ----
    // These replicate UniRig's src/tokenizer/tokenizer_part.py exactly and are
    // what `predict()` uses to turn the decoded token stream into a skeleton, so
    // they're exercised by the same code on every run AND directly in tests.

    // undiscretize a coordinate bin [0..255] back to a continuous value in
    // [-1,1]:  (bin + 0.5) / 256 * (hi-lo) + lo,  with lo=-1, hi=1.
    static double undiscretize(int bin);

    // Run the detokenizer FSM over a token id stream (leading BOS / trailing PAD
    // tolerated; terminal EOS dropped). Returns joints in mesh-local space as
    // `undiscretize(bin) * scale + centre`, with resolved parent indices
    // (root = -1), parent-before-child ordered. ok=false on a malformed stream.
    // NOTE: works in the model's axis convention — callers that rotated the up
    // axis for inference must rotate the returned joints back themselves.
    static Result detokenize(const std::vector<int>& ids,
                             double scale,
                             const std::array<double, 3>& centre,
                             Labeling labeling = Labeling::Humanoid);

    // Assign anatomical bone names (Hips / Spine / Neck / Head / {Left,Right}
    // {Arm,ForeArm,Hand,UpLeg,Leg,Foot}, Mixamo-style) to a predicted joint set
    // by REST-POSE GEOMETRY — UniRig emits positional names (joint_N) with no
    // semantics or L/R, so downstream tooling that keys off bone names (the
    // text-to-motion canonical mapper, segmentation, etc.) resolves nothing.
    // Pure-data + unit-testable. `upAxis` (0=X,1=Y,2=Z, default +Y) is the body's
    // up direction; the character's LEFT is +X by convention. Joints it can't
    // confidently classify keep their original name. Mutates `joints[].name`.
    /// Returns whether the result is a PLAUSIBLE humanoid (#1013): the axis it
    /// detected as "up" agrees with `upAxis`, and exactly one Left and one
    /// Right chain were classified as arm and as leg. A car (its long axis is
    /// mistaken for the spine) and a tree (many "arms") both return false.
    static bool labelJointsAnatomically(std::vector<Joint>& joints, int upAxis = 1);
    /// #1013: neutral hierarchical names — the root "root", every other joint
    /// "bone_NN" in parent-before-child order. Unique by construction. Pure.
    static void labelJointsGeneric(std::vector<Joint>& joints);
    /// #1013: apply `labeling` — returns the naming actually used
    /// ("humanoid" | "generic").
    static QString applyLabeling(std::vector<Joint>& joints, int upAxis, Labeling labeling);
};

#endif // UNIRIG_PREDICTOR_H

