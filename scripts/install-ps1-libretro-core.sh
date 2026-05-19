#!/usr/bin/env bash
# Install a working libretro PSX core (+ deps) into QtMeshEditor PS1Cores/.
# Prefers the libretro buildbot nightly (Arch/Ubuntu distro cores often crash on .iso).
# Usage: ./scripts/install-ps1-libretro-core.sh [path/to/build/debug]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${1:-$ROOT/build/debug}/PS1Cores"
mkdir -p "$OUT_DIR"

install_file() {
  local src="$1"
  if [[ -f "$src" ]]; then
    cp -f "$src" "$OUT_DIR/"
    echo "Installed $(basename "$src") -> $OUT_DIR/"
    return 0
  fi
  return 1
}

fetch_buildbot_beetle() {
  local zip="/tmp/mednafen_psx_libretro.so.zip"
  local url="https://buildbot.libretro.com/nightly/linux/x86_64/latest/mednafen_psx_libretro.so.zip"
  echo "Downloading mednafen_psx_libretro from libretro buildbot ($url)..."
  if ! curl -fsSL -o "$zip" "$url"; then
    return 1
  fi
  rm -rf /tmp/beetle-buildbot
  mkdir -p /tmp/beetle-buildbot
  unzip -o -q "$zip" -d /tmp/beetle-buildbot
  install_file /tmp/beetle-buildbot/mednafen_psx_libretro.so
}

fetch_arch_beetle() {
  local pkg="/tmp/libretro-beetle-psx.pkg.tar.zst"
  local url="https://archlinux.org/packages/extra/x86_64/libretro-beetle-psx/download/"
  echo "Downloading libretro-beetle-psx from Arch Linux ($url)..."
  if ! curl -fsSL -o "$pkg" "$url"; then
    return 1
  fi
  rm -rf /tmp/beetle-extract
  mkdir -p /tmp/beetle-extract
  if command -v bsdtar >/dev/null 2>&1; then
    bsdtar --zstd -xf "$pkg" -C /tmp/beetle-extract
  else
    zstd -d "$pkg" -o /tmp/beetle.pkg.tar -f
    tar -xf /tmp/beetle.pkg.tar -C /tmp/beetle-extract
  fi
  install_file /tmp/beetle-extract/usr/lib/libretro/mednafen_psx_libretro.so
  # beetle may need libtrio at runtime on some distros
  for dep in /tmp/beetle-extract/usr/lib/libtrio.so*; do
    [[ -e "$dep" ]] && install_file "$dep"
  done
}

if [[ -n "${QTMESH_PS1_LIBRETRO_CORE:-}" ]]; then
  install_file "$QTMESH_PS1_LIBRETRO_CORE"
  exit 0
fi

if fetch_buildbot_beetle; then
  echo "Using libretro buildbot mednafen_psx_libretro (recommended for .iso rips)."
  exit 0
fi

if fetch_arch_beetle; then
  echo "Warning: using Arch libretro-beetle-psx; .iso may need BIN+CUE. Prefer buildbot download." >&2
  exit 0
fi

for candidate in \
  /usr/lib/x86_64-linux-gnu/libretro/mednafen_psx_libretro.so \
  /usr/lib/aarch64-linux-gnu/libretro/mednafen_psx_libretro.so \
  /usr/local/lib/libretro/mednafen_psx_libretro.so; do
  if install_file "$candidate"; then
    echo "Warning: using distro core; install may crash on .iso — re-run script for buildbot core." >&2
    exit 0
  fi
done

echo "Failed to install libretro PSX core. Set QTMESH_PS1_LIBRETRO_CORE to a .so path." >&2
exit 1
