#!/bin/bash
# Publish the character generator bundles (built by scripts/build-character-assets.py).
# Pass the directory holding mhr_character.qmcb, ict_character.qmcb, hair_character.qmcb,
# LICENSE-MHR.txt, LICENSE-ICT-FaceKit.txt, LICENSE-MakeHuman-hair.txt.
#
# Default: refresh the character-assets-v1 GitHub release that CharacterAssets
# downloads from on first use. Optionally also mirror to the HF models repo
# under character/ (needs a WRITE token: `hf auth login`).
#
# The release is always a --prerelease so the in-app updater never treats it as
# an application version (GitHubReleaseParser also skips non-semver tags).
#
# After uploading, the SHA-256 digests pinned in src/CharacterGen/CharacterAssets.cpp
# must match these files (sha256sum them). If digests change, bump the release tag
# AND update kDefaultBaseUrl + the FileSpec table together — this script refuses
# to --clobber when digests diverge from the pins below.
set -euo pipefail
DIR="${1:?usage: $0 <bundle-dir>}"
TAG="${CHARACTER_ASSETS_TAG:-character-assets-v1}"
REPO="${CHARACTER_ASSETS_REPO:-fernandotonon/QtMeshEditor}"

# Keep in sync with src/CharacterGen/CharacterAssets.cpp FileSpec table.
declare -A PINNED=(
  [mhr_character.qmcb]=58f3903e6e93026e18c9dbffdaf2634fa8815b919f93f6cf5e347425bacbec6c
  [ict_character.qmcb]=f1b74cebf2bc8e21276e7f1ce717b0c52122bb1def9cb214052364952ae657f1
  [LICENSE-MHR.txt]=cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30
  [LICENSE-ICT-FaceKit.txt]=b20a78d8fc51657e14f3e00c8abb538d786ad00aa63210328c22565e583167e1
  [hair_character.qmcb]=69cbb5ec8fe23a13ed70313e6b9983b6df3e0d3abb5d71cb2e3fd7d8c693c716
  [LICENSE-MakeHuman-hair.txt]=1397704a53d998016e743c3add6801160c2b615536eed3b0c7cfd0911f3e38eb
)

for f in "${!PINNED[@]}"; do
  [ -f "$DIR/$f" ] || { echo "missing $DIR/$f" >&2; exit 1; }
  got=$(sha256sum "$DIR/$f" | awk '{print $1}')
  want=${PINNED[$f]}
  if [ "$got" != "$want" ]; then
    echo "ABORT: $f digest $got does not match the pin $want" >&2
    echo "Bump CHARACTER_ASSETS_TAG + kDefaultBaseUrl + FileSpec pins together; do not clobber $TAG." >&2
    exit 1
  fi
done

FILES=(
  "$DIR/mhr_character.qmcb" "$DIR/ict_character.qmcb" "$DIR/hair_character.qmcb"
  "$DIR/LICENSE-MHR.txt" "$DIR/LICENSE-ICT-FaceKit.txt" "$DIR/LICENSE-MakeHuman-hair.txt"
)

echo ">> GitHub release $REPO/$TAG (prerelease)"
if gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then
  gh release upload "$TAG" -R "$REPO" --clobber "${FILES[@]}"
  gh release edit "$TAG" -R "$REPO" --prerelease
else
  gh release create "$TAG" -R "$REPO" --prerelease \
    --title "Character generator assets (MHR + ICT-FaceKit + hair)" \
    --notes "Bundled character-generator data for first-use download. Digests pinned in CharacterAssets.cpp. Published as a prerelease so the app updater ignores it." \
    "${FILES[@]}"
fi

if [ "${UPLOAD_TO_HF:-0}" = "1" ]; then
  echo ">> Hugging Face fernandotonon/QtMeshEditor-models character/"
  huggingface-cli upload fernandotonon/QtMeshEditor-models "$DIR" character/ --include "*.qmcb" "LICENSE-*.txt" --repo-type model
else
  echo "(skip HF mirror; set UPLOAD_TO_HF=1 with a write token to also push character/)"
fi
