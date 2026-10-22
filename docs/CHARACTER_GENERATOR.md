# Character generator

`qtmesh character` / MCP `generate_character` / Inspector "Character Generator" build a rigged human from two open data sets:

* **MHR** (Momentum Human Rig, Apache-2.0): 18,439-vertex body, 45 identity shape modes, 127-joint skeleton + skin weights.
* **ICT-FaceKit** (MIT): head with 100 identity modes and 51 ARKit expression shapes; teeth, tongue, eyes and lashes are separate parts.

## Head
By default the character keeps the MHR body's **own head** (no seam, no cut). The 51 ARKit expression shapes are built offline
(`scripts/build-character-assets.py`) by transferring the ICT-FaceKit shapes onto the MHR head and expressing them with MHR's own
72 face-expression vectors (bounded ridge least squares); the jaw shapes are the real skinned motion of the `c_jaw` joint, the
eyelid shapes are transferred directly in a mask around each eye. The mouth interior (gums, tongue, teeth) and the eyeballs (sclera, iris) are ICT's. MHR's eyes are sculpted shut, so
with **Open eyes** on (the default; `--closed-eyes` / MCP `open_eyes:false` / the panel checkbox turn it off) the thin layer between the lids is removed
(the triangle list was picked by hand, `scripts/mhr_eye_layer_faces.json`) and the eyeballs show; eye colour only has an effect then.
The eyeballs and the mouth interior are seated relative to landmarks on the face (the eyelid layer and the lip line), so they follow every body/head shape.
**Skin texture.** The body gets a procedural albedo (on by default; `--no-skin-texture` / MCP `skin_texture:false` / the panel checkbox turn it off): tone patches, blush, freckles (more on fair skin) and pores around the skin colour, baked in the body's own UV layout (`SkinTexture.{h,cpp}`, 1024², PNG in `<AppData>/generated_textures/`, exported with the glTF). The noise is sampled at each texel's 3D surface position, so it is continuous across UV seams and has the same grain everywhere. Only for the native head (the ICT head has another UV layout).

**Textured eyes (opt-in, local).** Set `QTMESH_CHARACTER_EYE_OBJ` to an eyeball OBJ (a UV sphere whose iris pole is on -Y; it is rotated to face
forward, scaled to the seated ICT eyeball and used instead of the ICT sclera/iris) and `QTMESH_CHARACTER_EYE_TEX` to a directory holding
`iris_blue|brown|grey|green|hazel|dark.png` (`scripts/build-eye-iris-textures.py` builds them from an eye base-colour texture and a grid of iris
colours). The eye colour then picks the closest of the six textures. No eye asset ships with the editor; check the licence of the one you use.
`--ict-head` / MCP `head: "ict"` selects the older ICT head stitched on at the neck (native ARKit shapes, eyes, visible seam).

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

## Not yet
Hair, photo-driven creation (SAM body shape + face colour sampling), clothing.
