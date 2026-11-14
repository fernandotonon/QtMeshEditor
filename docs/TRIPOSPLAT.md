# TripoSplat — image → 3D Gaussians → game-ready mesh

`--backend triposplat` runs VAST-AI's TripoSplat (MIT) natively: one image in,
up to 262,144 3D Gaussians out, then QtMesh Editor turns them into a textured,
game-ready mesh. The raw Gaussians are kept too, as a standard 3DGS `.ply` —
that file is the representation TripoSplat's own demos render (SuperSplat,
SparkJS, and the splat plugins for Unity / Unreal / Godot load it).

```bash
qtmesh generate3d goblin.png --backend triposplat -o goblin.glb
#   -> goblin.glb            mesh: 150k tris, 2k diffuse + detail normal map
#   -> goblin_splats.ply     the 262,144 generated Gaussians
qtmesh generate3d goblin.png --backend triposplat --flow-steps 10 -o goblin.glb   # ~half the time
qtmesh generate3d goblin.png --backend triposplat --target-tris 50000 --gaussians 131072 -o g.glb
qtmesh generate3d goblin.png --backend triposplat --splat-out g.ply --no-splats   # (--no-splats: mesh only)
```

MCP: `generate_mesh_from_image {backend: "triposplat", gaussians, splat_output,
flow_steps, guidance, seed, target_tris, resolution, texture_size}`.
GUI: Inspector → Object mode → "AI: Image → 3D" → Backend "TripoSplat".
Models: AI Model Settings → "TripoSplat" (~6.1 GB, downloads on first use).

## Pipeline

1. **Matte.** With `--remove-bg` / the GUI checkbox (default on), the input is
   always matted (`BackgroundRemover`) — even if it already has some alpha
   (a soft edge is not a subject matte). Without the flag, inputs that have
   no real alpha are still matted. The model is trained on a subject
   composited over black. Then upstream's crop: erode alpha 1 px, square crop
   1.2× the alpha bounding box, 1024², composite on black
   (`TripoSplatPredictor::prepareImage`).
2. **Encode.** DINOv3 ViT-H (1024² → 4101×1280) and the FLUX.2 VAE encoder
   (→ 4101×128, with its sampling noise drawn by us).
3. **Flow matching.** 8192×16 latent + a 5-dim camera token, Euler steps with
   a shifted schedule (`t = 3l/(1+2l)`) and classifier-free guidance (zero
   features as the unconditional branch), one ONNX call per branch per step.
4. **Octree.** 8 levels of coarse-to-fine point placement; each level's logits
   split the remaining point budget over 8 children by systematic sampling
   (one uniform per cell), then a uniform jitter inside the leaf cell.
5. **Gaussians.** 32 Gaussians per point, activated in the graph (offsets,
   SH-DC colour, sigmoid opacity, softplus scale, rotation).
6. **Splats → mesh** (`SplatToMesh`): splat opacity into a density grid,
   threshold to a shell, morphological closing, exterior flood fill → solid,
   blur, marching cubes. Slab-streamed: 1024³ peaks at ~3.6 GB for the whole
   mesh + bake stage instead of ~12 GB for dense float grids.
7. **Game-ready + bake** (shared with TripoSR): simplify to the target, xatlas
   unwrap, colour from the nearest splats' SH-DC, detail normal map from the
   dense mesh.

## Measurements (Apple M-series, CPU, ORT 1.31.1)

| stage | time |
|---|---|
| DINOv3 | ~11 s |
| VAE encoder | ~6 s |
| flow, per call | **~38 s** (B=2 is no faster) |
| octree + Gaussian decoder | ~28 s |
| mesh (1024³) + simplify + 2k bake | ~77 s, 3.6 GB peak |

At the reference 20 steps with guidance that is 40 flow calls, ~25 minutes.
The flow call is ~25 TFLOP (12,294 tokens × 26 blocks); torch on CPU takes
31 s, torch on MPS fp16 3.6 s. **CoreML cannot compile the flow graph** (an MPS
matmul shape assertion that aborts the process), so macOS runs on the CPU EP;
the CUDA EP is used on Linux when available.

**Steps vs guidance** (6 seeds each, fraction of the mesh in its largest
connected part — 1.0 = one clean object):

| steps / guidance | per seed |
|---|---|
| 20 / 3 (default) | 1.0 ×6 |
| 10 / 3 | 1.0 ×6 |
| 6 / 3 | one seed 0.80 |
| 10 / 2 | 0.72 and 0.87 on two seeds |
| 10 / 1 (no CFG) | fails on some seeds (seed 1: fragments) |

So `--flow-steps 10` is a safe way to halve the time; guidance must stay > 1.

**Grid resolution.** The model's splats have a median nearest-neighbour
spacing of 0.00101 (unit box, 262k Gaussians); a 1024³ voxel is 0.00103, so
1024 is where the grid matches the data. Finer grids add triangles and per-splat
bumps, not shape. The dense 1024³ mesh (~1.7M tris) is used as the SOURCE of
the game-ready simplification + detail normal — unwrapping it directly takes
xatlas many minutes.

## Export notes (scripts/export-triposplat-onnx.py)

- `torch.polar` / `view_as_complex` in the RoPE are rewritten with real cos/sin
  (ONNX has no complex type).
- `F.layer_norm(x, x.shape[-1:])` traces the channel size as dynamic once any
  axis is dynamic; frozen to a constant.
- **All attention is exported as `com.microsoft.MultiHeadAttention`.** Plain
  SDPA lowers to MatMul-Softmax-MatMul, and ORT then materialises the whole
  16 × 12,294² score matrix (~9.7 GB per block) — it never finished and took a
  24 GB machine down. The fused CPU kernel runs the same attention in 1.5 s at
  0.4 GB.
- Graphs load and match torch to ~1e-4; dynamic shapes checked at runtime
  sizes. Export one graph per process (`--only`) on a 24 GB machine.

## Bugs found on the way (fixed for every backend)

- **xatlas ignores faces with 3D area ≤ FLT_EPSILON in absolute units** and
  parks them at UV (0,0). A ~1-unit generated mesh has many such faces (2054
  vertices on a 1024³ goblin); they had no texture and an arbitrary tangent
  frame. `MeshGenBaker` now unwraps a copy scaled to a 1000-unit diagonal.
- **Ogre's `buildTangentVectors` leaves zero tangents** on vertices whose
  triangles are all below float epsilon in UV area (459 vertices at 512³,
  2120 at 1024³) — black pixels under normal mapping. `MeshGenBuilder` now
  writes its own tangents (`MeshGenBaker::computeTangents`), and the
  detail-normal bake uses the same basis.
- **The #404 normal/roughness guess runs on the UV atlas**, and the atlas of a
  dense marching-cubes mesh is thousands of tiny charts: the model draws a
  ridge at every chart border, rendering as dark speckle along every seam.
  TripoSplat results opt out (`Result::synthesizePbrMaps = false`); their
  relief comes from the geometry and the real detail normal.

## Known issue

Re-importing a generated GLB into QtMesh Editor shows dark speckle on the
normal-mapped material even with a perfectly flat normal map; the same file
renders clean in Blender, and the mesh renders clean in QtMesh Editor straight
from generation or as `.mesh`. Ruled out: normals, tangent validity,
binormals, material, gamma, the UV (0,0) faces. Tracked separately.
