#!/usr/bin/env python3
"""QtMeshEditor Kimodo sidecar: text -> SOMA-77 motion, inference only.

Runs NVIDIA Kimodo (Apache-2.0 code, NVIDIA Open Model License weights) and writes
QtMeshEditor's motion interchange NPZ; everything after that (bone mapping, retargeting
onto the user's rig, keyframes, export) is QtMeshEditor's own C++
(src/MotionGen/KimodoMotion.*).

Protocol: one JSON object per stdout line (stderr is free-form log text):
  {"event":"stage","stage":"<name>"}                     stage transitions
  {"event":"progress","stage":s,"done":d,"total":t}      coarse progress
  {"event":"error","message":m}                          fatal; exit code != 0
  {"event":"done", ...}                                   success + result summary

Interchange NPZ (one file per variant, float32 arrays + byte-string names, version 1):
  local_rot_mats  [T,J,3,3]  parent-relative rotations, identity = rest pose below
  root_positions  [T,3]      root (Hips) world position, metres
  foot_contacts   [T,C]      0..1 (C = 4 or 6, see Kimodo's output_to_SOMASkeleton77)
  rest_joints     [J,3]      rest-pose GLOBAL joint positions, metres (y up, +z forward)
  joint_parents   [J]        parent index, -1 for the root (stored as float32)
  joint_names     [J]        |S byte strings
  fps             [1]
  format_version  [1]        1
"""

import argparse
import json
import os
import sys
import time
import traceback

FORMAT_VERSION = 1
DEFAULT_MODEL = "Kimodo-SOMA-RP-v1.2"
HERE = os.path.dirname(os.path.abspath(__file__))
TEXT_ENCODER_KIND = ""   # bf16 | int8 | int4 once the local encoder ran (reported in the manifest)


def emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def stage(name: str) -> None:
    emit({"event": "stage", "stage": name})


def log(msg: str) -> None:
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()


def fail(message: str, code: int = 1):
    emit({"event": "error", "message": message})
    sys.exit(code)


def parse_segments(args) -> list:
    """[{text, duration}] from --segments (JSON) or --prompt/--duration."""
    if args.segments:
        try:
            segs = json.loads(args.segments)
        except json.JSONDecodeError as e:
            fail(f"--segments is not valid JSON: {e}", 2)
        if not isinstance(segs, list) or not segs:
            fail("--segments must be a non-empty JSON list", 2)
        out = []
        for s in segs:
            text = str(s.get("text", "")).strip()
            dur = float(s.get("duration", 0))
            if not text or dur <= 0:
                fail("every segment needs a non-empty text and a positive duration", 2)
            out.append({"text": text, "duration": dur})
        return out
    if not args.prompt:
        fail("give --prompt or --segments", 2)
    if args.duration <= 0:
        fail("--duration must be positive", 2)
    return [{"text": args.prompt.strip(), "duration": args.duration}]


def write_interchange(path, local_rot_mats, root_positions, foot_contacts, rest_joints, parents, names, fps):
    import numpy as np

    np.savez(
        path,
        local_rot_mats=np.asarray(local_rot_mats, dtype=np.float32),
        root_positions=np.asarray(root_positions, dtype=np.float32),
        foot_contacts=np.asarray(foot_contacts, dtype=np.float32),
        rest_joints=np.asarray(rest_joints, dtype=np.float32),
        joint_parents=np.asarray(parents, dtype=np.float32),
        joint_names=np.asarray([n.encode("ascii") for n in names]),
        fps=np.asarray([fps], dtype=np.float32),
        format_version=np.asarray([FORMAT_VERSION], dtype=np.float32),
    )


# ---------------------------------------------------------------------------
# Mock: numpy only, no torch, no weights. A hip-swaying walk-in-place on the
# real SOMA-77 rest skeleton so the C++ mapping/retarget path runs in CI.
# ---------------------------------------------------------------------------

def run_mock(args, segments) -> dict:
    import numpy as np

    with open(os.path.join(HERE, "somaskel77_rest.json"), encoding="utf-8") as f:
        rest = json.load(f)
    names, parents = rest["names"], rest["parents"]
    rest_joints = np.asarray(rest["rest_joints"], dtype=np.float32)
    fps = 30.0
    total = sum(s["duration"] for s in segments)
    T = max(2, int(round(total * fps)))
    J = len(names)

    def rot(axis, ang):
        c, s = np.cos(ang), np.sin(ang)
        if axis == "x":
            return np.array([[1, 0, 0], [0, c, -s], [0, s, c]], dtype=np.float32)
        if axis == "y":
            return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]], dtype=np.float32)
        return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]], dtype=np.float32)

    files = []
    for v in range(args.num_samples):
        rng_phase = 0.3 * v
        local = np.tile(np.eye(3, dtype=np.float32), (T, J, 1, 1))
        root = np.tile(rest_joints[0], (T, 1)).astype(np.float32)
        for t in range(T):
            ph = 2 * np.pi * t / fps + rng_phase
            for side, sgn in (("Left", 1.0), ("Right", -1.0)):
                local[t, names.index(side + "Leg")] = rot("x", 0.45 * sgn * np.sin(ph))
                local[t, names.index(side + "Shin")] = rot("x", 0.35 * max(0.0, -sgn * np.sin(ph)))
                local[t, names.index(side + "Arm")] = rot("z", -sgn * 1.2) @ rot("x", -0.3 * sgn * np.sin(ph))
            root[t, 2] = rest_joints[0, 2] + (0.6 * t / fps if args.root_motion else 0.0)
        contacts = np.zeros((T, 4), dtype=np.float32)
        path = os.path.join(args.output_dir, f"motion_{v:02d}.npz")
        write_interchange(path, local, root, contacts, rest_joints, parents, names, fps)
        files.append(path)
        emit({"event": "progress", "stage": "generate", "done": v + 1, "total": args.num_samples})
    return {"files": files, "fps": fps, "frames": T, "joints": J, "model": "mock", "device": "cpu"}


# ---------------------------------------------------------------------------
# Real inference
# ---------------------------------------------------------------------------

def pick_device(requested: str) -> str:
    import torch

    if requested and requested != "auto":
        return requested
    if torch.cuda.is_available():
        return "cuda:0"
    if getattr(torch.backends, "mps", None) and torch.backends.mps.is_available():
        return "mps"
    return "cpu"


class CachedTextEncoder:
    """Stand-in for Kimodo's text encoder holding precomputed prompt embeddings.

    The Llama-3-8B encoder needs ~16 GB; keeping it resident next to the
    diffusion model does not fit a 24 GB unified-memory Mac. The prompts are
    known before generation starts, so they are encoded once, the encoder is
    freed, and the model is given this cache instead (same call contract as
    LLM2VecEncoder / TextEncoderAPI: (padded features, lengths)).
    """

    def __init__(self, table, dim, unconditional=False):
        self.table = table   # text -> float32 cpu tensor [1, dim]
        self.dim = dim
        self.unconditional = unconditional   # test mode: every prompt encodes as empty (zeros)
        self.device = "cpu"
        self.dtype = None

    def to(self, device=None, dtype=None):
        if device is not None:
            self.device = device
        if dtype is not None:
            self.dtype = dtype
        return self

    def eval(self):
        return self

    def __call__(self, texts):
        import torch
        if isinstance(texts, str):
            texts = [texts]
        out = torch.zeros(len(texts), 1, self.dim)
        for i, t in enumerate(texts):
            if t.strip() and not self.unconditional:
                if t not in self.table:
                    raise KeyError(f"prompt was not pre-encoded: {t!r}")
                out[i] = self.table[t]
        out = out.to(device=self.device, dtype=self.dtype or torch.float32)
        return out, [1] * len(texts)


def runtime_root() -> str:
    """Where the runtime keeps derived files: QTMESH_KIMODO_ENV, else the app-data kimodo dir (install.py's default)."""
    if os.environ.get("QTMESH_KIMODO_ENV"):
        return os.environ["QTMESH_KIMODO_ENV"]
    if sys.platform.startswith("linux"):
        base = os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share"))
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("APPDATA", os.path.expanduser("~"))
    return os.path.join(base, "QtMeshEditor", "QtMeshEditor", "kimodo")


def textenc_quant_module():
    import importlib.util
    spec = importlib.util.spec_from_file_location("qtmesh_textenc_quant", os.path.join(HERE, "textenc_quant.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def load_text_encoder(mode: str, device: str):
    """The LLM2Vec encoder: bf16 (mode none) or a local weight-quantized checkpoint (int8/int4; auto = int8,
    built once from the user's own Llama download on first use)."""
    from kimodo.model.load_model import _build_local_text_encoder_conf, instantiate_from_dict

    if mode == "none":
        return instantiate_from_dict(_build_local_text_encoder_conf()), "bf16"
    bits = 4 if mode == "int4" else 8
    tq = textenc_quant_module()
    path = os.environ.get("QTMESH_KIMODO_TEXTENC_DIR") or os.path.join(runtime_root(), f"textenc_int{bits}")
    if not tq.is_valid_dir(path, bits):
        stage("quantize_text_encoder")
        log(f"building the int{bits} text encoder once in {path} (from the local Llama 3 download)")
        tq.build_and_save(path, bits)
        import gc
        gc.collect()
    return tq.QuantizedEncoder(path, os.environ.get("TEXT_ENCODER_DEVICE") or device), f"int{bits}"


def encode_prompts_then_free(texts, device, quant="auto"):
    """Encode `texts` with the local LLM2Vec encoder, then release it."""
    import gc
    import torch
    from kimodo.sanitize import sanitize_texts

    stage("load_text_encoder")
    t0 = time.time()
    global TEXT_ENCODER_KIND
    encoder, kind = load_text_encoder(quant, device)
    TEXT_ENCODER_KIND = kind
    log(f"loaded the {kind} text encoder on {os.environ.get('TEXT_ENCODER_DEVICE', 'auto')} in {time.time() - t0:.1f}s")
    if device == "mps":
        log(f"text encoder GPU memory: {torch.mps.driver_allocated_memory() / 1e9:.1f} GB")
    stage("encode_text")

    table = {}
    # Encode the prompts exactly as the model will pass them (it sanitises first).
    for t in dict.fromkeys(x for x in sanitize_texts(list(texts)) if x.strip()):
        feat, _ = encoder([t])
        table[t] = feat[0].detach().float().cpu()
    dim = int(encoder.llm_dim)
    del encoder
    gc.collect()
    if device == "mps":
        torch.mps.empty_cache()
    elif device.startswith("cuda"):
        torch.cuda.empty_cache()
    return CachedTextEncoder(table, dim)


def run_kimodo(args, segments) -> dict:
    stage("import")
    try:
        import numpy as np
        import torch
        from kimodo import load_model
        from kimodo.constraints import load_constraints_lst
        from kimodo.tools import seed_everything
    except ImportError as e:
        fail(f"Kimodo runtime is not installed ({e}). Run ai/kimodo/install.py.", 3)

    device = pick_device(args.device)
    if args.text_encoder_device:
        os.environ["TEXT_ENCODER_DEVICE"] = args.text_encoder_device
    elif device == "mps" and "TEXT_ENCODER_DEVICE" not in os.environ:
        os.environ["TEXT_ENCODER_DEVICE"] = "mps"
    if args.text_encoder_url:
        # Reuse an already running `kimodo_textencoder` service instead of loading Llama here.
        os.environ["TEXT_ENCODER_URL"] = args.text_encoder_url
        os.environ.setdefault("TEXT_ENCODER_MODE", "api")
    elif "TEXT_ENCODER_MODE" not in os.environ:
        os.environ["TEXT_ENCODER_MODE"] = "local"

    texts = [s["text"] if s["text"].endswith(".") else s["text"] + "." for s in segments]
    try:
        # Local encoder: encode the prompts first and free Llama before the diffusion model loads.
        if os.environ.get("QTMESH_KIMODO_NO_TEXT_ENCODER") == "1":
            # Pipeline test without the 16 GB Llama encoder: the real motion model runs,
            # but every prompt is encoded as empty, so the motion ignores the text.
            log("QTMESH_KIMODO_NO_TEXT_ENCODER=1: prompts are IGNORED (unconditional motion, test mode)")
            cached = CachedTextEncoder({}, 4096, unconditional=True)
        elif args.text_encoder_url or args.keep_text_encoder:
            cached = None
        else:
            cached = encode_prompts_then_free(texts, device, args.text_encoder_quant)
        stage("load_model")
        t0 = time.time()
        # MPS has no float64 and the checkpoint keeps its normalisation stats and
        # neutral skeleton in float64: load on the CPU, cast, then move.
        load_device = "cpu" if device == "mps" else device
        model, resolved = load_model(args.model, device=load_device, default_family="Kimodo",
                                     return_resolved_name=True, text_encoder=cached)
        if load_device != device:
            for t in list(model.parameters()) + list(model.buffers()):
                if t.dtype == torch.float64:
                    t.data = t.data.float()
            model.to(device)
            model.device = device
    except Exception as e:  # gated Llama access, missing weights, OOM, ...
        msg = str(e)
        if "gated" in msg.lower() or "403" in msg or "401" in msg:
            msg = ("The Kimodo text encoder needs Meta Llama 3 (meta-llama/Meta-Llama-3-8B-Instruct, gated). "
                   "Accept Meta's licence on Hugging Face and log in (`hf auth login`). Details: " + msg)
        fail(f"could not load {args.model}: {msg}", 4)
    if "soma" not in resolved.lower():
        fail(f"{resolved} is not a SOMA model; QtMeshEditor maps SOMA skeletons only", 2)
    log(f"loaded {resolved} on {device} in {time.time() - t0:.1f}s")

    fps = float(model.fps)
    num_frames = [max(2, int(s["duration"] * fps)) for s in segments]

    constraints = []
    if args.constraints:
        constraints = load_constraints_lst(args.constraints, model.skeleton)

    if args.seed is not None:
        seed_everything(args.seed)

    stage("generate")
    t0 = time.time()
    # Required by Kimodo; 0 = upstream's default (scripts/generate.py: 100).
    steps = args.diffusion_steps if args.diffusion_steps > 0 else 100
    output = model(texts, num_frames, constraint_lst=constraints, num_samples=args.num_samples,
                   multi_prompt=True, num_transition_frames=args.transition_frames,
                   post_processing=not args.no_postprocess, return_numpy=True, num_denoising_steps=steps)
    log(f"generated {args.num_samples} sample(s) in {time.time() - t0:.1f}s")

    stage("write")
    skel = model.skeleton
    if skel.nbjoints != 77 and hasattr(skel, "somaskel77"):
        skel = skel.somaskel77
    names = list(skel.bone_order_names)
    parents = [int(p) for p in skel.joint_parents.cpu().numpy().tolist()]
    rest_joints = skel.neutral_joints.cpu().numpy()

    local = np.asarray(output["local_rot_mats"])
    if local.ndim == 4:  # unbatched
        local = local[None]
    if local.shape[2] != len(names):
        fail(f"model returned {local.shape[2]} joints, expected {len(names)} ({skel.name})", 5)
    roots = np.asarray(output["root_positions"]).reshape(local.shape[0], local.shape[1], 3)
    contacts = np.asarray(output.get("foot_contacts", np.zeros((local.shape[0], local.shape[1], 4))))
    contacts = contacts.reshape(local.shape[0], local.shape[1], -1)

    files = []
    for v in range(local.shape[0]):
        path = os.path.join(args.output_dir, f"motion_{v:02d}.npz")
        root = roots[v].copy()
        if not args.root_motion:
            root[:, 0] = root[0, 0]
            root[:, 2] = root[0, 2]
        write_interchange(path, local[v], root, contacts[v], rest_joints, parents, names, fps)
        files.append(path)
        emit({"event": "progress", "stage": "write", "done": v + 1, "total": int(local.shape[0])})
    return {"files": files, "fps": fps, "frames": int(local.shape[1]), "joints": len(names),
            "model": resolved, "device": device}


def run_check() -> None:
    info = {"event": "done", "check": "ok", "python": sys.version.split()[0]}
    try:
        import torch
        info["torch"] = torch.__version__
        info["cuda"] = bool(torch.cuda.is_available())
        info["mps"] = bool(getattr(torch.backends, "mps", None) and torch.backends.mps.is_available())
    except ImportError:
        fail("torch is not installed", 3)
    try:
        import kimodo  # noqa: F401
        info["kimodo"] = True
    except ImportError as e:
        fail(f"kimodo is not installed ({e})", 3)
    token_path = os.path.expanduser("~/.cache/huggingface/token")
    info["hf_token"] = bool(os.environ.get("HF_TOKEN") or os.path.exists(token_path))
    emit(info)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--prompt", help="single text prompt")
    ap.add_argument("--duration", type=float, default=5.0, help="seconds (single prompt)")
    ap.add_argument("--segments", help='JSON list [{"text":..,"duration":..}], one per timeline segment')
    ap.add_argument("--output-dir", help="directory for motion_NN.npz + result.json")
    ap.add_argument("--num-samples", type=int, default=1)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--diffusion-steps", type=int, default=0, help="0 = Kimodo's default (100)")
    ap.add_argument("--transition-frames", type=int, default=5)
    ap.add_argument("--constraints", help="Kimodo constraints JSON")
    ap.add_argument("--no-postprocess", action="store_true", help="skip Kimodo's foot-skate cleanup")
    ap.add_argument("--no-root-motion", dest="root_motion", action="store_false",
                    help="keep the root in place horizontally (height is kept)")
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--device", default="auto", help="auto|cuda:0|mps|cpu")
    ap.add_argument("--text-encoder-device", default="", help="device for the Llama text encoder")
    ap.add_argument("--text-encoder-url", default="", help="reuse a running kimodo_textencoder service")
    ap.add_argument("--text-encoder-quant", choices=("auto", "none", "int8", "int4"),
                    default=os.environ.get("QTMESH_KIMODO_TEXTENC_QUANT", "auto"),
                    help="Llama text encoder weights: auto (= int8, built once locally; ~8.7 GB instead of ~15 GB, "
                         "embedding cosine 0.9998 vs bf16), none (bf16), int8, int4 (~5.6 GB, cosine 0.97)")
    ap.add_argument("--keep-text-encoder", action="store_true",
                    help="keep Llama loaded next to the model (default: encode the prompts, then free it)")
    ap.add_argument("--mock", action="store_true", help="synthetic motion, numpy only (tests)")
    ap.add_argument("--check", action="store_true", help="report the runtime and exit")
    args = ap.parse_args()

    if args.check:
        run_check()
        return
    if not args.output_dir:
        fail("--output-dir is required", 2)
    if not 1 <= args.num_samples <= 16:
        fail("--num-samples must be 1..16", 2)
    os.makedirs(args.output_dir, exist_ok=True)
    segments = parse_segments(args)

    try:
        result = run_mock(args, segments) if args.mock else run_kimodo(args, segments)
    except SystemExit:
        raise
    except Exception as e:
        log(traceback.format_exc())
        fail(f"{type(e).__name__}: {e}")

    manifest = {
        "format": "qtmesh-motion-interchange", "format_version": FORMAT_VERSION,
        "source": "kimodo", "segments": segments,
        "text_conditioning": "none" if os.environ.get("QTMESH_KIMODO_NO_TEXT_ENCODER") == "1" and not args.mock else "prompt", "seed": args.seed,
        "text_encoder": TEXT_ENCODER_KIND or None,
        "num_samples": args.num_samples, "root_motion": args.root_motion,
        "postprocess": not args.no_postprocess, **result,
    }
    with open(os.path.join(args.output_dir, "result.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    emit({"event": "done", **manifest})


if __name__ == "__main__":
    main()
