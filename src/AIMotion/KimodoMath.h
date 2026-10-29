#pragma once

// Pure pieces of the native Kimodo runtime (no ONNX, no llama.cpp, no Ogre): each is a transcription of the
// Python it replaces, pinned by KimodoMath_test against values captured from Kimodo itself.
//
//   sanitizeText      kimodo/sanitize.py sanitize_text (Python str semantics on ASCII)
//   llm2vecPrompt     the LLM2Vec Llama-3-Instruct prompt + the pooled suffix (kimodo/model/llm2vec)
//   DdimSchedule      kimodo/model/diffusion.py space_timesteps + calc_diffusion_vars, float32
//   smoothSignal      kimodo/motion_rep/smooth_root.py smooth_signal (multigrid ADMM; banded solve)
//   quatFromMatrixWxyz / matrixFromQuatWxyz   kimodo/geometry.py (PyTorch3D) conversions

#include <QString>

#include <array>
#include <cstdint>
#include <vector>

namespace AIMotion::KimodoMath {

QString sanitizeText(const QString& text);

/// The text LLM2Vec feeds Llama-3-Instruct (with special tokens to parse) and the part of it the mean pooling
/// covers (the prompt plus <|eot_id|>, tokenized WITHOUT a BOS).
struct LlmPrompt {
    QString full;     ///< "<|start_header_id|>user<|end_header_id|>\n\n" + text + "<|eot_id|>"
    QString pooled;   ///< text + "<|eot_id|>"
};
LlmPrompt llm2vecPrompt(const QString& sanitizedText);

struct DdimSchedule {
    std::vector<int64_t> timesteps;     ///< use_timesteps: [i] is the model timestep of denoising step i
    std::vector<float> alphasCumprod;   ///< per step (clamped >= 1e-9)
    std::vector<float> alphasCumprodPrev;
};
/// `base` = alphas_cumprod of the full training schedule (kimodo_meta.json diffusion.alphasCumprodBase).
DdimSchedule ddimSchedule(const std::vector<float>& base, int steps);

/// One DDIM (eta = 0) update at step i: x_{t-1} from x_t and the guided clean prediction. Element-wise.
void ddimStep(const DdimSchedule& s, int i, const float* pred, float* x, size_t n);

/// A planar path, one (x, z) per frame.
using Signal = std::vector<std::array<float, 2>>;

/// Kimodo's root path smoother on the x/z plane. `margin` = 0.06 m in Kimodo.
Signal smoothSignal(const Signal& x, float margin);

/// PyTorch3D matrix_to_quaternion: row-major 3x3 -> (w, x, y, z), not sign-normalised (bit-compatible branch choice).
std::array<float, 4> quatFromMatrixWxyz(const float m[9]);
void matrixFromQuatWxyz(const std::array<float, 4>& q, float out[9]);

} // namespace AIMotion::KimodoMath
