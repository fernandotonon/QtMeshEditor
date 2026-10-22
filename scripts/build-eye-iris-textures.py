#!/usr/bin/env python3
"""Make the six iris-colour eye textures the character generator can pick (blue brown grey green hazel dark).

Input: the eye's base-colour texture (square) and a 3x2 grid image of iris colours (row 1: blue brown grey, row 2: green hazel dark,
each tile with a caption). The grid tiles use a different layout from the eye's UVs, so only the iris disc is cut out of each tile,
scaled to the base texture's iris and feathered over it; the sclera and veins stay the base texture's.

    build-eye-iris-textures.py --base eye_basecolor.png --grid iris_grid.png --out dir/

Writes dir/iris_<name>.png. Point QTMESH_CHARACTER_EYE_TEX at that directory (with QTMESH_CHARACTER_EYE_OBJ at the eye mesh).
The generator does not ship an eye asset: check the licence of whatever you feed it.
"""
import argparse, os
import numpy as np
from PIL import Image

NAMES = [['blue', 'brown', 'grey'], ['green', 'hazel', 'dark']]
TILE_PUPIL_R, TILE_IRIS_R = 57.5, 158.0   # measured on the grid tiles (pixels)


def pupil(lum, lo, hi):
    sub = lum[lo[0]:hi[0], lo[1]:hi[1]] < 22
    yy, xx = np.nonzero(sub)
    return yy.mean() + lo[0], xx.mean() + lo[1], float(np.sqrt(sub.sum() / np.pi))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', required=True)
    ap.add_argument('--grid', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    base = np.array(Image.open(a.base).convert('RGB')).astype(np.float32)
    n = base.shape[0]
    cy, cx, pr = pupil(base.mean(2), (int(n * .29), int(n * .29)), (int(n * .7), int(n * .7)))
    grid = np.array(Image.open(a.grid).convert('RGB')).astype(np.float32)
    gh, gw = grid.shape[:2]
    th, tw = gh // 2, gw // 3
    scale = pr / TILE_PUPIL_R
    r_out = TILE_IRIS_R * scale
    yy, xx = np.mgrid[0:n, 0:n]
    d = np.hypot(xx - cx, yy - cy)
    alpha = np.clip((r_out * 1.16 - d) / (r_out * 0.12), 0, 1)[..., None]
    os.makedirs(a.out, exist_ok=True)
    for r in range(2):
        for c in range(3):
            tile = grid[r * th + 2:(r + 1) * th - 1, c * tw + 2:(c + 1) * tw - 1]
            h, w = tile.shape[:2]
            tcy, tcx, _ = pupil(tile.mean(2), (int(h * .3), int(w * .25)), (int(h * .7), int(w * .75)))
            sx = np.clip(tcx + (xx - cx) / scale, 0, w - 1)
            sy = np.clip(tcy + (yy - cy) / scale, 0, h - 1)
            x0, y0 = np.floor(sx).astype(int), np.floor(sy).astype(int)
            x1, y1 = np.minimum(x0 + 1, w - 1), np.minimum(y0 + 1, h - 1)
            fx, fy = (sx - x0)[..., None], (sy - y0)[..., None]
            s = tile[y0, x0] * (1 - fx) * (1 - fy) + tile[y0, x1] * fx * (1 - fy) + tile[y1, x0] * (1 - fx) * fy + tile[y1, x1] * fx * fy
            out = base * (1 - alpha) + s * alpha
            Image.fromarray(out.clip(0, 255).astype(np.uint8)).save(os.path.join(a.out, 'iris_%s.png' % NAMES[r][c]))


if __name__ == '__main__':
    main()
