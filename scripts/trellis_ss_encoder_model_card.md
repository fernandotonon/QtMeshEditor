---
license: mit
library_name: onnx
pipeline_tag: image-to-3d
tags:
  - onnx
  - image-to-3d
  - trellis
  - trellis2
  - spacecontrol
  - shape-guide
  - qtmesheditor
base_model:
  - microsoft/TRELLIS-image-large
---

# TRELLIS sparse-structure encoder: ONNX export (shape guide)

**ONNX export of the sparse-structure VAE encoder `ss_enc_conv3d_16l8_fp16`
from [microsoft/TRELLIS-image-large](https://huggingface.co/microsoft/TRELLIS-image-large)**
(MIT code and weights). All credit for the original weights goes to Microsoft.

TRELLIS.2 reuses TRELLIS-1's sparse-structure VAE, and the native
[trellis.cpp](https://github.com/fernandotonon/trellis.cpp) runtime ships only its
decoder. **[QtMeshEditor](https://github.com/fernandotonon/QtMeshEditor)** runs
this encoder to turn a rough blockout (a house, a vehicle, a body) into a
sparse-structure latent, and starts TRELLIS.2's sparse-structure flow from it
(training-free blockout control in the style of SpaceControl). The result keeps
the image's look while following the guide's size, placement and volume:
`qtmesh generate3d image.png --backend trellis2 --guide house --guide-strength 0.5`,
the "Shape guide" row of the GUI, or the MCP `guide` argument.

> The file QtMeshEditor downloads at runtime lives in the shared
> [`fernandotonon/QtMeshEditor-models`](https://huggingface.co/fernandotonon/QtMeshEditor-models)
> repo under `trellis2/`. This repo is the standalone model card + mirror.

## Files

| file | role |
|---|---|
| `ss_encoder.onnx` | the encoder, fp32, opset 17, ~237 MB |
| `LICENSE-TRELLIS.txt` | MIT (Microsoft TRELLIS) |

SHA-256 of `ss_encoder.onnx`:
`904d66260f36d4922b891d366abddfaa48f76adafbda80126137119f6e77e8c0`
(pinned in QtMeshEditor's `src/ImageTo3D/ShapeGuide.cpp`; the app verifies it).

## I/O contract

| name | type | shape | meaning |
|---|---|---|---|
| input `occupancy` | float32 | `[1,1,64,64,64]` | surface voxels of the guide, 0 or 1, index `(x,y,z)` of the TRELLIS grid (Z up) |
| output `latent` | float32 | `[1,8,16,16,16]` | posterior **mean** (no sampling) |

The guide mesh is centred and scaled so its largest side spans 0.95 of the
`[-0.5, 0.5]` cube before voxelization. In TRELLIS.2's sampler the latent is
laid out `[4096][8]` (cell `x*256 + y*16 + z`, channel fastest) and mixed with
noise as `t0 * noise + (1 - t0) * latent`, where a higher guide strength gives
a lower `t0`.

## Export

`scripts/export-trellis-ss-encoder-onnx.py` in the QtMeshEditor repo (offline
developer tool): loads the TRELLIS checkpoint, exports with
`torch.onnx.export`, and refuses to write a graph whose output differs from
torch by more than 2e-5 on a test blockout.

## Licence

MIT, as the original TRELLIS code and weights (`LICENSE-TRELLIS.txt`).
