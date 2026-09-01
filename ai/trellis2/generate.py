#!/usr/bin/env python3
"""QtMeshEditor TRELLIS.2 inference sidecar.

Runs Microsoft TRELLIS.2 (MIT, pinned revision - see runtime.json written by
install.py) image-to-3D inference and exports the RAW generation - vertices,
faces and the sparse PBR attribute volume (base color / metallic / roughness /
alpha) - to a QTM3D interchange file. Everything downstream (mesh cleanup,
simplification, UV unwrapping, texture/PBR baking, preview rendering, GLB/FBX
export) is done by QtMeshEditor's own C++ code.

LICENSE BOUNDARY (do not weaken - see docs/trellis2-dependencies.md):
  * nvdiffrast and nvdiffrec are PROHIBITED. This process must run in an
    environment where importing the prohibited nvdiffrast fails because it is not
    installed. They are never imported here, and o_voxel/__init__.py is
    patched by install.py so the o-voxel package no longer imports them
    either. A startup check warns if either is unexpectedly present
    (--strict turns the warning into a hard error).
  * briaai/RMBG-2.0 (the upstream default background remover named in
    TRELLIS.2-4B/pipeline.json) is CC BY-NC and is NEVER downloaded or
    loaded: the rembg loader is stubbed out before pipeline construction,
    and the input image must already carry an alpha matte (QtMeshEditor
    produces it with its own Apache-2.0 U^2-Net remover).

Progress protocol: one JSON object per line on stdout -
  {"event":"stage","stage":"<name>"}                stage transitions
  {"event":"progress","stage":s,"done":d,"total":t} coarse progress
  {"event":"deps", ...}                             dependency report
  {"event":"done", ...stats}                        success (last line)
  {"event":"error","message":m}                     failure (last line)
Human/debug output (tqdm etc.) goes to stderr only.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import platform
import signal
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import qtm3d  # noqa: E402  (local interchange writer)

PROHIBITED_MODULES = ("nvdiffrast", "nvdiffrec", "nvdiffrec_render")

PRESET_PIPELINE_TYPE = {
    "fast": "512",
    "balanced": "1024_cascade",
    "high": "1536_cascade",
}


def emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def stage(name: str) -> None:
    emit({"event": "stage", "stage": name})


def log(msg: str) -> None:
    sys.stderr.write(f"[trellis2] {msg}\n")
    sys.stderr.flush()


def fail(message: str, code: int = 1) -> "NoReturn":  # noqa: F821
    emit({"event": "error", "message": message})
    sys.exit(code)


def check_prohibited(strict: bool) -> list[str]:
    """Detect prohibited NVIDIA research-only packages in the environment.

    They must not be installed at all; this integration never imports them.
    Presence is a packaging mistake worth surfacing (Phase 12).
    """
    present = [m for m in PROHIBITED_MODULES if importlib.util.find_spec(m) is not None]
    for m in present:
        log(f"WARNING: prohibited module '{m}' is installed in this environment. "
            "QtMeshEditor's TRELLIS.2 integration does not use it, but its license "
            "(NVIDIA Source Code License, research/evaluation only) makes it unsafe "
            "for commercial environments - uninstall it.")
    if present and strict:
        fail("prohibited modules present in --strict mode: " + ", ".join(present))
    return present


def runtime_info() -> dict:
    info_path = os.path.join(HERE, "runtime.json")
    # install.py writes runtime.json next to the installed copy of this script;
    # when running from the source tree, look in the env dir instead.
    if not os.path.exists(info_path):
        env_dir = os.environ.get("QTMESH_TRELLIS2_ENV", "")
        if env_dir:
            info_path = os.path.join(env_dir, "runtime.json")
    if os.path.exists(info_path):
        try:
            with open(info_path) as f:
                return json.load(f)
        except Exception:
            pass
    return {}


def dependency_report(present_prohibited: list[str]) -> dict:
    """Phase 15: report which backend dependencies are actually loaded."""
    rep = {
        "event": "deps",
        "python": platform.python_version(),
        "platform": platform.platform(),
    }
    rt = runtime_info()
    rep["trellis2Revision"] = rt.get("trellis2Revision", "unknown")
    rep["cumeshRevision"] = rt.get("cumeshRevision", "unknown")
    rep["flexgemmRevision"] = rt.get("flexgemmRevision", "unknown")
    for mod, key in (("torch", "torch"), ("transformers", "transformers"),
                     ("flash_attn", "flashAttn"), ("o_voxel", "oVoxel"),
                     ("cumesh", "cumesh"), ("flex_gemm", "flexGemm")):
        try:
            spec = importlib.util.find_spec(mod)
        except (ModuleNotFoundError, ValueError):
            spec = None
        rep[key] = "installed" if spec is not None else "NOT INSTALLED"
    try:
        import torch  # noqa: WPS433
        rep["torch"] = torch.__version__
        rep["cuda"] = torch.version.cuda or "none"
        rep["cudaAvailable"] = bool(torch.cuda.is_available())
        if torch.cuda.is_available():
            rep["gpu"] = torch.cuda.get_device_name(0)
            rep["vramGiB"] = round(
                torch.cuda.get_device_properties(0).total_memory / (1024 ** 3), 1)
    except Exception as exc:  # torch missing/broken
        rep["cudaAvailable"] = False
        rep["torchError"] = str(exc)
    for m in PROHIBITED_MODULES:
        rep[m] = "PRESENT (prohibited!)" if m in present_prohibited \
            else "NOT INSTALLED / NOT USED"
    rep["qtmeshTextureBaker"] = "enabled (C++ Trellis2Bake)"
    rep["qtmeshRasterizer"] = "enabled (C++ Trellis2Bake)"
    return rep


def load_rgba(path: str):
    from PIL import Image
    import numpy as np

    img = Image.open(path)
    img.load()
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    alpha = np.asarray(img)[:, :, 3]
    has_matte = bool((alpha < 255).any())
    return img, has_matte


def make_mock_result(seed: int):
    """Synthetic generation for plumbing tests: a UV sphere with procedural
    PBR attributes and a matching sparse attribute volume. No torch, no GPU,
    no TRELLIS.2 needed - exercises the exact interchange/bake/export path."""
    import numpy as np

    rng = np.random.default_rng(seed)
    rings, segs = 48, 64
    verts = []
    for r in range(rings + 1):
        theta = np.pi * r / rings
        for s in range(segs):
            phi = 2 * np.pi * s / segs
            verts.append((
                0.4 * np.sin(theta) * np.cos(phi),
                0.4 * np.cos(theta),
                0.4 * np.sin(theta) * np.sin(phi),
            ))
    verts = np.asarray(verts, dtype=np.float32)
    faces = []
    for r in range(rings):
        for s in range(segs):
            a = r * segs + s
            b = r * segs + (s + 1) % segs
            c = (r + 1) * segs + s
            d = (r + 1) * segs + (s + 1) % segs
            if r != 0:
                faces.append((a, b, c))
            if r != rings - 1:
                faces.append((b, d, c))
    faces = np.asarray(faces, dtype=np.uint32)

    resolution = 64
    voxel_size = 1.0 / resolution
    origin = np.array([-0.5, -0.5, -0.5], dtype=np.float32)
    # occupied voxels = shell around the sphere surface
    ijk = np.unique(((verts - origin) / voxel_size).astype(np.int64), axis=0)
    neigh = np.array([(dx, dy, dz) for dx in (-1, 0, 1) for dy in (-1, 0, 1)
                      for dz in (-1, 0, 1)], dtype=np.int64)
    ijk = np.unique((ijk[:, None, :] + neigh[None, :, :]).reshape(-1, 3), axis=0)
    ijk = ijk.clip(0, resolution - 1)
    ijk = np.unique(ijk, axis=0)
    centers = origin + (ijk.astype(np.float32) + 0.5) * voxel_size
    # procedural attrs: hue bands by height, metallic top half, rough bottom
    h = (centers[:, 1] + 0.5)
    attrs = np.zeros((ijk.shape[0], 6), dtype=np.uint8)
    attrs[:, 0] = (255 * np.clip(np.abs(np.sin(6.0 * h)), 0, 1)).astype(np.uint8)
    attrs[:, 1] = (255 * h).clip(0, 255).astype(np.uint8)
    attrs[:, 2] = (255 * (1.0 - h)).clip(0, 255).astype(np.uint8)
    attrs[:, 3] = np.where(h > 0.5, 230, 10).astype(np.uint8)   # metallic
    attrs[:, 4] = (255 * (0.2 + 0.6 * (1.0 - h))).astype(np.uint8)  # roughness
    attrs[:, 5] = 255                                           # alpha
    del rng

    vcol = np.zeros((verts.shape[0], 4), dtype=np.uint8)
    vh = (verts[:, 1] + 0.5)
    vcol[:, 0] = (255 * np.clip(np.abs(np.sin(6.0 * vh)), 0, 1)).astype(np.uint8)
    vcol[:, 1] = (255 * vh).clip(0, 255).astype(np.uint8)
    vcol[:, 2] = (255 * (1.0 - vh)).clip(0, 255).astype(np.uint8)
    vcol[:, 3] = 255
    return verts, faces, ijk.astype(np.uint16), attrs, vcol, resolution, voxel_size, origin


def run_trellis2(args):
    """The real inference path. Imports of torch/trellis2 happen here so
    --mock/--check work in torch-free environments."""
    stage("load_model")
    os.environ.setdefault("PYTORCH_CUDA_ALLOC_CONF", "expandable_segments:True")
    import numpy as np
    import torch

    if not torch.cuda.is_available():
        fail("CUDA GPU not available. TRELLIS.2 requires an NVIDIA GPU with "
             ">=24 GB VRAM (upstream requirement). QtMeshEditor's TripoSR/"
             "TripoSG backends remain available without one.")

    # ---- RMBG-2.0 (CC BY-NC) bypass: stub the rembg loader BEFORE pipeline
    # construction so the non-commercial model is never downloaded/loaded.
    import trellis2.pipelines.rembg as _rembg

    class _NoRembg:  # noqa: D401
        def __init__(self, *a, **k):
            pass

        def to(self, *a, **k):
            pass

        def cuda(self):
            pass

        def cpu(self):
            pass

        def __call__(self, image):
            raise RuntimeError(
                "background removal is handled by QtMeshEditor (U^2-Net); the "
                "input image must already carry an alpha matte. The upstream "
                "default (briaai/RMBG-2.0) is CC BY-NC and is not used.")

    _rembg.BiRefNet = _NoRembg

    from trellis2.pipelines import Trellis2ImageTo3DPipeline

    model = args.model or "microsoft/TRELLIS.2-4B"
    pipeline = Trellis2ImageTo3DPipeline.from_pretrained(model)
    pipeline.cuda()  # low_vram mode staggers the individual models

    stage("preprocess")
    image, has_matte = load_rgba(args.input)
    if not has_matte and not args.allow_opaque:
        fail("input image has no alpha matte; QtMeshEditor should remove the "
             "background first (or pass --allow-opaque to proceed - the whole "
             "frame will be treated as foreground).")

    pipeline_type = args.pipeline_type or PRESET_PIPELINE_TYPE[args.preset]
    sampler_params = {}
    if args.steps:
        sampler_params = {"steps": int(args.steps)}

    stage("generate")
    t0 = time.time()
    meshes = pipeline.run(
        image,
        seed=args.seed,
        preprocess_image=True,   # model-free on RGBA-with-alpha input
        pipeline_type=pipeline_type,
        max_num_tokens=args.max_num_tokens,
        sparse_structure_sampler_params=sampler_params,
        shape_slat_sampler_params=sampler_params,
        tex_slat_sampler_params=sampler_params,
    )
    gen_seconds = time.time() - t0
    mesh = meshes[0]

    stage("extract")
    verts = mesh.vertices.detach().float().cpu().numpy().astype(np.float32)
    faces = mesh.faces.detach().cpu().numpy().astype(np.uint32)
    coords = mesh.coords.detach().cpu().numpy()
    attrs01 = (mesh.attrs.detach().float().cpu().numpy()).clip(0.0, 1.0)
    attrs = (attrs01 * 255.0 + 0.5).astype(np.uint8)
    resolution = int(round(1.0 / mesh.voxel_size))
    origin = mesh.origin.detach().float().cpu().numpy().astype(np.float32)

    stage("attributes")
    try:
        vattr01 = mesh.query_vertex_attrs().detach().float().cpu().numpy().clip(0, 1)
        vcol = np.empty((verts.shape[0], 4), dtype=np.uint8)
        vcol[:, 0:3] = (vattr01[:, 0:3] * 255.0 + 0.5).astype(np.uint8)
        vcol[:, 3] = (vattr01[:, 5] * 255.0 + 0.5).astype(np.uint8)
    except Exception as exc:  # non-fatal: baking samples the volume anyway
        log(f"per-vertex attribute query failed (non-fatal): {exc}")
        vcol = None

    coord_dtype = np.uint16 if coords.max(initial=0) < 65536 else np.uint32
    return (verts, faces, coords.astype(coord_dtype), attrs, vcol,
            resolution, float(mesh.voxel_size), origin, pipeline_type,
            gen_seconds)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--input", help="input image (RGBA with alpha matte)")
    ap.add_argument("--output", help="output .qtm3d interchange path")
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--preset", choices=sorted(PRESET_PIPELINE_TYPE),
                    default="balanced")
    ap.add_argument("--pipeline-type",
                    choices=["512", "1024", "1024_cascade", "1536_cascade"],
                    help="override the preset's TRELLIS.2 pipeline type")
    ap.add_argument("--steps", type=int, default=0,
                    help="sampler steps override for all three stages "
                         "(0 = upstream defaults)")
    ap.add_argument("--max-num-tokens", type=int, default=49152)
    ap.add_argument("--model", default="",
                    help="HF repo id or local path (default microsoft/TRELLIS.2-4B)")
    ap.add_argument("--allow-opaque", action="store_true",
                    help="proceed even when the input has no alpha matte")
    ap.add_argument("--mock", action="store_true",
                    help="write a synthetic result without loading TRELLIS.2 "
                         "(plumbing/e2e tests; no GPU or torch needed)")
    ap.add_argument("--check", action="store_true",
                    help="verify the environment and exit (no generation)")
    ap.add_argument("--report-deps", action="store_true",
                    help="print the runtime dependency report (Phase 15)")
    ap.add_argument("--strict", action="store_true",
                    help="fail (instead of warn) if prohibited modules are "
                         "installed in the environment")
    args = ap.parse_args()

    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))

    present = check_prohibited(args.strict)
    if args.report_deps or args.check:
        emit(dependency_report(present))

    if args.check:
        problems = []
        for mod in ("torch", "trellis2", "o_voxel", "cumesh", "flex_gemm"):
            if importlib.util.find_spec(mod) is None:
                problems.append(f"missing module: {mod}")
        # o_voxel must import WITHOUT nvdiffrast (install.py patches it)
        if not problems:
            try:
                import o_voxel  # noqa: F401
            except Exception as exc:
                problems.append(f"import o_voxel failed: {exc}")
        if problems:
            fail("environment check failed: " + "; ".join(problems))
        emit({"event": "done", "check": "ok"})
        return

    if not args.input or not args.output:
        fail("--input and --output are required", 2)
    if not os.path.exists(args.input):
        fail(f"input image not found: {args.input}", 2)

    try:
        if args.mock:
            stage("generate")
            (verts, faces, coords, attrs, vcol,
             resolution, voxel_size, origin) = make_mock_result(args.seed)
            pipeline_type, gen_seconds = "mock", 0.0
        else:
            (verts, faces, coords, attrs, vcol, resolution, voxel_size,
             origin, pipeline_type, gen_seconds) = run_trellis2(args)

        stage("write")
        arrays = {
            "positions": verts,
            "indices": faces,
            "voxel_coords": coords,
            "voxel_attrs": attrs,
        }
        if vcol is not None:
            arrays["vertex_colors"] = vcol
        rt = runtime_info()
        qtm3d.write(args.output, arrays, meta={
            "seed": args.seed,
            "preset": args.preset,
            "pipelineType": pipeline_type,
            "resolution": resolution,
            "voxelSize": voxel_size,
            "origin": [float(x) for x in origin],
            "sourceImage": os.path.basename(args.input),
            "generationSeconds": round(gen_seconds, 2),
            "trellis2Revision": rt.get("trellis2Revision", "unknown"),
            "mock": bool(args.mock),
        })
        emit({
            "event": "done",
            "output": args.output,
            "vertexCount": int(verts.shape[0]),
            "triangleCount": int(faces.shape[0]),
            "voxelCount": int(coords.shape[0]),
            "resolution": resolution,
            "generationSeconds": round(gen_seconds, 2),
        })
    except SystemExit:
        raise
    except KeyboardInterrupt:
        fail("cancelled", 130)
    except Exception as exc:  # surface a single structured error line
        import traceback
        traceback.print_exc(file=sys.stderr)
        fail(f"{type(exc).__name__}: {exc}")


if __name__ == "__main__":
    main()
