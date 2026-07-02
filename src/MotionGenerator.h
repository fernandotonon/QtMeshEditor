#ifndef MOTION_GENERATOR_H
#define MOTION_GENERATOR_H

#include <QString>
#include <array>
#include <vector>

#include "MotionLibrary.h"   // reuse MotionLibrary::Clip as the output type

// EXPERIMENTAL from-scratch text-to-motion MODEL (#411). The shipped, reliable
// default is the template-clip retarget (MotionLibrary); this is an opt-in
// ALTERNATIVE motion source behind `--model` (CLI) / `model:true` (MCP) / the
// GUI checkbox. It runs a small ONNX model (t2m.onnx, ~17 MB) trained from
// scratch on CLEAN, dynamic, single-action CMU MoCap + Quaternius CC0 windows
// (commercial-OK; AMASS/HumanML3D excluded). The model emits a per-frame,
// 22-joint canonical LOCAL-quaternion clip — the SAME shape as a template clip —
// so it feeds AnimationMerger::applyMotionClip unchanged.
//
// QUALITY (honest, render-verified): locomotion (walk) is coherent; gestures
// (wave/run) can drift slightly in late frames. Hence experimental + the
// template retarget remains the default and the automatic fallback whenever the
// model is unavailable (no ENABLE_ONNX, model not downloaded, prompt action not
// in the model's vocab, or inference fails).
//
// ONNX contract (must match scripts/train-t2m-onnx-v3.py export):
//   input  "tokens" float32 [1, V]    one-hot/bag over the fixed action vocab
//   input  "seed"   float32 [1, Z]    latent (we pass zeros for the mean clip)
//   output "motion" float32 [1, T, C] C = 22*10 per-joint [t.xyz, q.xyzw, s.xyz]
// The accompanying t2m-vocab.json gives {vocab, Z, T, C, J, joints}.
class MotionGenerator {
public:
    MotionGenerator() = delete;

    // True only when built with ENABLE_ONNX. (Model file presence is checked
    // separately, per-call, against modelPath().)
    static bool isModelBackendAvailable();

    // Absolute path the model + vocab are cached at (AppData/ai_models/motion/).
    static QString modelPath();          // t2m.onnx
    static QString vocabPath();          // t2m-vocab.json
    static bool    modelPresent();       // both files exist on disk

    // Download the model+vocab on first use (blocks via a local event loop, like
    // the other predictors). Returns the model path on success, or empty when
    // offline / disabled / a download failed. Honours QTMESH_T2M_NO_DOWNLOAD and
    // base-URL override QTMESH_T2M_MODEL_BASE_URL / QSettings ai/t2mModelBaseUrl.
    static QString ensureModelBlocking();

    struct Result {
        bool ok = false;
        QString error;                   // populated when !ok
        QString matchedAction;           // vocab action the prompt resolved to
        MotionLibrary::Clip clip;        // the generated clip
        // Frame convention of clip.quats, from the vocab json's "frame" key:
        // true = WORLD-space joint orientations (v4 models — rides the same
        // superior retarget path as the v3 template library), false = LOCAL
        // parent-relative quats (v3 models). Pass to applyMotionClip.
        bool worldFrame = false;
    };

    // Generate a canonical clip for `prompt` using the model at `modelPath`
    // (with `vocabPath`). Maps the prompt to the nearest vocab action (keyword +
    // the same synonyms MotionLibrary uses), runs the model, and packs the
    // 22-joint per-frame quats into a MotionLibrary::Clip. `durationSec` (>0)
    // retimes the fixed-length output by frame stride/pad, mirroring the
    // template path. Returns ok=false (so the caller falls back to the template
    // library) when ONNX is off, the model/vocab are missing, the action is not
    // in the model vocab, or inference fails. Never throws.
    static Result generate(const QString& prompt,
                           const QString& modelPath,
                           const QString& vocabPath,
                           double durationSec = 0.0);
};

#endif // MOTION_GENERATOR_H
