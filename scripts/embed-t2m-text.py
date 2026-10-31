#!/usr/bin/env python3
"""Embed text-to-motion prompts with Qwen3-Embedding-0.6B (dev tool, not shipped).

Runs the GGUF through llama.cpp's llama-embedding (the same library the editor bundles, so the training
vectors match what the app computes at run time): Qwen3-Embedding's query format
"Instruct: <task>\\nQuery:<text>", last-token pooling, L2-normalised, 1024 floats per prompt.

    python3 scripts/embed-t2m-text.py --llama-embedding <llama.cpp>/bin/llama-embedding \
        --model Qwen3-Embedding-0.6B-Q8_0.gguf --texts texts.txt --out embeddings.npz

--texts: one prompt per line. Output: npz with `text` (str array) and `emb` [N,1024] float32.
"""

import argparse
import json
import subprocess
import sys

import numpy as np

# Keep in sync with the C++ encoder (the task line is part of the model input).
INSTRUCT = "Instruct: Given a description of a human body motion, retrieve motions that match it\nQuery:"
SEP = "<#qtmesh-sep#>"


def _run(llama_embedding, model, chunk):
    """One llama-embedding call; None when its stdout is cut short.

    llama-embedding prints the JSON through llama.cpp's asynchronous logger and can exit (code 0) before
    the log queue is flushed, so a large batch sometimes comes back truncated at a random byte.
    """
    prompt = SEP.join(INSTRUCT + t for t in chunk)
    r = subprocess.run([llama_embedding, "-m", model, "--pooling", "last", "--embd-normalize", "2",
                        "--embd-separator", SEP, "--embd-output-format", "json", "-c", "8192",
                        "-b", "8192", "-ub", "8192", "-p", prompt],
                       capture_output=True, text=True, check=False)
    if r.returncode != 0:
        sys.exit(f"llama-embedding failed:\n{r.stderr[-2000:]}")
    try:
        data = json.loads(r.stdout[r.stdout.index("{"):])["data"]
    except (ValueError, KeyError):
        return None
    if len(data) != len(chunk):
        return None
    return [d["embedding"] for d in sorted(data, key=lambda d: d["index"])]


def embed(llama_embedding, model, texts, batch=64):
    out = []
    for i in range(0, len(texts), batch):
        pending = [texts[i:i + batch]]
        while pending:
            chunk = pending.pop(0)
            got = None
            for _ in range(3):
                got = _run(llama_embedding, model, chunk)
                if got is not None:
                    break
            if got is None:
                if len(chunk) == 1:
                    sys.exit(f"llama-embedding output unreadable for: {chunk[0]!r}")
                h = len(chunk) // 2
                pending[:0] = [chunk[:h], chunk[h:]]
                continue
            out += got
    return np.asarray(out, np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--llama-embedding", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--texts", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    with open(a.texts, encoding="utf-8") as f:
        texts = list(dict.fromkeys(t.strip() for t in f if t.strip()))
    emb = embed(a.llama_embedding, a.model, texts)
    np.savez(a.out, text=np.array(texts, dtype=np.str_), emb=emb)
    print(f"wrote {a.out}: {emb.shape}")


if __name__ == "__main__":
    main()
