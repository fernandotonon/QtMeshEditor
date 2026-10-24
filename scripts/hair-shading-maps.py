#!/usr/bin/env python3
"""Turn the coloured MakeHuman hair textures of a hair bundle into neutral SHADING maps (greyscale, alpha kept).

The character generator tints them at runtime (HairTint.cpp), so one texture serves every hair colour. Each map is the
luminance of the original normalised so the median opaque pixel is 0.5 (the tint's mid stop) and twice the median is 1.0,
which keeps the strand detail and the root-to-tip gradient but drops the baked brown / green cast. The 'www.makehuman.org'
logo MakeHuman burns into the lower right corner of some textures is cleared: it is the only PURE WHITE content there (the hair is
dark brown), so only white pixels of that corner are removed (afro01/braid01/short01 use the corner for real hair and have none).

usage: hair-shading-maps.py in_hair_character.qmcb out_hair_character.qmcb
"""
import importlib.util, io, json, os, sys
import numpy as np
from PIL import Image

spec = importlib.util.spec_from_file_location("bha", os.path.join(os.path.dirname(os.path.abspath(__file__)), "build-hair-assets.py"))
bha = importlib.util.module_from_spec(spec); spec.loader.exec_module(bha)

LOGO = (0.83, 0.96)   # the watermark corner: x >= LOGO[0] * width, y >= LOGO[1] * height


def shading(png_bytes):
    img = np.asarray(Image.open(io.BytesIO(png_bytes)).convert("RGBA")).astype(np.float32)
    h, w = img.shape[:2]
    y0, x0 = int(h * LOGO[1]), int(w * LOGO[0])
    corner = img[y0:, x0:]
    white = (corner[..., 3] > 0) & (corner[..., :3].min(axis=2) >= 235)
    grown = white.copy()   # the logo's anti-aliased edge is a little darker than pure white
    grown[1:] |= white[:-1]; grown[:-1] |= white[1:]; grown[:, 1:] |= white[:, :-1]; grown[:, :-1] |= white[:, 1:]
    grown &= corner[..., :3].mean(axis=2) >= 120
    corner[grown] = 0
    a = img[..., 3]
    lum = img[..., 0] * 0.299 + img[..., 1] * 0.587 + img[..., 2] * 0.114
    opaque = lum[a > 127]
    med = max(float(np.median(opaque)), 8.0) if opaque.size else 128.0
    s = np.clip(lum / (2.0 * med), 0, 1) * 255.0
    out = np.empty((h, w, 4), np.uint8)
    out[..., 0] = out[..., 1] = out[..., 2] = s.astype(np.uint8)
    out[..., 3] = a.astype(np.uint8)
    buf = io.BytesIO(); Image.fromarray(out, "RGBA").save(buf, "PNG", optimize=True)
    return buf.getvalue(), med


def main():
    src, dst = sys.argv[1], sys.argv[2]
    blobs = bha.read_bundle(src)
    meta = json.loads(bytes(blobs["meta"]).decode())
    for st in meta["styles"]:
        name = st["name"] if isinstance(st, dict) else st
        png, med = shading(bytes(blobs[f"hair.{name}.texture"]))
        blobs[f"hair.{name}.texture"] = np.frombuffer(png, np.uint8)
        print(f"{name}: median luminance {med:.0f}, {len(png)//1024} KB")
    meta["textureKind"] = "shading"
    blobs["meta"] = np.frombuffer(json.dumps(meta).encode(), np.uint8)
    bha.write_bundle(dst, blobs)


if __name__ == "__main__":
    main()
