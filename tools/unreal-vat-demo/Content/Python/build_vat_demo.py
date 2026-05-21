# Builds the OpenVAT demo assets from the bake files in Content/Rumba/.
#
# Why a Python script instead of pre-built .uasset files:
#   Unreal's .uasset is a proprietary binary that re-cooks per engine
#   version and can't be hand-written or committed reliably across
#   UE 5.3 / 5.4 / 5.5. The bake itself (PNG, sidecar, glTF, USF
#   shader) is all text/standard files — we ship those, and run
#   this script once to generate the engine-specific glue (Material,
#   Texture import settings, demo Blueprint).
#
# Run from inside the Unreal editor:
#   Window → Output Log → Python tab → execute file:
#     Content/Python/build_vat_demo.py
#   or:   py Content/Python/build_vat_demo.py
#
# Idempotent — re-running rebuilds the Material in place.
#
# How this works (and why it's simpler than it used to be):
#   The bake's source.gltf is produced by `qtmesh vat --emit-uv2`,
#   which writes the per-vertex bake-column index as a TEXCOORD_1
#   attribute directly into the glTF. Unreal's mesh importer
#   reorders vertices for cache locality, but a vertex attribute
#   travels WITH its vertex through any reorder — so TEXCOORD_1
#   still points at the right column in the imported mesh. No
#   runtime UV2-baking, no bind-sidecar matcher, no engine-version-
#   specific Geometry Script paths. The shader just reads
#   TexCoord[1] and indexes the position texture by that.

import json
import os
import unreal


BAKE_DIR = "/Game/Rumba"
DEMO_DIR = "/Game/VATDemo"
RUMBA_FS_DIR = os.path.join(os.path.dirname(__file__), "..", "Rumba")


# ────────────────────────────────────────────────────────────────────
# 1. Import the bake: position PNG, diffuse PNG, glTF mesh.
# ────────────────────────────────────────────────────────────────────

def _import_texture(src_filename, dst_name):
    """Import a PNG via Unreal's stock TextureFactory.

    Returns the resulting Texture2D asset, or None if the import failed.
    """
    src_path = os.path.abspath(os.path.join(RUMBA_FS_DIR, src_filename))
    if not os.path.exists(src_path):
        unreal.log_warning("Skipping missing texture: " + src_path)
        return None
    task = unreal.AssetImportTask()
    task.set_editor_property("automated", True)
    task.set_editor_property("destination_path", BAKE_DIR)
    task.set_editor_property("destination_name", dst_name)
    task.set_editor_property("replace_existing", True)
    task.set_editor_property("save", True)
    task.set_editor_property("filename", src_path)
    task.set_editor_property("factory", unreal.TextureFactory())
    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([task])
    return unreal.load_asset(BAKE_DIR + "/" + dst_name + "." + dst_name)


def _import_gltf_via_interchange(src_filename, dst_name):
    """Import a glTF via Unreal's Interchange framework (built into
    UE 5.0+; no separate plugin required).

    Returns the imported skeletal mesh, or None on failure. The
    Interchange API moved between 5.0 and 5.3 — we use the high-level
    `unreal.InterchangeManager` entry point and fall back to logging
    actionable instructions if the user is on an older engine.
    """
    src_path = os.path.abspath(os.path.join(RUMBA_FS_DIR, src_filename))
    if not os.path.exists(src_path):
        unreal.log_warning("Skipping missing mesh: " + src_path)
        return None

    if not hasattr(unreal, "InterchangeManager"):
        unreal.log_warning(
            "InterchangeManager not available — please drag "
            + src_path + " into the Content Browser at " + BAKE_DIR
            + " manually (and rename the result to " + dst_name + ").")
        return None

    src_data = unreal.InterchangeManager.create_source_data(src_path)
    params = unreal.ImportAssetParameters()
    params.is_automated = True
    params.destination_name = dst_name

    mgr = unreal.InterchangeManager.get_interchange_manager_scripted()
    try:
        mgr.import_asset(BAKE_DIR, src_data, params)
    except Exception as e:
        unreal.log_warning("Interchange import failed: " + str(e)
            + " — please drag the .gltf in manually.")
        return None

    return unreal.load_asset(BAKE_DIR + "/" + dst_name + "." + dst_name)


def import_bake_assets():
    """Bring textures + glTF in under /Game/Rumba.

    The bake's POSITION texture needs very specific import settings —
    Unreal's default Texture2D import gamma-corrects + DXT-compresses
    the data, which corrupts the per-vertex floats. We re-open after
    import and override.
    """
    _import_texture("mixamo.com_pos.png", "T_OpenVAT_Pos")
    _import_texture("Boss_diffuse.png",   "T_Boss_Diffuse")
    _import_gltf_via_interchange("source.gltf", "SK_Rumba")

    # Position texture override: lossless, no sRGB, no mips, nearest.
    pos_tex = unreal.load_asset(BAKE_DIR + "/T_OpenVAT_Pos.T_OpenVAT_Pos")
    if pos_tex:
        pos_tex.set_editor_property("srgb", False)
        pos_tex.set_editor_property("compression_settings",
            unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
        pos_tex.set_editor_property("mip_gen_settings",
            unreal.TextureMipGenSettings.TMGS_NO_MIPMAPS)
        pos_tex.set_editor_property("filter",
            unreal.TextureFilter.TF_NEAREST)
        unreal.EditorAssetLibrary.save_loaded_asset(pos_tex)
        unreal.log("Position texture configured: non-sRGB, lossless, no mips, nearest")
    else:
        unreal.log_error("Could not load imported T_OpenVAT_Pos — bake will look wrong.")


# ────────────────────────────────────────────────────────────────────
# 2. Read the OpenVAT sidecar.
# ────────────────────────────────────────────────────────────────────

def read_sidecar():
    """Return (frame_count, bounds_min, bounds_max) from -remap_info.json."""
    path = os.path.abspath(os.path.join(
        RUMBA_FS_DIR, "mixamo.com-remap_info.json"))
    with open(path, "r") as f:
        data = json.load(f)
    os_remap = data.get("os-remap") or data
    frames = int(os_remap["Frames"])
    mn = [float(x) for x in os_remap["Min"]]
    mx = [float(x) for x in os_remap["Max"]]
    return frames, mn, mx


# ────────────────────────────────────────────────────────────────────
# 3. Verify source.gltf carries TEXCOORD_1 (the bake-column index).
#
# Without it the dancer renders as scattered triangles because the
# material's `TexCoord[1]` would read whatever (or nothing) the
# importer put there. `qtmesh vat --emit-uv2` is what writes this
# channel; we sanity-check at bootstrap time so a stale bake (made
# before the --emit-uv2 era) fails loudly instead of silently.
# ────────────────────────────────────────────────────────────────────

def verify_gltf_has_uv2():
    """Inspect source.gltf to confirm every primitive carries
    TEXCOORD_1. Returns True on success, False otherwise."""
    path = os.path.abspath(os.path.join(RUMBA_FS_DIR, "source.gltf"))
    try:
        with open(path, "r") as f:
            d = json.load(f)
    except Exception as e:
        unreal.log_error("Could not read source.gltf: " + str(e))
        return False
    meshes = d.get("meshes", [])
    if not meshes:
        unreal.log_error("source.gltf has no meshes.")
        return False
    missing = []
    for mi, mesh in enumerate(meshes):
        for pi, prim in enumerate(mesh.get("primitives", [])):
            if "TEXCOORD_1" not in prim.get("attributes", {}):
                missing.append("mesh[%d].primitives[%d]" % (mi, pi))
    if missing:
        unreal.log_error(
            "source.gltf is MISSING TEXCOORD_1 on: " + ", ".join(missing)
            + " — re-bake with `qtmesh vat --emit-uv2` or the dancer "
              "will render as scattered triangles.")
        return False
    unreal.log("source.gltf carries TEXCOORD_1 on every primitive ✓")
    return True


# ────────────────────────────────────────────────────────────────────
# 4. Build the Material. Custom node carries openvat.usf body inline.
# ────────────────────────────────────────────────────────────────────

def read_usf_body():
    """The Custom node treats its `Code` property as the body of an
    HLSL function — no `void main`, no #version, no struct boilerplate.
    Inline body kept short enough that users don't need to chase the
    full openvat.usf file for the math."""
    return r"""
// OpenVAT Custom-node body. Inputs (wire from the material graph):
//   pos_tex        Texture2D  — the imported T_OpenVAT_Pos
//   uv2            float2     — (col, row_block) per vertex, from
//                                the mesh's TEXCOORD_1 (written by
//                                `qtmesh vat --emit-uv2`)
//   current_frame  float      — scalar parameter, animated 0..frame_count
//   frame_count    float      — scalar parameter (from sidecar)
//   bounds_min     float3
//   bounds_max     float3
// Output: float3 — the displaced model-space vertex position.

int col = (int)uv2.x;
int row_block = (int)uv2.y;
int N = max((int)frame_count, 1);

int curr = (int)floor(current_frame);
curr = ((curr % N) + N) % N;
int nxt = (curr + 1) % N;
float blend = frac(current_frame);

int base_row = row_block * N;
float3 p_curr = pos_tex.Load(int3(col, base_row + curr, 0)).rgb;
float3 p_next = pos_tex.Load(int3(col, base_row + nxt,  0)).rgb;
float3 p = lerp(p_curr, p_next, blend);
return bounds_min + p * (bounds_max - bounds_min);
"""


def build_material(frame_count, bounds_min, bounds_max):
    """Create or rebuild M_OpenVAT.

    Idempotent: if the asset already exists (rerun of the bootstrap
    after changing bake inputs), delete-and-recreate so the graph
    reflects the new sidecar bounds + frame count. `create_asset` on
    a pre-existing path returns None — we'd otherwise abort silently
    and the user would be left with a stale material.
    """
    unreal.EditorAssetLibrary.make_directory(DEMO_DIR)
    mat_path = DEMO_DIR + "/M_OpenVAT"
    if unreal.EditorAssetLibrary.does_asset_exist(mat_path):
        unreal.log("M_OpenVAT already exists — deleting and recreating "
                   "so the graph picks up the latest sidecar bounds.")
        unreal.EditorAssetLibrary.delete_asset(mat_path)
    mat_factory = unreal.MaterialFactoryNew()
    mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "M_OpenVAT", DEMO_DIR, unreal.Material, mat_factory)
    if mat is None:
        unreal.log_error("Failed to create M_OpenVAT")
        return None

    me = unreal.MaterialEditingLibrary

    # Scalar/vector parameters bound by the actor at runtime.
    p_curr = me.create_material_expression(mat,
        unreal.MaterialExpressionScalarParameter, -800, -200)
    p_curr.set_editor_property("parameter_name", "current_frame")
    p_curr.set_editor_property("default_value", 0.0)

    p_frames = me.create_material_expression(mat,
        unreal.MaterialExpressionScalarParameter, -800, -100)
    p_frames.set_editor_property("parameter_name", "frame_count")
    p_frames.set_editor_property("default_value", float(frame_count))

    p_lo = me.create_material_expression(mat,
        unreal.MaterialExpressionVectorParameter, -800, 0)
    p_lo.set_editor_property("parameter_name", "bounds_min")
    p_lo.set_editor_property("default_value", unreal.LinearColor(*bounds_min, 0))

    p_hi = me.create_material_expression(mat,
        unreal.MaterialExpressionVectorParameter, -800, 100)
    p_hi.set_editor_property("parameter_name", "bounds_max")
    p_hi.set_editor_property("default_value", unreal.LinearColor(*bounds_max, 0))

    # Texture parameter so the actor can hot-swap in another bake.
    p_tex = me.create_material_expression(mat,
        unreal.MaterialExpressionTextureObjectParameter, -800, 200)
    p_tex.set_editor_property("parameter_name", "pos_tex")
    pos_tex_asset = unreal.load_asset(BAKE_DIR + "/T_OpenVAT_Pos")
    if pos_tex_asset:
        p_tex.set_editor_property("texture", pos_tex_asset)

    # Per-vertex UV2 lookup. TexCoord index 1 maps directly to the
    # glTF's TEXCOORD_1 — the (column, row_block) pair `qtmesh vat
    # --emit-uv2` wrote into source.gltf. Unreal's importer preserves
    # vertex attributes across its vertex-buffer reorder, so this
    # always lines up with the bake.
    p_uv2 = me.create_material_expression(mat,
        unreal.MaterialExpressionTextureCoordinate, -800, 300)
    p_uv2.set_editor_property("coordinate_index", 1)

    # Custom node carrying the openvat math.
    custom = me.create_material_expression(mat,
        unreal.MaterialExpressionCustom, -400, 0)
    custom.set_editor_property("code", read_usf_body())
    custom.set_editor_property("output_type",
        unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("description", "OpenVAT_Vertex")

    custom_inputs = []
    for name in ("pos_tex", "uv2", "current_frame", "frame_count",
                 "bounds_min", "bounds_max"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", name)
        custom_inputs.append(ci)
    custom.set_editor_property("inputs", custom_inputs)

    me.connect_material_expressions(p_tex,    "", custom, "pos_tex")
    me.connect_material_expressions(p_uv2,    "", custom, "uv2")
    me.connect_material_expressions(p_curr,   "", custom, "current_frame")
    me.connect_material_expressions(p_frames, "", custom, "frame_count")
    me.connect_material_expressions(p_lo,     "", custom, "bounds_min")
    me.connect_material_expressions(p_hi,     "", custom, "bounds_max")

    # The Custom node returns absolute model-space position and we
    # wire it straight to World Position Offset. Assumes the bind
    # pose sits at the actor origin — true for Mixamo characters
    # (the demo asset); other rigs would need a
    # `WPO = custom_output - bind_position` subtraction.
    me.connect_material_property(custom, "",
        unreal.MaterialProperty.MP_WORLD_POSITION_OFFSET)

    # Albedo: sample T_Boss_Diffuse against UV0 (TexCoord 0).
    p_uv0 = me.create_material_expression(mat,
        unreal.MaterialExpressionTextureCoordinate, -800, 600)
    p_uv0.set_editor_property("coordinate_index", 0)

    p_diff = me.create_material_expression(mat,
        unreal.MaterialExpressionTextureSample, -400, 600)
    diff_tex = unreal.load_asset(BAKE_DIR + "/T_Boss_Diffuse")
    if diff_tex:
        p_diff.set_editor_property("texture", diff_tex)
    me.connect_material_expressions(p_uv0, "", p_diff, "UVs")
    me.connect_material_property(p_diff, "RGB",
        unreal.MaterialProperty.MP_BASE_COLOR)

    me.recompile_material(mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    unreal.log("Built M_OpenVAT (Custom-node WPO + diffuse).")
    return mat


# ────────────────────────────────────────────────────────────────────
# 5. Build a sample Blueprint that drives current_frame over time.
# ────────────────────────────────────────────────────────────────────

def build_actor_blueprint(frame_count):
    """Create BP_VATDancer skeleton.

    Unreal's Blueprint creation from Python is supported via
    KismetEditorUtilities + EditorAssetLibrary, but graph editing
    (event nodes + math chain) requires UnrealEd's BP graph API
    which isn't fully exposed to Python. Easiest portable path:
    create the Blueprint asset and let the user double-click to
    add the 4-node tick logic per the README. This is what most
    production UE pipelines do.
    """
    bp_path = DEMO_DIR + "/BP_VATDancer"
    if unreal.EditorAssetLibrary.does_asset_exist(bp_path):
        unreal.log("BP_VATDancer already exists — skipping.")
        return

    factory = unreal.BlueprintFactory()
    factory.set_editor_property("parent_class", unreal.Actor)
    bp = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
        "BP_VATDancer", DEMO_DIR, None, factory)
    if bp:
        unreal.EditorAssetLibrary.save_loaded_asset(bp)
        unreal.log("Created BP_VATDancer skeleton — see README for the "
                   "4-node tick wiring.")


# ────────────────────────────────────────────────────────────────────
# Entry point
# ────────────────────────────────────────────────────────────────────

def main():
    unreal.log("=== QtMeshEditor OpenVAT demo bootstrap ===")
    import_bake_assets()
    if not verify_gltf_has_uv2():
        unreal.log_warning(
            "=== Bootstrap STOPPED: source.gltf is missing TEXCOORD_1. "
            "Re-bake with `qtmesh vat <file> --anim <name> --emit-uv2 "
            "-o <dir>` and overwrite the files under Content/Rumba/. ===")
        return
    frames, mn, mx = read_sidecar()
    unreal.log("Sidecar: frames=%d, min=%s, max=%s" % (frames, mn, mx))
    build_material(frames, mn, mx)
    build_actor_blueprint(frames)
    unreal.log("=== Bootstrap done. Open /Game/VATDemo/M_OpenVAT + "
               "BP_VATDancer; finish wiring per README. ===")


if __name__ == "__main__":
    main()
