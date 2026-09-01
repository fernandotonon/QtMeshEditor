#!/usr/bin/env python3
"""QtMeshEditor TRELLIS.2 runtime installer.

Creates an ISOLATED Python environment for the TRELLIS.2 backend (Phase 12 -
nothing here touches QtMeshEditor's own dependencies), pinned to the audited
revisions in docs/trellis2-dependencies.md:

    TRELLIS.2  microsoft/TRELLIS.2  75fbf0183001ed9876c8dbb35de6b68552ee08bd (MIT)
    CuMesh     JeffreyXiang/CuMesh   12289e1062f0603f2f0d0771b02e1395d247f26f (MIT)
    FlexGEMM   JeffreyXiang/FlexGEMM 6dd94a859c26ee8246888502eada3dd8ad85532e (MIT)

PROHIBITED (never installed - NVIDIA Source Code License, research-only):
    nvdiffrast and nvdiffrec are prohibited. The upstream setup.sh installs
    them for its own GLB texture baking and preview renderers; QtMeshEditor
    replaces that functionality with its own C++ rasterizer/baker
    (src/ImageTo3D/Trellis2Bake.*). To make the o-voxel package importable
    WITHOUT the prohibited nvdiffrast, this installer patches the MIT file
    o_voxel/__init__.py to import its `postprocess` module lazily (that
    module imports the prohibited nvdiffrast at top level but is never used
    by QtMeshEditor).

Layout under --dest (default: <QtMeshEditor app data>/trellis2):
    env/          the virtualenv
    TRELLIS.2/    the pinned upstream checkout (trellis2 + o-voxel packages)
    CuMesh/       pinned checkout (built + installed into env)
    FlexGEMM/     pinned checkout (built + installed into env)
    runtime.json  marker read by QtMeshEditor to detect the runtime

Requires: Linux, Python >= 3.10, git, an NVIDIA GPU (>= 24 GB VRAM
recommended), CUDA toolkit 12.4 for building the extensions.

Model weights are NOT fetched here; TRELLIS.2-4B (MIT) downloads on first
generation via huggingface_hub. The DINOv3 image encoder
(facebook/dinov3-vitl16-pretrain-lvd1689m) is GATED - accept Meta's DINOv3
License on Hugging Face and `huggingface-cli login` before first use.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import venv

TRELLIS2_REPO = "https://github.com/microsoft/TRELLIS.2.git"
TRELLIS2_REV = "75fbf0183001ed9876c8dbb35de6b68552ee08bd"
CUMESH_REPO = "https://github.com/JeffreyXiang/CuMesh.git"
CUMESH_REV = "12289e1062f0603f2f0d0771b02e1395d247f26f"
FLEXGEMM_REPO = "https://github.com/JeffreyXiang/FlexGEMM.git"
FLEXGEMM_REV = "6dd94a859c26ee8246888502eada3dd8ad85532e"

TORCH_SPEC = ["torch==2.6.0", "torchvision==0.21.0"]
TORCH_INDEX = "https://download.pytorch.org/whl/cu124"
FLASH_ATTN_SPEC = "flash-attn==2.7.3"

# sha256 of the pristine o-voxel/o_voxel/__init__.py at TRELLIS2_REV.
OVOXEL_INIT_SHA256 = "ca30e1545d11f3e862b7d11b31af6200427c91482f11d8b543982d692cd8588b"
OVOXEL_INIT_PATCHED = '''"""o_voxel package init - PATCHED by QtMeshEditor (ai/trellis2/install.py).

Upstream imports `postprocess` eagerly, and o_voxel/postprocess.py imports the
prohibited nvdiffrast at module top. nvdiffrast is under the NVIDIA Source Code
License (research/evaluation only) and is prohibited in QtMeshEditor's
TRELLIS.2 integration, so `postprocess` is made lazy: the core conversion /
IO / rasterize / serialize APIs work without the prohibited nvdiffrast, and
`o_voxel.postprocess` still resolves for users who have it. This patch
modifies MIT-licensed Microsoft code only - no NVIDIA source is involved.
"""
from . import (
    convert,
    io,
    rasterize,
    serialize
)


def __getattr__(name):
    if name == 'postprocess':
        import importlib
        return importlib.import_module('.postprocess', __name__)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
'''


def log(msg: str) -> None:
    print(f"[trellis2-install] {msg}", flush=True)


def run(cmd: list[str], **kw) -> None:
    log("$ " + " ".join(cmd))
    subprocess.run(cmd, check=True, **kw)


def default_dest() -> str:
    if os.environ.get("QTMESH_TRELLIS2_ENV"):
        return os.environ["QTMESH_TRELLIS2_ENV"]
    if sys.platform.startswith("linux"):
        base = os.environ.get("XDG_DATA_HOME",
                              os.path.expanduser("~/.local/share"))
        return os.path.join(base, "QtMeshEditor", "trellis2")
    if sys.platform == "darwin":
        return os.path.expanduser(
            "~/Library/Application Support/QtMeshEditor/trellis2")
    return os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")),
                        "QtMeshEditor", "trellis2")


def clone_pinned(repo: str, rev: str, dest: str) -> None:
    if os.path.isdir(os.path.join(dest, ".git")):
        head = subprocess.run(["git", "-C", dest, "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
        if head == rev:
            log(f"{dest}: already at {rev[:12]}")
            return
        run(["git", "-C", dest, "fetch", "origin", rev])
        run(["git", "-C", dest, "checkout", "--detach", rev])
        return
    os.makedirs(dest, exist_ok=True)
    run(["git", "init", "-q", dest])
    run(["git", "-C", dest, "remote", "add", "origin", repo])
    run(["git", "-C", dest, "fetch", "--depth", "1", "origin", rev])
    run(["git", "-C", dest, "checkout", "--detach", "FETCH_HEAD"])


def patch_ovoxel_init(trellis_dir: str) -> None:
    path = os.path.join(trellis_dir, "o-voxel", "o_voxel", "__init__.py")
    with open(path, "rb") as f:
        data = f.read()
    digest = hashlib.sha256(data).hexdigest()
    if b"PATCHED by QtMeshEditor" in data:
        log("o_voxel/__init__.py already patched")
        return
    if digest != OVOXEL_INIT_SHA256:
        raise SystemExit(
            f"o_voxel/__init__.py has unexpected content (sha256 {digest}); "
            "the pinned revision must have changed - re-audit before patching "
            "(docs/trellis2-dependencies.md).")
    with open(path, "w", encoding="utf-8") as f:
        f.write(OVOXEL_INIT_PATCHED)
    log("patched o_voxel/__init__.py (lazy postprocess - "
        "the prohibited nvdiffrast is no longer imported)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dest", default=default_dest(),
                    help="install root (default: %(default)s)")
    ap.add_argument("--attn", choices=["flash-attn", "xformers"],
                    default="flash-attn",
                    help="attention backend to install")
    ap.add_argument("--skip-torch", action="store_true",
                    help="assume torch/torchvision already in the env")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 1),
                    help="parallel build jobs for the CUDA extensions")
    args = ap.parse_args()

    if not sys.platform.startswith("linux"):
        # Hard stop: upstream's CUDA extensions only build on Linux, and the
        # venv layout below assumes env/bin/python (Windows venvs use
        # env\Scripts\python.exe) — continuing would fail with a confusing
        # filesystem error instead of this message. On macOS use the
        # trellis.cpp runtime instead (docs/TRELLIS2.md).
        raise SystemExit(
            "TRELLIS.2's Python sidecar supports Linux + NVIDIA GPUs only. "
            "On macOS/Windows install the trellis.cpp runtime instead — see "
            "docs/TRELLIS2.md.")
    if sys.version_info < (3, 10):
        raise SystemExit("Python >= 3.10 required")
    if shutil.which("git") is None:
        raise SystemExit("git is required")
    if shutil.which("nvidia-smi") is None:
        log("WARNING: nvidia-smi not found - no NVIDIA GPU detected. "
            "TRELLIS.2 needs an NVIDIA GPU (>= 24 GB VRAM recommended).")

    dest = os.path.abspath(args.dest)
    env_dir = os.path.join(dest, "env")
    os.makedirs(dest, exist_ok=True)
    log(f"installing into {dest}")

    # 1. venv ---------------------------------------------------------------
    py = os.path.join(env_dir, "bin", "python")
    if not os.path.exists(py):
        log("creating virtualenv")
        venv.EnvBuilder(with_pip=True, upgrade_deps=True).create(env_dir)
    pip = [py, "-m", "pip", "install", "--no-input"]

    # 2. torch + base requirements ------------------------------------------
    if not args.skip_torch:
        run(pip + ["--index-url", TORCH_INDEX] + TORCH_SPEC)
    req = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "requirements.txt")
    run(pip + ["-r", req])
    if args.attn == "flash-attn":
        # needs torch importable at build time
        run(pip + ["--no-build-isolation", FLASH_ATTN_SPEC])
    else:
        run(pip + ["xformers"])
        log("remember to run generation with ATTN_BACKEND=xformers")

    # 3. pinned checkouts -----------------------------------------------------
    trellis_dir = os.path.join(dest, "TRELLIS.2")
    clone_pinned(TRELLIS2_REPO, TRELLIS2_REV, trellis_dir)
    patch_ovoxel_init(trellis_dir)
    cumesh_dir = os.path.join(dest, "CuMesh")
    clone_pinned(CUMESH_REPO, CUMESH_REV, cumesh_dir)
    flexgemm_dir = os.path.join(dest, "FlexGEMM")
    clone_pinned(FLEXGEMM_REPO, FLEXGEMM_REV, flexgemm_dir)

    # NOTE: never `pip install cumesh` - the PyPI project of that name is an
    # unrelated, unlicensed package (see docs/trellis2-dependencies.md).
    build_env = dict(os.environ, MAX_JOBS=str(args.jobs))
    run(pip + ["--no-build-isolation", os.path.join(trellis_dir, "o-voxel")],
        env=build_env)
    run(pip + ["--no-build-isolation", cumesh_dir], env=build_env)
    run(pip + ["--no-build-isolation", flexgemm_dir], env=build_env)

    # 4. make `trellis2` importable from the pinned checkout ------------------
    site = subprocess.run(
        [py, "-c", "import sysconfig;print(sysconfig.get_paths()['purelib'])"],
        capture_output=True, text=True, check=True).stdout.strip()
    with open(os.path.join(site, "qtmesh_trellis2.pth"), "w") as f:
        f.write(trellis_dir + "\n")

    # 5. prohibited-module check (Phase 12/13) --------------------------------
    probe = subprocess.run(
        [py, "-c",
         "import importlib.util as u;"
         "print(int(u.find_spec('nvdiffrast') is not None),"
         "int(u.find_spec('nvdiffrec') is not None),"
         "int(u.find_spec('nvdiffrec_render') is not None))"],
        capture_output=True, text=True, check=True).stdout.split()
    if any(p != "0" for p in probe):
        log("WARNING: a prohibited NVIDIA research-only package (nvdiffrast/"
            "nvdiffrec) is present in the environment. QtMeshEditor never "
            "uses it; uninstall it to keep the environment license-clean.")

    # 6. verify the core imports WITHOUT nvdiffrast ---------------------------
    log("verifying imports (torch-free modules)")
    run([py, "-c", "import o_voxel, o_voxel.convert; print('o_voxel ok')"])
    run([py, "-c", "import trellis2; print('trellis2 ok')"])

    # 7. copy the sidecar scripts + write the runtime marker ------------------
    here = os.path.dirname(os.path.abspath(__file__))
    for name in ("generate.py", "qtm3d.py", "requirements.txt",
                 "THIRD_PARTY_LICENSES.md"):
        src = os.path.join(here, name)
        if os.path.exists(src) and os.path.abspath(here) != dest:
            shutil.copy2(src, os.path.join(dest, name))
    with open(os.path.join(dest, "runtime.json"), "w") as f:
        json.dump({
            "schema": "qtmesh-trellis2-runtime-v1",
            "python": os.path.join(env_dir, "bin", "python"),
            "generate": os.path.join(dest, "generate.py"),
            "trellis2Revision": TRELLIS2_REV,
            "cumeshRevision": CUMESH_REV,
            "flexgemmRevision": FLEXGEMM_REV,
            "attnBackend": args.attn,
            "platform": platform.platform(),
        }, f, indent=2)
    log("done. QtMeshEditor will auto-detect this runtime "
        "(or set QTMESH_TRELLIS2_ENV / QSettings ai/trellis2Env to "
        f"{dest}).")
    log("First generation downloads microsoft/TRELLIS.2-4B (MIT, ~19 GB) and "
        "the GATED facebook/dinov3-vitl16 encoder - accept Meta's DINOv3 "
        "License on Hugging Face and `huggingface-cli login` first.")


if __name__ == "__main__":
    main()
