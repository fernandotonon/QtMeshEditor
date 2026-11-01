---
license: llama3
license_name: meta-llama-3-community-license
license_link: https://huggingface.co/fernandotonon/Llama-3-LLM2Vec-8B-Kimodo-TextEncoder-GGUF/blob/main/LICENSE-Llama-3.txt
base_model:
  - meta-llama/Meta-Llama-3-8B-Instruct
  - McGill-NLP/LLM2Vec-Meta-Llama-3-8B-Instruct-mntp
  - McGill-NLP/LLM2Vec-Meta-Llama-3-8B-Instruct-mntp-supervised
tags:
  - llama-3
  - llm2vec
  - text-embeddings
  - gguf
  - kimodo
  - text-to-motion
library_name: llama.cpp
---

# Llama-3-LLM2Vec-8B-Kimodo-TextEncoder (GGUF, Q8_0)

**Built with Meta Llama 3.**

The text encoder of [NVIDIA Kimodo](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1.2) packaged for
[QtMeshEditor](https://github.com/fernandotonon/QtMeshEditor), which runs Kimodo's text-to-motion natively
(no Python) through its bundled llama.cpp.

## What it is

[Meta-Llama-3-8B-Instruct](https://huggingface.co/meta-llama/Meta-Llama-3-8B-Instruct) with the
[LLM2Vec](https://github.com/McGill-NLP/llm2vec) MNTP and supervised LoRA adapters
(`McGill-NLP/LLM2Vec-Meta-Llama-3-8B-Instruct-mntp`, `-mntp-supervised`, MIT) merged into its weights, saved as
a headless bidirectional Llama (`llama-embed` architecture, `attention.causal = false`), converted with
llama.cpp's `convert_hf_to_gguf.py` and quantized to Q8_0. Built by
[`scripts/export-kimodo-text-encoder-gguf.py`](https://github.com/fernandotonon/QtMeshEditor/blob/master/scripts/export-kimodo-text-encoder-gguf.py).

## How Kimodo uses it

LLM2Vec embeddings with mean pooling over the prompt's own tokens: the input is
`<|begin_of_text|><|start_header_id|>user<|end_header_id|>\n\n{prompt}<|eot_id|>` and the embedding is the mean
of the last hidden states of `{prompt}<|eot_id|>` (per-token embeddings, `pooling = none`, bidirectional
attention). Measured against the original bf16 PyTorch encoder: cosine similarity 0.9999 on every test prompt.

## Licence

This model is a derivative of Meta Llama 3 and is distributed under the
**Meta Llama 3 Community License** — see `LICENSE-Llama-3.txt`. Use must comply with the
**Meta Llama 3 Acceptable Use Policy** (`USE_POLICY-Llama-3.md`).

Meta Llama 3 is licensed under the Meta Llama 3 Community License, Copyright © Meta Platforms, Inc. All Rights
Reserved. (`NOTICE`)

The merged LLM2Vec adapters are MIT-licensed (`LICENSE-LLM2Vec-MIT.txt`, Copyright (c) 2024 McGill NLP).
