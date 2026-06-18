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
artifact_kind=""

while IFS='=' read -r key value; do
  case "$key" in
    parent_pid) parent_pid="$value" ;;
    install_root) install_root="$value" ;;
    payload_dir) payload_dir="$value" ;;
    executable_path) executable_path="$value" ;;
    artifact_kind) artifact_kind="$value" ;;
  esac
done < "$manifest"

if [[ -z "$install_root" || -z "$executable_path" || -z "$payload_dir" ]]; then
  exit 2
fi

if [[ -n "$parent_pid" ]]; then
  while kill -0 "$parent_pid" 2>/dev/null; do
    sleep 0.2
  done
fi

if [[ "$artifact_kind" == "app_image" ]]; then
  mv -f "$payload_dir" "$executable_path"
  chmod +x "$executable_path"
  exec "$executable_path" "$@"
fi

if ! command -v rsync >/dev/null 2>&1; then
  echo "rsync is required for tarball installs" >&2
  exit 3
fi

rsync -a --delete "${payload_dir}/" "${install_root}/"
exec "$executable_path" "$@"
