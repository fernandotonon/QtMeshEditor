#!/usr/bin/env python3
"""QtMeshEditor Kimodo runtime installer.

Creates an ISOLATED Python environment for the NVIDIA Kimodo text-to-motion
backend (nothing here touches QtMeshEditor's own dependencies), pinned to the
revision audited for this integration:

    Kimodo   nv-tlabs/kimodo  e1c219c99b4b94ba48be1fb6a1cbc64ecee4c954 (Apache-2.0 code)

Model weights are NOT fetched here; they download on the first generation:
    nvidia/Kimodo-SOMA-RP-v1.2   NVIDIA Open Model License (commercial use allowed)
    McGill-NLP/LLM2Vec-Meta-Llama-3-8B-Instruct-mntp(+ -supervised)   MIT (LoRA adapters)
    meta-llama/Meta-Llama-3-8B-Instruct   GATED, Meta Llama 3 Community License (~16 GB):
        accept the licence on Hugging Face and log in (`hf auth login`) first.
QtMeshEditor never stores, passes or logs the Hugging Face token: the
Hugging Face libraries read the user's own login.

Apple Silicon / other ARM: Kimodo's MotionCorrection extension (its foot-skate
cleanup) calls SSE intrinsics directly and is compiled with -msse4.1 -mavx,
which ARM compilers reject. With --arm-sse2neon the installer (1) adds those
flags only on x86, (2) routes SIMD.h through sse2neon (MIT, pinned below,
SHA-256 verified) on ARM, (3) defines Kimodo's FORCE_INLINE for Clang (upstream
only handles MSVC and GCC) and (4) replaces its one AVX-only intrinsic. Every
patched file is verified against their
pristine SHA-256 first, so a moved upstream fails loudly instead of being
patched blindly. Without the flag, ARM installs stop with an explanation.

Layout under --dest (default: <QtMeshEditor app data>/kimodo):
    env/          the virtualenv
    kimodo/       the pinned upstream checkout
    runtime.json  marker read by QtMeshEditor to detect the runtime
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
import urllib.request

KIMODO_REPO = "https://github.com/nv-tlabs/kimodo.git"
KIMODO_REV = "e1c219c99b4b94ba48be1fb6a1cbc64ecee4c954"

SSE2NEON_URL = "https://raw.githubusercontent.com/DLTcollab/sse2neon/v1.8.0/sse2neon.h"
SSE2NEON_SHA256 = "07723c9f9457dd4316f1fde3dd4eb6f31dd67d9955f6c21f4e609ac1698be48a"
CMAKELISTS_SHA256 = "768c026a9e6c176c5ef5f58cc7773a7b1d0597e872988ae5a908ab9621d94073"
SIMD_H_SHA256 = "ce7e81a3a4f07d576510e85f45665094eb28a2849ade94415d88bb7d4f3a3b9e"
COMPILER_H_SHA256 = "257119fdd0346b6c56d6e07860e84c43840db196c2264b218ccdfd737c4dd60d"
VECTOR_INL_SHA256 = "3a5378ca1e62aac587fdac66254ba4ca37e072ce9b036a74525555bae0c2f1d5"


def log(msg: str) -> None:
    print(f"[kimodo-install] {msg}", flush=True)


def run(cmd: list[str], **kw) -> None:
    log("$ " + " ".join(cmd))
    subprocess.run(cmd, check=True, **kw)


def sha256(path: str) -> str:
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def default_dest() -> str:
    # Must match AppStorage::persistentRoot() (QStandardPaths AppDataLocation: organisation + application).
    if os.environ.get("QTMESH_KIMODO_ENV"):
        return os.environ["QTMESH_KIMODO_ENV"]
    if os.environ.get("SNAP_USER_COMMON"):   # AppStorage::persistentRoot() under Snap: stable across refreshes, flat name
        return os.path.join(os.environ["SNAP_USER_COMMON"], "QtMeshEditor", "kimodo")
    if sys.platform.startswith("linux"):
        base = os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share"))
    elif sys.platform == "darwin":
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("APPDATA", os.path.expanduser("~"))
    return os.path.join(base, "QtMeshEditor", "QtMeshEditor", "kimodo")


def clone_pinned(dest: str) -> None:
    if not os.path.isdir(os.path.join(dest, ".git")):
        run(["git", "clone", "--filter=blob:none", KIMODO_REPO, dest])
    run(["git", "-C", dest, "fetch", "origin", KIMODO_REV])
    run(["git", "-C", dest, "checkout", "--force", "--detach", KIMODO_REV])


def replace_once(path: str, old: str, new: str) -> None:
    text = open(path, encoding="utf-8").read()
    if text.count(old) != 1:
        raise SystemExit(f"{path}: patch anchor not found exactly once; refusing to continue")
    open(path, "w", encoding="utf-8").write(text.replace(old, new))


def patch_for_arm(src: str) -> None:
    mc = os.path.join(src, "MotionCorrection")
    cml = os.path.join(mc, "CMakeLists.txt")
    simd = os.path.join(mc, "src", "cpp", "Math", "SIMD.h")
    compiler = os.path.join(mc, "src", "cpp", "Compiler.h")
    vector = os.path.join(mc, "src", "cpp", "Math", "Vector.inl")
    for path, want in ((cml, CMAKELISTS_SHA256), (simd, SIMD_H_SHA256), (compiler, COMPILER_H_SHA256),
                       (vector, VECTOR_INL_SHA256)):
        if sha256(path) != want:
            raise SystemExit(f"{path} does not match the audited revision; refusing to patch it")

    header = os.path.join(os.path.dirname(simd), "sse2neon.h")
    log(f"downloading sse2neon (MIT) from {SSE2NEON_URL}")
    with urllib.request.urlopen(SSE2NEON_URL, timeout=60) as r:
        data = r.read()
    if hashlib.sha256(data).hexdigest() != SSE2NEON_SHA256:
        raise SystemExit("sse2neon.h SHA-256 mismatch; refusing to use it")
    with open(header, "wb") as f:
        f.write(data)

    # x86-only SIMD flags.
    replace_once(cml,
                 "    target_compile_options(motion_correction_cpp_base PRIVATE -Wall -Wextra -msse4.1 -mavx)\n",
                 "    target_compile_options(motion_correction_cpp_base PRIVATE -Wall -Wextra)\n"
                 '    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86" AND NOT CMAKE_OSX_ARCHITECTURES MATCHES "arm64")\n'
                 "        target_compile_options(motion_correction_cpp_base PRIVATE -msse4.1 -mavx)\n"
                 "    endif()\n")
    # sse2neon on ARM. It defines (and leaves defined) its own FORCE_INLINE, which
    # would replace Kimodo's, so the include is fenced with push/pop_macro.
    replace_once(simd, "#include <immintrin.h>\n",
                 "#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__)\n"
                 '#pragma push_macro("FORCE_INLINE")\n#undef FORCE_INLINE\n'
                 '#include "sse2neon.h"  // patched by QtMeshEditor ai/kimodo/install.py\n'
                 '#undef FORCE_INLINE\n#pragma pop_macro("FORCE_INLINE")\n'
                 "#else\n#include <immintrin.h>\n#endif\n")
    # Upstream defines FORCE_INLINE for MSVC and GCC only: Clang (every macOS build) gets none.
    replace_once(compiler, "#elif defined(COMPILER_GNUC)\n\t#define FORCE_INLINE",
                 "#elif defined(COMPILER_GNUC) || defined(COMPILER_CLANG)\n\t#define FORCE_INLINE")
    # _mm_permutevar_ps is AVX (sse2neon does not provide it): a portable lane gather on ARM.
    replace_once(vector,
                 "        __m128i vControl = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&elem[0]));\n"
                 "        return _mm_permutevar_ps(m_data, vControl);\n",
                 "#if defined(__aarch64__) || defined(_M_ARM64) || defined(__arm__)\n"
                 "        alignas(16) float src[4];\n"
                 "        _mm_store_ps(src, m_data);\n"
                 "        return _mm_setr_ps(src[elem[0]], src[elem[1]], src[elem[2]], src[elem[3]]);\n"
                 "#else\n"
                 "        __m128i vControl = _mm_loadu_si128(reinterpret_cast<const __m128i*>(&elem[0]));\n"
                 "        return _mm_permutevar_ps(m_data, vControl);\n"
                 "#endif\n")
    log("patched MotionCorrection for ARM (x86-only flags, sse2neon, Clang FORCE_INLINE, permutevar)")


def main() -> None:
    ap = argparse.ArgumentParser(description="Install the Kimodo runtime for QtMeshEditor")
    ap.add_argument("--dest", default=default_dest())
    ap.add_argument("--python", default=sys.executable, help="interpreter for the venv (>= 3.10)")
    ap.add_argument("--torch-index", default="", help="pip index for torch (e.g. a CUDA wheel index)")
    ap.add_argument("--arm-sse2neon", action="store_true",
                    help="ARM only: build MotionCorrection with the pinned sse2neon header (see module doc)")
    args = ap.parse_args()

    # The venv is created BY the requested interpreter, so validate that one, not the one running this script.
    try:
        ver = subprocess.run([args.python, "-c", "import sys; print('%d.%d' % sys.version_info[:2])"],
                             check=True, capture_output=True, text=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError) as e:
        raise SystemExit(f"cannot run --python {args.python}: {e}")
    if tuple(int(x) for x in ver.split(".")) < (3, 10):
        raise SystemExit(f"Python >= 3.10 is required ({args.python} is {ver})")
    dest = os.path.abspath(args.dest)
    os.makedirs(dest, exist_ok=True)
    env_dir = os.path.join(dest, "env")
    src = os.path.join(dest, "kimodo")

    is_arm = platform.machine().lower() in ("arm64", "aarch64")
    if is_arm and not args.arm_sse2neon:
        raise SystemExit("ARM machine: Kimodo's MotionCorrection extension uses x86 SIMD. Re-run with --arm-sse2neon "
                         "to build it through the pinned, hash-verified sse2neon header.")

    clone_pinned(src)
    if is_arm:
        patch_for_arm(src)

    if not os.path.isdir(env_dir):
        log(f"creating virtualenv {env_dir}")
        run([args.python, "-m", "venv", env_dir])
    py = os.path.join(env_dir, "Scripts" if os.name == "nt" else "bin", "python.exe" if os.name == "nt" else "python")
    run([py, "-m", "pip", "install", "--upgrade", "pip"])
    torch_cmd = [py, "-m", "pip", "install", "torch"]
    if args.torch_index:
        torch_cmd += ["--index-url", args.torch_index]
    run(torch_cmd)
    run([py, "-m", "pip", "install", "-e", src])
    run([py, "-c", "import kimodo, torch; print('kimodo ok, torch', torch.__version__)"])

    here = os.path.dirname(os.path.abspath(__file__))
    for name in ("generate.py", "textenc_quant.py", "somaskel77_rest.json", "README.md"):
        s = os.path.join(here, name)
        if os.path.exists(s) and os.path.abspath(here) != dest:
            shutil.copy2(s, os.path.join(dest, name))
    with open(os.path.join(dest, "runtime.json"), "w", encoding="utf-8") as f:
        json.dump({
            "schema": "qtmesh-kimodo-runtime-v1",
            "python": py,
            "generate": os.path.join(dest, "generate.py"),
            "kimodoRevision": KIMODO_REV,
            "armSse2neon": bool(is_arm),
            "platform": platform.platform(),
        }, f, indent=2)
    log(f"done. QtMeshEditor detects this runtime automatically (or set QTMESH_KIMODO_ENV={dest}).")
    log("Before the first generation: accept Meta's licence for meta-llama/Meta-Llama-3-8B-Instruct on Hugging Face "
        "and run `hf auth login` (the text encoder is ~16 GB).")


if __name__ == "__main__":
    main()
