#!/usr/bin/env bash
# Populate media/hdri/ with the Slice F (#472) bundled CC0 HDRIs (2k) + synthetic flat_grey.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${ROOT}/media/hdri"
mkdir -p "$OUT"

poly_url() {
    local id="$1"
    python3 - <<PY
import json, sys, urllib.request
id = "${id}"
req = urllib.request.Request(
    f"https://api.polyhaven.com/files/{id}",
    headers={"User-Agent": "QtMeshEditor/3.0 (https://github.com/fernandotonon/QtMeshEditor)"},
)
with urllib.request.urlopen(req, timeout=120) as r:
    data = json.load(r)
url = data["hdri"]["2k"]["hdr"]["url"]
print(url)
PY
}

download_poly() {
    local id="$1" dest="$2"
    if [[ -f "${OUT}/${dest}" ]] && [[ -s "${OUT}/${dest}" ]]; then
        echo "skip ${dest} (already present)"
        return
    fi
    echo "fetch ${dest} from Poly Haven (${id})…"
    local url
    url="$(poly_url "$id")"
    curl -fsSL "$url" -o "${OUT}/${dest}"
}

download_poly studio_small_09 studio_neutral.hdr
download_poly sunset_forest sunset_outdoor.hdr
download_poly overcast_soil_puresky overcast_outdoor.hdr
download_poly anniversary_lounge indoor_window.hdr

python3 - <<'PY' "${OUT}/flat_grey.hdr"
import math, struct, sys

def float_to_rgbe(v: float) -> tuple[int, int, int, int]:
    if v <= 1e-32:
        return (0, 0, 0, 0)
    e = int(math.floor(math.log2(v))) + 1
    m = v * 256.0 / (2 ** e)
    return (int(m), int(m), int(m), e + 128)

def write_flat(path: str, width: int = 512, height: int = 256, value: float = 0.5) -> None:
    pixel = bytes(float_to_rgbe(value))
    with open(path, "wb") as f:
        f.write(b"#?RADIANCE\n# QtMeshEditor synthetic neutral grey\nFORMAT=32-bit_rle_rgbe\n\n")
        f.write(f"-Y {height} +X {width}\n".encode("ascii"))
        for _ in range(height):
            f.write(b"\x02\x02")
            f.write(struct.pack(">H", width))
            f.write(pixel * width)

write_flat(sys.argv[1])
print(f"wrote {sys.argv[1]}")
PY

echo "Bundled HDRIs ready under ${OUT}"
du -ch "${OUT}"/*.hdr 2>/dev/null | tail -1 || true
