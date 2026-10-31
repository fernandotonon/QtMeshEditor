#!/usr/bin/env python3
# ruff: noqa: E702, E741
"""Build the TEXT-conditioned text-to-motion training cache (v9; dev tool, not shipped).

The v8 model conditions on one of ~30 action labels. v9 conditions on a sentence embedding of the prompt
(Qwen3-Embedding-0.6B, scripts/embed-t2m-text.py), so directions, sides, styles and two-step prompts
reach the model. Sources:

  --kimodo DIR   Kimodo corpus folders (scripts/build-kimodo-t2m-corpus.py + kimodo-corpus-to-canonical.py):
                 canonical/index.jsonl + batch_*.json; only clips that passed the corpus gates
                 (quality.jsonl ok) are used. Whole clips (4-5 s), each also MIRRORED with
                 left <-> right swapped in its text.
  --v8 NPZ       the v8 action-label cache (prep-t2m-v6.py output; already canonical + mirrored): its
                 2 s windows get a caption per action (ACTION_CAPTIONS).

Clips have different lengths, so every sample is padded to --tmax frames (last frame repeated) and
carries its true length `ln`; the trainer masks the padding. Each text gets PARAPHRASES (same motion,
different wording) so the model learns the meaning, not one phrasing; all are embedded once.

Output npz: mo[N,Tmax,22,4] canonical quats, ln[N], msk[N,22], src[N] (0 kimodo, 1 v8),
tv[N,P] text-variant ids, texts[K], emb[K,1024], fps, canonRestDir, buildArgv.
"""

import argparse
import importlib.util
import json
import os
import re
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
J = 22
P = 4   # text variants per sample


def load_module(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


prep6 = load_module("prep6", "prep-t2m-v6.py")
prep5 = prep6.prep5
embedder = load_module("embedt2m", "embed-t2m-text.py")

# Captions for the v8 action labels (game-asset clip names included).
ACTION_CAPTIONS = {
    "attack": ["a person attacks", "a person swings a weapon to attack"],
    "boxing": ["a person boxes", "a person throws boxing punches"],
    "buildloop": ["a person hammers while building something"],
    "cartgiveloop": ["a person pushes a cart and hands something over"],
    "climb": ["a person climbs", "a person climbs up a wall"],
    "cough": ["a person coughs"],
    "crouch": ["a person crouches down", "a person crouches"],
    "cut": ["a person cuts something with a blade"],
    "dance": ["a person dances"],
    "death": ["a person dies and falls down", "a person collapses and dies"],
    "fall": ["a person falls down", "a person falls over"],
    "farmloop": ["a person works the ground with a hoe"],
    "hit": ["a person gets hit", "a person reacts to being hit"],
    "idle": ["a person stands idle", "a person stands still"],
    "jump": ["a person jumps", "a person jumps up"],
    "kick": ["a person kicks", "a person does a kick"],
    "pickup": ["a person picks something up from the ground"],
    "punch": ["a person punches", "a person throws a punch"],
    "run": ["a person runs forward", "a person runs"],
    "salute": ["a person salutes"],
    "shake": ["a person shakes hands", "a person shakes"],
    "shoot": ["a person aims and shoots a gun", "a person shoots"],
    "sit": ["a person sits", "a person sits down"],
    "stonegiveloop": ["a person carries a stone and hands it over"],
    "strafeleft": ["a person strafes to the left", "a person side steps to the left"],
    "straferight": ["a person strafes to the right", "a person side steps to the right"],
    "walk": ["a person walks forward", "a person walks"],
    "wave": ["a person waves", "a person waves hello"],
    "woodgiveloop": ["a person carries wood and hands it over"],
    "working": ["a person works with the hands", "a person is working"],
    "march": ["a person marches forward"], "swim": ["a person swims"], "crawl": ["a person crawls"],
    "shakehand": ["a person shakes hands"], "throw": ["a person throws something"],
}


def swap_sides(text):
    return re.sub(r"\b(left|right)\b", lambda m: "right" if m.group(1) == "left" else "left", text)


def paraphrases(text):
    """Same meaning, different wording: the prefix and person word vary; the action words are kept."""
    body = re.sub(r"^(a person|someone|the person)\s+", "", text.strip())
    out = [text, f"someone {body}", f"the character {body}", body]
    seen = list(dict.fromkeys(out))
    while len(seen) < P:
        seen.append(seen[len(seen) % len(seen)])
    return seen[:P]


def kimodo_clips(root):
    ok = set()
    qpath = os.path.join(root, "quality.jsonl")
    if os.path.exists(qpath):
        for line in open(qpath, encoding="utf-8"):
            d = json.loads(line)
            if d.get("ok"):
                ok.add(d["id"])
    cdir = os.path.join(root, "canonical")
    for b in sorted(f for f in os.listdir(cdir) if f.startswith("batch_") and f.endswith(".json")):
        dump = json.load(open(os.path.join(cdir, b), encoding="utf-8"))
        if int(dump.get("fps", 30)) != 30:
            sys.exit(f"{b}: expected 30 fps")
        for c in dump["clips"]:
            if ok and c.get("id") not in ok:
                continue
            yield c


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kimodo", action="append", default=[])
    ap.add_argument("--v8", default="")
    ap.add_argument("--v8-max", type=int, default=0,
                    help="cap the v8 windows (the v8 cache repeats its curated takes 40x); 0 = all")
    ap.add_argument("--tmax", type=int, default=150)
    ap.add_argument("--min-roles", type=int, default=16)
    ap.add_argument("--llama-embedding", required=True)
    ap.add_argument("--encoder", required=True, help="Qwen3-Embedding-0.6B GGUF")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    Tm = a.tmax

    mo, ln, msk, src, texts = [], [], [], [], []

    def add(q, valid, text, s):
        n = min(q.shape[0], Tm)
        w = np.empty((Tm, J, 4), np.float32)
        w[:n] = q[:n]
        w[n:] = q[n - 1]
        mo.append(w); ln.append(n); msk.append(valid); src.append(s); texts.append(text)

    nk = dropped = 0
    for root in a.kimodo:
        for c in kimodo_clips(os.path.expanduser(root)):
            cq, valid = prep5.canonicalize(np.asarray(c["quats"], np.float32), c["restWorld"], c["restDir"])
            if int(valid.sum()) < a.min_roles:
                dropped += 1
                continue
            add(cq, valid, c["text"], 0)
            mq, mv = prep6.mirror(cq, valid)
            add(mq, mv, swap_sides(c["text"]), 0)
            nk += 1
    print(f"kimodo: {nk} clips (+mirrors) -> {len(mo)} samples; {dropped} below --min-roles")

    if a.v8:
        d = np.load(os.path.expanduser(a.v8), allow_pickle=False)
        vocab = [str(v) for v in d["vocab"]]
        idx = np.arange(d["mo"].shape[0])
        if a.v8_max and len(idx) > a.v8_max:
            idx = np.random.default_rng(0).choice(idx, a.v8_max, replace=False)
        missing = sorted({vocab[i] for i in range(len(vocab))} - set(ACTION_CAPTIONS))
        if missing:
            sys.exit(f"no caption for v8 actions: {missing}")
        rng = np.random.default_rng(1)
        for i in idx:
            act = vocab[int(d["tk"][i].argmax())]
            caps = ACTION_CAPTIONS[act]
            add(d["mo"][i], d["msk"][i], caps[int(rng.integers(len(caps)))], 1)
        print(f"v8: {len(idx)} windows -> {len(mo)} samples (cum)")

    # Every distinct text x paraphrase, embedded once.
    variants = [paraphrases(t) for t in texts]
    uniq = list(dict.fromkeys(v for vs in variants for v in vs))
    tid = {t: i for i, t in enumerate(uniq)}
    tv = np.array([[tid[v] for v in vs] for vs in variants], np.int32)
    with tempfile.TemporaryDirectory() as tmp:
        tp = os.path.join(tmp, "texts.txt")
        with open(tp, "w", encoding="utf-8") as f:
            f.write("\n".join(uniq) + "\n")
        emb = embedder.embed(a.llama_embedding, a.encoder, uniq)
    print(f"embedded {len(uniq)} texts -> {emb.shape}")

    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    np.savez_compressed(a.out, mo=np.stack(mo), ln=np.array(ln, np.int32), msk=np.stack(msk).astype(np.float32),
                        src=np.array(src, np.int8), tv=tv, texts=np.array(uniq, dtype=np.str_), emb=emb,
                        fps=30, canonRestDir=prep5.D_CANON, buildArgv=np.array(sys.argv, dtype=np.str_))
    print(f"wrote {a.out}: {len(mo)} samples, Tmax {Tm}, {len(uniq)} texts")


if __name__ == "__main__":
    main()
