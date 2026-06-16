#!/usr/bin/env bash
# Print base64 of a minisign secret key for the MINISIGN_SECRET_KEY GitHub Actions secret.
# Usage: ./scripts/encode-minisign-secret-for-github.sh [path-to-minisign.key]
# Default path: ~/.minisign/minisign.key
set -euo pipefail

KEY="${1:-$HOME/.minisign/minisign.key}"

if [[ ! -f "$KEY" ]]; then
  echo "Secret key not found: $KEY" >&2
  exit 1
fi

echo "Copy the single line below into GitHub → Settings → Secrets → Actions → MINISIGN_SECRET_KEY"
echo "(Do not commit this output or paste it in issues/PRs.)"
echo
base64 "$KEY" | tr -d '\n'
echo
