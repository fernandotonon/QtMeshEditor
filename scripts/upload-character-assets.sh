#!/bin/bash
# Publish the character generator bundles (built by scripts/build-character-assets.py).
# Pass the directory holding mhr_character.qmcb, ict_character.qmcb, hair_character.qmcb,
# LICENSE-MHR.txt, LICENSE-ICT-FaceKit.txt, LICENSE-MakeHuman-hair.txt.
#
# Default: refresh the character-assets-v1 GitHub release that CharacterAssets
# downloads from on first use. Optionally also mirror to the HF models repo
# under character/ (needs a WRITE token: `hf auth login`).
#
# After uploading, the SHA-256 digests pinned in src/CharacterGen/CharacterAssets.cpp
# must match these files (sha256sum them). If digests change, bump the release tag
# AND update kDefaultBaseUrl + the FileSpec table together.
set -euo pipefail
DIR="${1:?usage: $0 <bundle-dir>}"
TAG="${CHARACTER_ASSETS_TAG:-character-assets-v1}"
REPO="${CHARACTER_ASSETS_REPO:-fernandotonon/QtMeshEditor}"
for f in mhr_character.qmcb ict_character.qmcb LICENSE-MHR.txt LICENSE-ICT-FaceKit.txt hair_character.qmcb LICENSE-MakeHuman-hair.txt; do
  [ -f "$DIR/$f" ] || { echo "missing $DIR/$f" >&2; exit 1; }
done

echo ">> GitHub release $REPO/$TAG"
if gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then
  gh release upload "$TAG" -R "$REPO" --clobber \
    "$DIR/mhr_character.qmcb" "$DIR/ict_character.qmcb" "$DIR/hair_character.qmcb" \
    "$DIR/LICENSE-MHR.txt" "$DIR/LICENSE-ICT-FaceKit.txt" "$DIR/LICENSE-MakeHuman-hair.txt"
else
  gh release create "$TAG" -R "$REPO" \
    --title "Character generator assets (MHR + ICT-FaceKit + hair)" \
    --notes "Bundled character-generator data for first-use download. Digests pinned in CharacterAssets.cpp." \
    "$DIR/mhr_character.qmcb" "$DIR/ict_character.qmcb" "$DIR/hair_character.qmcb" \
    "$DIR/LICENSE-MHR.txt" "$DIR/LICENSE-ICT-FaceKit.txt" "$DIR/LICENSE-MakeHuman-hair.txt"
fi

if [ "${UPLOAD_TO_HF:-0}" = "1" ]; then
  echo ">> Hugging Face fernandotonon/QtMeshEditor-models character/"
  huggingface-cli upload fernandotonon/QtMeshEditor-models "$DIR" character/ --include "*.qmcb" "LICENSE-*.txt" --repo-type model
else
  echo "(skip HF mirror; set UPLOAD_TO_HF=1 with a write token to also push character/)"
fi
