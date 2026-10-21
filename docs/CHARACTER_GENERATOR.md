# Character generator

`qtmesh character` / MCP `generate_character` / Inspector "Character Generator" build a rigged human from two open data sets:

* **MHR** (Momentum Human Rig, Apache-2.0): 18,439-vertex body, 45 identity shape modes, 127-joint skeleton + skin weights.
* **ICT-FaceKit** (MIT): head with 100 identity modes and 51 ARKit expression shapes; teeth, tongue, eyes and lashes are separate parts.

## Output
Body and head are separate submeshes (`body`, `head_skin`, `head_mouth`, `head_teeth`, `head_sclera`, `head_iris`, `head_lashes`) so
materials and hair attach per part. The skeleton is the standard 52-joint Mixamo-style set (names from `SamBody::mhrAlias`), so
text-to-motion, retargeting and mocap work unchanged. Every part carries all 51 ARKit morph targets (zero-delta dummies where a part
has no deformation) so glTF export keeps the target names.

## Pipeline notes
* The head is scaled/anchored to the body by chin + head-top; a triangle strip stitches the neck (the body loses its head-dominated and neck-column vertices above the cut).
* The sclera front cap is removed so the iris colour shows.
* Undriven MHR twist/helper joints fold into the nearest kept ancestor; bones are world-aligned (identity orientations) — without that, limbs collapse under animation.
* Assets: `QTMESH_CHARACTER_DIR` or `<AppData>/ai_models/character` (`mhr_character.qmcb`, `ict_character.qmcb`, licence texts), downloaded on first use with pinned SHA-256s. Build them with `scripts/build-character-assets.py`; host with `scripts/upload-character-assets.sh`.

## Your own MetaHuman DNA (optional)
MetaHuman DNA files cannot be redistributed, so none ships with the editor. If you have a MetaHuman **body** `.dna` (you are responsible
for its licence), `qtmesh character --dna body.dna [--dna-mode joints+surface|surface|joints] -o out.glb`, MCP `generate_character {dna_path}` or the
panel's DNA field fit that body onto the generated shape: you get the DNA's own skeleton, skin weights and body correctives with our
proportions. The generated body (no head) is the target; the fit is the same one as `qtmesh dna fit` / the Fit DNA Body panel. A body DNA
has no head: attach one separately (e.g. `qtmesh dna face` on a face DNA).

## Not yet
Hair, photo-driven creation (SAM body shape + face colour sampling), clothing.
