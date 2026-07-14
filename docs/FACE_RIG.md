# Face auto-rig: ARKit blendshapes on any humanoid face

Epic [#889](https://github.com/fernandotonon/QtMeshEditor/issues/889). Given an
unrigged neutral **face** mesh, QtMeshEditor generates the 52 **ARKit**
blendshapes (`jawOpen`, `mouthSmileLeft`, `eyeBlinkLeft`, `browInnerUp`, …) and
attaches them as morph targets, so the mesh can be driven by face performance
capture ([#869](https://github.com/fernandotonon/QtMeshEditor/issues/869),
`qtmesh mocap --face`) — no manual sculpting of blend shapes.

It is **deterministic geometry**, not an ML model: a non-rigid fit of a
permissively-licensed template face onto yours, followed by deformation
transfer of each expression. No ONNX, no GPU, no network beyond a one-time
template download.

## Using it

### GUI
Select a face mesh → Inspector → **Vertex Morph Animation** (Edit Mode) →
**"✨ Add ARKit Blendshapes (AI)"**. The fit runs on a worker thread (the button
shows *Downloading… / Fitting…*); when it finishes the 52 shapes appear in the
Shapes list and the whole batch is a single undo step. The
[#869](https://github.com/fernandotonon/QtMeshEditor/issues/869) Performance
Capture panel then drives them.

### CLI
```bash
qtmesh facerig neutral_head.glb -o rigged.glb
qtmesh facerig head.fbx -o rigged.glb --max-shapes 20     # cap the shape count
qtmesh facerig head.glb  -o rigged.glb --max-residual 5   # stricter humanoid gate
qtmesh facerig head.glb  -o rigged.glb --json             # machine-readable report
```

### MCP
`add_arkit_blendshapes` — `{max_shapes?, max_residual_pct?, output_path?}`,
operates on the selected entity; re-exports when `output_path` is given.

## How it works

```
ArkitTemplate  (ICT-FaceKit neutral + 52 expression deltas, one topology)
     │
     ▼  NonRigidICP (Amberg 2007 optimal-step)   src/FaceRig/NonRigidICP
  correspondence X  — template verts fitted onto the USER surface
     │
     ▼  DeformationTransfer (Sumner & Popović 2004)   src/FaceRig/DeformationTransfer
  per-template-vertex delta per shape, on the user identity
     │
     ▼  resample template topology → the real user vertices   src/FaceRig/FaceRigger
  52 × per-user-vertex deltas
     │
     ▼  Ogre::Pose + VAT_POSE morph targets, named per FaceCap::kBlendshapeNames
```

- **`FaceRigger` / `FaceRigAttach`** (`src/FaceRig/`) orchestrate the pipeline;
  the pure-data core is Ogre-free and headless-unit-tested.
- The linear solves use a self-contained sparse CG (`SparseSolve`) — **no Eigen,
  no external solver, zero new dependencies** (the house rule for these
  features, same as skinning #402 and auto-rig #407).
- The template (`facerig/arkit_template.bin`, ICT-FaceKit MIT — see
  `THIRD_PARTY_AI_MODELS.md`) downloads on first use to
  `<AppData>/ai_models/facerig/`. Overrides: `QTMESH_FACERIG_MODEL_BASE_URL` /
  `QSettings ai/facerigModelBaseUrl`; offline guard `QTMESH_FACERIG_NO_DOWNLOAD`.

## Quality & limits

- **Humanoid faces only.** The fit residual is a gate: a non-face mesh fits
  poorly and is **rejected** (`--max-residual`, default 8% of the mesh
  diagonal), rather than emitting garbage shapes. This mirrors the
  AutoRig/Pinocchio precedent.
- **Measured** (decimated, different-topology ICT head, 15 755 verts): NRICP
  fit **mean 0.008% / max 0.61%** of the diagonal; **51 shapes** attached;
  jawOpen drops the lower face while the forehead stays still; mouthSmile /
  eyeBlink / browInnerUp localise to their regions.
- **Orientation:** the mesh should be roughly upright, +Y up, facing the
  template's orientation. A wildly rotated head may fit poorly.
- **glTF export** carries the morph-target geometry on the primitive; per-target
  *names* in glTF `extras.targetNames` are a follow-up (the in-editor targets
  and the mocap hand-off use the correct names regardless).

## Related

- `THIRD_PARTY_AI_MODELS.md` — ICT-FaceKit licensing verdict.
- `docs/FACE_RIG_SPIKE.md` — the offline feasibility spike + the C/D contract.
- Epic [#889](https://github.com/fernandotonon/QtMeshEditor/issues/889);
  slices B–G (#891–#895 + the polish slice).
