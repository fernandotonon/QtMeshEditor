#!/usr/bin/env bash
set -euo pipefail

manifest="${1:-}"
if [[ -z "$manifest" || ! -f "$manifest" ]]; then
  exit 1
fi

parent_pid=""
install_root=""
payload_dir=""
executable_path=""

while IFS='=' read -r key value; do
  case "$key" in
    parent_pid) parent_pid="$value" ;;
    install_root) install_root="$value" ;;
    payload_dir) payload_dir="$value" ;;
    executable_path) executable_path="$value" ;;
  esac
done < "$manifest"

if [[ -z "$install_root" || -z "$payload_dir" || -z "$executable_path" ]]; then
  exit 2
fi

if [[ -n "$parent_pid" ]]; then
  while kill -0 "$parent_pid" 2>/dev/null; do
    sleep 0.2
  done
fi

source_app="${payload_dir}/QtMeshEditor.app"
if [[ ! -d "$source_app" ]]; then
  echo "Payload app bundle missing: $source_app" >&2
  exit 3
fi

tmp_root="${install_root}.new.$$"
bak_root="${install_root}.old.$$"
rm -rf "$tmp_root" "$bak_root"

if command -v rsync >/dev/null 2>&1; then
  rsync -a --delete "${source_app}/" "${tmp_root}/"
else
  ditto "${source_app}" "${tmp_root}"
fi

mv "$install_root" "$bak_root"
if mv "$tmp_root" "$install_root"; then
  rm -rf "$bak_root"
else
  mv "$bak_root" "$install_root"
  exit 4
fi

open -n "$install_root"
exit 0
