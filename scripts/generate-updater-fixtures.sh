#!/usr/bin/env bash
# Regenerate minisign test fixtures under tests/fixtures/updater/.
# Requires: minisign, curl, libsodium (for the minisign binary).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
FIXTURE_DIR="$ROOT/tests/fixtures/updater"
RELEASE_TAG="${1:-3.5.3}"

mkdir -p "$FIXTURE_DIR"
cd "$FIXTURE_DIR"

KEY="$FIXTURE_DIR/minisign-test.key"
PUB="$FIXTURE_DIR/minisign-test.pub"

if [[ ! -f "$KEY" ]]; then
  minisign -G -f -W -s "$KEY" -p "$PUB"
  echo "Generated new test keypair. Do NOT commit $KEY."
fi

README="$FIXTURE_DIR/release-${RELEASE_TAG}-readme.md"
curl -fsSL "https://raw.githubusercontent.com/fernandotonon/QtMeshEditor/${RELEASE_TAG}/README.md" -o "$README"

minisign -S -s "$KEY" -x "${README}.minisig" -m "$README" \
  -c "spike fixture from release ${RELEASE_TAG} README" \
  -t "timestamp:0	file:release-${RELEASE_TAG}-readme.md"

echo "Fixtures updated. Commit:"
echo "  ${PUB}"
echo "  ${README}"
echo "  ${README}.minisig"
PUB_B64=$(grep -v '^untrusted' "$PUB" | grep -v '^#' | tr -d '\n\r')
echo "Update kTestPublicKeyBase64 in MinisignVerify_test.cpp if the keypair changed:"
echo "  ${PUB_B64}"
