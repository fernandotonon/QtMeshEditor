#!/usr/bin/env python3
"""Generate a text-to-motion training corpus with NVIDIA Kimodo (dev tool, not shipped).

Runs inside the Kimodo sidecar environment (ai/kimodo/install.py) and reuses ai/kimodo/generate.py:
every prompt is encoded ONCE with the local text encoder (int8 by default), the encoder is freed, the
diffusion model is loaded ONCE, then each prompt is generated with several seeds. Each clip is written
as QtMeshEditor's motion interchange NPZ (SOMA-77) plus one line in corpus.jsonl.

Licensing (see THIRD_PARTY_AI_MODELS.md): Kimodo's outputs are not Derivative Models under the NVIDIA
Open Model License (section 1.1) and NVIDIA claims no ownership of them (section 2.4), so they can train
our own model. The Llama 3 text embeddings are NOT written anywhere: our model must be trained on the
prompt text with its own text encoder, never on Llama outputs.

    ~/kimodo_eval/venv/bin/python scripts/build-kimodo-t2m-corpus.py --prompts prompts.txt \
        --out ~/t2m_kimodo/v1 --seeds 2 --duration 4

Resumable: clips already in corpus.jsonl are skipped.
"""

import argparse
import importlib.util
import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def load_generate():
    path = os.path.join(HERE, "..", "ai", "kimodo", "generate.py")
    spec = importlib.util.spec_from_file_location("qtmesh_kimodo_generate", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_prompts(path):
    """One prompt per line ('#' comments; optional '<prompt> @<seconds>'), or a JSON list of strings/objects."""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    items = []
    if text.lstrip().startswith("["):
        for e in json.loads(text):
            items.append(e if isinstance(e, dict) else {"text": e})
    else:
        for line in text.splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if "@" in line:
                t, _, d = line.rpartition("@")
                try:
                    items.append({"text": t.strip(), "duration": float(d)})
                    continue
                except ValueError:
                    pass
            items.append({"text": line})
    seen, out = set(), []
    for e in items:
        key = e["text"].strip().lower()
        if key and key not in seen:
            seen.add(key)
            out.append(e)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--prompts", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--seeds", type=int, default=2, help="samples per prompt")
    ap.add_argument("--duration", type=float, default=4.0, help="seconds, unless the prompt sets its own")
    ap.add_argument("--diffusion-steps", type=int, default=100)
    ap.add_argument("--text-encoder-quant", default="auto", choices=("auto", "none", "int8", "int4"))
    ap.add_argument("--limit", type=int, default=0, help="only the first N prompts (0 = all)")
    ap.add_argument("--model", default="Kimodo-SOMA-RP-v1.2")
    args = ap.parse_args()

    g = load_generate()
    import numpy as np
    import torch
    from kimodo import load_model
    from kimodo.tools import seed_everything

    prompts = read_prompts(args.prompts)
    if args.limit:
        prompts = prompts[: args.limit]
    os.makedirs(os.path.join(args.out, "clips"), exist_ok=True)
    index_path = os.path.join(args.out, "corpus.jsonl")
    done = set()
    if os.path.exists(index_path):
        with open(index_path, encoding="utf-8") as f:
            for line in f:
                try:
                    done.add(json.loads(line)["id"])
                except (ValueError, KeyError):
                    pass
    todo = [(i, p) for i, p in enumerate(prompts) if any(f"p{i:05d}_s{s}" not in done for s in range(args.seeds))]
    g.log(f"{len(prompts)} prompts, {len(todo)} to generate, {len(done)} clips already done")
    if not todo:
        return 0

    device = g.pick_device("auto")
    if device == "mps":
        os.environ.setdefault("TEXT_ENCODER_DEVICE", "mps")
    os.environ.setdefault("TEXT_ENCODER_MODE", "local")
    texts = [p["text"] if p["text"].endswith(".") else p["text"] + "." for _, p in todo]
    t0 = time.time()
    cached = g.encode_prompts_then_free(texts, device, args.text_encoder_quant)
    g.log(f"encoded {len(texts)} prompts in {time.time() - t0:.0f}s with the {g.TEXT_ENCODER_KIND} encoder")

    load_device = "cpu" if device == "mps" else device
    model, resolved = load_model(args.model, device=load_device, default_family="Kimodo",
                                 return_resolved_name=True, text_encoder=cached)
    if "soma" not in resolved.lower():
        g.log(f"{resolved} is not a SOMA model")
        return 2
    if load_device != device:
        for t in list(model.parameters()) + list(model.buffers()):
            if t.dtype == torch.float64:
                t.data = t.data.float()
        model.to(device)
        model.device = device
    skel = model.skeleton
    if skel.nbjoints != 77 and hasattr(skel, "somaskel77"):
        skel = skel.somaskel77
    names = list(skel.bone_order_names)
    parents = [int(p) for p in skel.joint_parents.cpu().numpy().tolist()]
    rest = skel.neutral_joints.cpu().numpy()
    fps = float(model.fps)

    with open(index_path, "a", encoding="utf-8") as index:
        for n, ((i, p), text) in enumerate(zip(todo, texts)):
            duration = float(p.get("duration") or args.duration)
            frames = max(2, int(duration * fps))
            seed = 1000 + i
            seed_everything(seed)
            t1 = time.time()
            out = model([text], [frames], constraint_lst=[], num_samples=args.seeds, multi_prompt=True,
                        num_transition_frames=5, post_processing=True, return_numpy=True,
                        num_denoising_steps=args.diffusion_steps)
            local = np.asarray(out["local_rot_mats"])
            if local.ndim == 4:
                local = local[None]
            roots = np.asarray(out["root_positions"]).reshape(local.shape[0], local.shape[1], 3)
            contacts = np.asarray(out.get("foot_contacts", np.zeros((local.shape[0], local.shape[1], 4))))
            contacts = contacts.reshape(local.shape[0], local.shape[1], -1)
            for s in range(local.shape[0]):
                cid = f"p{i:05d}_s{s}"
                if cid in done:
                    continue
                rel = os.path.join("clips", cid + ".npz")
                g.write_interchange(os.path.join(args.out, rel), local[s], roots[s], contacts[s], rest, parents, names, fps)
                index.write(json.dumps({"id": cid, "file": rel, "text": p["text"], "duration": duration,
                                        "frames": int(local.shape[1]), "fps": fps, "seed": seed, "sample": s,
                                        "model": resolved, "text_encoder": g.TEXT_ENCODER_KIND,
                                        "diffusion_steps": args.diffusion_steps}) + "\n")
            index.flush()
            g.log(f"[{n + 1}/{len(todo)}] {time.time() - t1:.1f}s  {p['text']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
