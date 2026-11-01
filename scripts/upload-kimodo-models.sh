#!/usr/bin/env bash
# Upload the native Kimodo models (src/AIMotion/KimodoNative.cpp). ONE-TIME, run by a maintainer with write access,
# AFTER scripts/export-kimodo-onnx.py (ONNX_DIR) and scripts/export-kimodo-text-encoder-gguf.py (GGUF) ran.
#
# Two homes, because the licences differ:
#   ONNX set (NVIDIA Open Model License) -> the aggregate repo  QtMeshEditor-models:kimodo/<file>
#                                           + the dedicated mirror QtMeshEditor-kimodo-onnx
#   text encoder (Meta Llama 3 derivative) -> its own repo, named "Llama-3-..." (Llama 3 licence 1.b.i), with the
#                                           licence, the Acceptable Use Policy and the "Notice" file (1.b.iii)
#
# NB: the app's default URLs are pinned to HF COMMITS together with SHA-256 digests (KimodoNative.cpp). After
# uploading new files, update the revisions and the digests there (this script prints both).
#
# Prereqs: hf auth login (write token)
# Usage:   ONNX_DIR=~/kimodo_export/host_models/kimodo TEXTENC_DIR=~/kimodo_export/host_textenc ./scripts/upload-kimodo-models.sh
set -euo pipefail

AGG="${AGG:-fernandotonon/QtMeshEditor-models}"
DED="${DED:-fernandotonon/QtMeshEditor-kimodo-onnx}"
TXT="${TXT:-fernandotonon/Llama-3-LLM2Vec-8B-Kimodo-TextEncoder-GGUF}"
ONNX_DIR="${ONNX_DIR:?set ONNX_DIR}"
TEXTENC_DIR="${TEXTENC_DIR:?set TEXTENC_DIR}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ONNX_FILES=(LICENSE-NVIDIA-OML.txt NOTICE-Kimodo.txt kimodo_meta.json kimodo_decode.onnx kimodo_encode.onnx kimodo_transition.onnx kimodo_denoiser.onnx)
TXT_FILES=(LICENSE-Llama-3.txt USE_POLICY-Llama-3.md NOTICE NOTICE-Llama-3.txt LICENSE-LLM2Vec-MIT.txt Llama-3-LLM2Vec-8B-Kimodo-TextEncoder-Q8_0.gguf)

retry() { local n=0; until "$@"; do n=$((n+1)); [ $n -ge 8 ] && return 1; echo "retry $n..."; sleep 10; done; }
for f in "${ONNX_FILES[@]}"; do [ -f "$ONNX_DIR/$f" ] || { echo "missing $ONNX_DIR/$f" >&2; exit 1; }; done
for f in "${TXT_FILES[@]}"; do [ -f "$TEXTENC_DIR/$f" ] || { echo "missing $TEXTENC_DIR/$f" >&2; exit 1; }; done

hf repo create "$DED" --repo-type model --exist-ok
hf repo create "$TXT" --repo-type model --exist-ok

# Licence texts first: a repo is never public without them.
retry hf upload "$TXT" "$TEXTENC_DIR" . --include "LICENSE-*" --include "USE_POLICY-*" --include "NOTICE*" --commit-message "Licence, Acceptable Use Policy, Notice"
retry hf upload "$TXT" "$HERE/kimodo_textenc_model_card.md" README.md --commit-message "Model card"
retry hf upload "$TXT" "$TEXTENC_DIR/Llama-3-LLM2Vec-8B-Kimodo-TextEncoder-Q8_0.gguf" Llama-3-LLM2Vec-8B-Kimodo-TextEncoder-Q8_0.gguf \
    --commit-message "Llama-3-LLM2Vec-8B-Kimodo-TextEncoder Q8_0"

for f in "${ONNX_FILES[@]}"; do retry hf upload "$AGG" "$ONNX_DIR/$f" "kimodo/$f" --commit-message "kimodo: $f"; done
for f in "${ONNX_FILES[@]}"; do retry hf upload "$DED" "$ONNX_DIR/$f" "$f" --commit-message "$f"; done
retry hf upload "$DED" "$HERE/kimodo_onnx_model_card.md" README.md --commit-message "Model card"

echo "== pin these in src/AIMotion/KimodoNative.cpp =="
"${PYTHON:-python3}" - "$AGG" "$TXT" <<'EOF'
import sys
from huggingface_hub import HfApi
api = HfApi()
for repo, prefix in ((sys.argv[1], "kimodo/"), (sys.argv[2], "")):
    info = api.model_info(repo, files_metadata=True)
    print(repo, "revision", info.sha)
    for s in info.siblings:
        if s.rfilename.startswith(prefix) and s.lfs:
            print("  ", s.rfilename, s.lfs.sha256)
EOF
