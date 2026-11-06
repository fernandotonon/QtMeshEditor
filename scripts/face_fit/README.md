# MHR face-landmark regression (offline, dev only)

Builds `resources/face_landmark_fit.bin`, the linear model landmarks(c) = L0 + A c used by `qtmesh character --photo --fit-face`.

1. `gen_samples.py` — 160 random identities (45 MHR coefficients, N(0,1) clipped at ±2.5) as bust glbs via `qtmesh character`.
2. `render_batch.py` (`blender -b -P render_batch.py -- list.txt`) — frontal close-up renders (Workbench).
3. `build_model.py` — MediaPipe landmarks (`lmk.py` mirrors `FaceLandmarkDetector`: 256 px input, two passes) on every render; `regress.py` — ridge regression
   of centred, RMS-scaled landmarks on the coefficients (held-out error 0.0054 of the inter-ocular distance vs 0.0169 for the mean face), writes `lmk_model.npz`;
   export `W` (46 x 1404, float32) as `MFL1` + int32 modes + int32 points + data.
4. `evaluate.py` / `evaluate2.py` / `rerender_check.py` — landmark error of a photo vs its character, with SAM's coefficients vs the fitted ones
   (re-rendered and re-detected). 16 CC0 portraits: mean-face 0.0426, SAM 0.0431, fitted 0.0377, better on 16/16; two different people differ by 0.054.

Scripts expect to run from a scratch folder holding `photos` landmark dumps (`QTMESH_PHOTO_DEBUG_DIR` of `qtmesh character --photo`). Rebuild the model whenever the MHR bundle changes.
