---
license: other
license_name: nvidia-open-model-license
license_link: https://huggingface.co/fernandotonon/QtMeshEditor-kimodo-onnx/blob/main/LICENSE-NVIDIA-OML.txt
base_model: nvidia/Kimodo-SOMA-RP-v1.2
tags:
  - onnx
  - kimodo
  - text-to-motion
  - motion-generation
---

# Kimodo-SOMA-RP-v1.2 (ONNX)

Licensed by NVIDIA Corporation under the NVIDIA Open Model License (`LICENSE-NVIDIA-OML.txt`, `NOTICE-Kimodo.txt`).

An ONNX conversion of [NVIDIA Kimodo-SOMA-RP-v1.2](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1.2) for
[QtMeshEditor](https://github.com/fernandotonon/QtMeshEditor), which runs Kimodo's text-to-motion natively
(ONNX Runtime + llama.cpp, no Python). Made by
[`scripts/export-kimodo-onnx.py`](https://github.com/fernandotonon/QtMeshEditor/blob/master/scripts/export-kimodo-onnx.py):

| File | Contents |
|---|---|
| `kimodo_denoiser.onnx` | the two-stage transformer denoiser (batch 1, dynamic frame count) |
| `kimodo_decode.onnx` | features -> SOMA-30 rotations, root, contacts |
| `kimodo_encode.onnx` | SOMA-30 rotations + root (+ smoothed root) -> features |
| `kimodo_transition.onnx` | the multi-segment transition conditioning |
| `kimodo_meta.json` | normalization statistics, diffusion schedule, skeletons, post-processing rig |

The sampling loop (DDIM, classifier-free guidance), segment stitching, root smoothing and NVIDIA's
MotionCorrection post-processing run in C++. Verified against Kimodo's own PyTorch generation on the same text
features and noise: max joint rotation error 2e-4, root 0.003 cm, identical foot contacts.

The text encoder lives in its own repository (a Meta Llama 3 derivative):
[Llama-3-LLM2Vec-8B-Kimodo-TextEncoder-GGUF](https://huggingface.co/fernandotonon/Llama-3-LLM2Vec-8B-Kimodo-TextEncoder-GGUF).
