#!/bin/bash
# Upload the character generator bundles (built by scripts/build-character-assets.py) to the HF models repo under character/.
# Pass the directory holding mhr_character.qmcb, ict_character.qmcb, LICENSE-MHR.txt, LICENSE-ICT-FaceKit.txt.
# After uploading, the SHA-256 digests pinned in src/CharacterGen/CharacterAssets.cpp must match these files (sha256sum them).
set -euo pipefail
DIR="${1:?usage: $0 <bundle-dir>}"
for f in mhr_character.qmcb ict_character.qmcb LICENSE-MHR.txt LICENSE-ICT-FaceKit.txt; do
  [ -f "$DIR/$f" ] || { echo "missing $DIR/$f" >&2; exit 1; }
done
huggingface-cli upload fernandotonon/QtMeshEditor-models "$DIR" character/ --include "*.qmcb" "LICENSE-*.txt" --repo-type model
