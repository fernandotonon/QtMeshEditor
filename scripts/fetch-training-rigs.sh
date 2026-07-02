#!/usr/bin/env bash
# Assemble a CC0 corpus of RIGGED characters for segmentation training (#410).
#
# OFFLINE dev tool — NOT shipped; the app never runs it. The shipped weights are
# a derived work of the training data, so we only use sources we can legally
# train a REDISTRIBUTABLE (commercial-OK) model on. That rules out Mixamo
# (Adobe EULA), ShapeNet-Part / PartNet (non-commercial) and LAFAN1
# (CC-BY-NC-ND) — the same wall #404/#408/#409 hit. See THIRD_PARTY_AI_MODELS.md.
#
# WHY NOT FULLY AUTOMATED: the clean CC0 character sources (Quaternius, Poly
# Pizza, Kenney) distribute via Google Drive / itch.io / per-asset pages with no
# stable raw-file URLs — hardcoded download links rot within months. So this
# script does the maintainable thing: it records each CC0 source + its license
# below, fetches the few with stable direct URLs, and otherwise tells you the
# landing page to grab the pack from. Drop everything into ./training_rigs/ and
# run mine-training-data.sh — that step is the real workhorse and is fully
# automated over whatever you assemble.
#
# CURATED CC0 SOURCES (all "free for commercial use, no attribution required"):
#   1. Quaternius — Ultimate Animated Character Pack (50+ rigged humanoids)
#        https://quaternius.com/packs/ultimatedanimatedcharacter.html   [CC0]
#      Quaternius — Universal Base Characters (clean humanoid rigs, .glb)
#        https://quaternius.com/packs/universalbasecharacters.html       [CC0]
#   2. Poly Pizza — search "character" + filter license CC0, rigged glTF
#        https://poly.pizza/                                              [CC0]
#   3. Kenney — Blocky Characters / Mini Characters (rigged, stylised)
#        https://kenney.nl/assets/blocky-characters                       [CC0]
#   4. GitHub mirror with stable raw URLs, used for the v2 model corpus.
#      ONLY the Quaternius free-pack subfolders under
#      "FreeModels by Quaternius[Patreon]/Characters and Animals/" are CC0
#      (they mirror the packs Quaternius publishes as CC0 on quaternius.com);
#      the "[Patreon Exclusive]" folders MUST be skipped and the repo as a
#      whole carries no license grant — record each pack you actually take as
#      its own SOURCES.md row (pack name + this mirror URL + CC0), never a
#      blanket entry for the mirror:
#        https://github.com/beep2bleep/FreeAssetsByKenneyNLandQuaternius
#
# CC-BY sources are also acceptable IF you add the attribution to a NOTICE file
# shipped with the weights (e.g. some Sketchfab "downloadable + CC-BY" rigs).
#
# Usage:
#   scripts/fetch-training-rigs.sh [dest-dir]
#     dest-dir   where to place the downloaded rigs (default ./training_rigs)
#
# After this: scripts/mine-training-data.sh <dest-dir> ./mined_training_data
set -euo pipefail

DEST="${1:-./training_rigs}"
mkdir -p "$DEST"

# Provenance ledger: every file's source + license, written alongside the rigs
# so the training corpus is auditable (matches THIRD_PARTY_AI_MODELS.md rigor).
LEDGER="$DEST/SOURCES.md"
if [[ ! -f "$LEDGER" ]]; then
  cat > "$LEDGER" <<'EOF'
# Training-rig corpus provenance (segmentation #410)

Every mesh used to train the shipped meshseg.onnx must be CC0 or CC-BY (with
attribution recorded here). Add one line per source pack you download.

| pack | source URL | license | attribution |
|------|-----------|---------|-------------|
EOF
fi

# Try a direct download if a stable URL is available; otherwise print guidance.
# (Add stable CC0 direct URLs here as you confirm them; keep the ledger honest.)
declare -a DIRECT_URLS=(
  # "https://example.org/some-cc0-rigged-pack.glb"
)

fetched=0
for url in "${DIRECT_URLS[@]}"; do
  [[ -z "$url" ]] && continue
  fn="$DEST/$(basename "$url")"
  echo "Fetching $url"
  if curl -fSL --retry 3 -o "$fn" "$url"; then
    fetched=$((fetched+1))
    echo "| $(basename "$url") | $url | CC0 | — |" >> "$LEDGER"
  else
    echo "  failed: $url (skipped)" >&2
  fi
done

cat <<EOF

Downloaded $fetched file(s) with stable URLs into: $DEST

For the curated CC0 packs WITHOUT stable raw URLs (Quaternius / Poly Pizza /
Kenney — see the header of this script), download them manually from their
landing pages and unzip the rigged *.fbx / *.glb / *.gltf into:
    $DEST
then add a row to $LEDGER recording the pack + CC0 license.

When the corpus is assembled:
    scripts/mine-training-data.sh "$DEST" ./mined_training_data
    python scripts/export-meshseg-onnx.py --samples 6000 \\
        --real-data ./mined_training_data --out meshseg.onnx
EOF
