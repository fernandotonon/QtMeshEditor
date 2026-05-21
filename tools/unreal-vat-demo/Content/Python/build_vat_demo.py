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

# Bumped any time build_material's graph layout changes so the
# auto-runner (init_unreal.py) knows to rebuild a stale M_OpenVAT.
# The build number is stamped into the material's asset-tag metadata
# under `OpenVATBuild` when the material is created; init_unreal
# compares the tag against this constant and forces a rebuild on
# mismatch.
OPENVAT_BUILD = 3


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


def find_skeletal_mesh():
    """Locate the imported Rumba skeletal mesh.

    Interchange's `destination_name` parameter is advisory — when
    importing a glTF, the framework writes a subtree under
    `/Game/Rumba/source/...` (mesh, skeleton, animation, materials)
    and does NOT honour our requested `SK_Rumba` short name. So the
    asset can land at `/Game/Rumba/SK_Rumba` (older UE / Interchange
    builds), `/Game/Rumba/source/SkeletalMeshes/SK_Rumba`
    (5.5+ Interchange default), or under a `Rumba_Dancing_mesh`
    name if Interchange used the glTF's node name.

    We try every known path, then fall back to an AssetRegistry
    search under `/Game/Rumba/` for the first SkeletalMesh asset.
    """
    candidates = [
        "/Game/Rumba/SK_Rumba",
        "/Game/Rumba/source/SkeletalMeshes/SK_Rumba",
        "/Game/Rumba/Rumba_Dancing_mesh",
        "/Game/Rumba/source/SkeletalMeshes/Rumba_Dancing_mesh",
    ]
    for path in candidates:
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            asset = unreal.load_asset(path)
            if asset is not None:
                unreal.log("find_skeletal_mesh: located at " + path)
                return asset

    # Last resort — AssetRegistry sweep under /Game/Rumba/.
    try:
        ar = unreal.AssetRegistryHelpers.get_asset_registry()
        # Recursive get_assets_by_path returns every asset in the
        # subtree (including textures); filter to SkeletalMesh.
        assets = ar.get_assets_by_path("/Game/Rumba", recursive=True)
        for ad in assets:
            try:
                cls = str(ad.get_class().get_name())
            except Exception:
                cls = ""
            if cls == "SkeletalMesh":
                obj = ad.get_asset()
                if obj:
                    pkg = str(ad.package_name) if hasattr(ad, "package_name") else "?"
                    unreal.log("find_skeletal_mesh: AssetRegistry "
                               "fallback found " + pkg)
                    return obj
    except Exception as e:
        unreal.log_warning("AssetRegistry sweep failed: " + str(e))
    return None


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
    full openvat.usf file for the math.

    `current_frame` is now driven inside the material via the built-in
    Time node (Time × fps), not a scalar parameter the actor has to
    poke each Tick. Means a static actor with this material renders
    the animation forever, no Blueprint logic required."""
    return r"""
// OpenVAT Custom-node body — emits a WPO offset for Unreal.
//
// Inputs (wire from the material graph):
//   pos_tex        Texture2D  — the imported T_OpenVAT_Pos
//   uv2            float2     — TEXCOORD_1 = (col, row_block), per-
//                                vertex (written by `qtmesh vat --emit-uv2`)
//   current_frame  float      — derived from material Time × fps
//   frame_count    float      — scalar param (from sidecar)
//   bind_local     float3     — `PreSkinnedPosition`: bind-pose
//                                vertex in object space, Z-up,
//                                centimeters (Interchange-imported)
//   bounds_min     float3     — Y-up, meters (raw bake)
//   bounds_max     float3     — Y-up, meters (raw bake)
//
// Output: float3 — World-space OFFSET (delta) for WPO, in cm.
//
// Coordinate-system gymnastics: the bake's texels + bounds live in
// glTF native space (Y-up RH, meters). Unreal's Interchange importer
// has already swizzled the imported mesh to Z-up LH cm. To get a
// usable WPO we have to take the bake's target position into the
// same frame the bind sits in, then subtract.
//
//   target_yup_m  = bounds_min + p * (bounds_max - bounds_min)
//   target_zup_cm = (target_yup_m.x, -target_yup_m.z, target_yup_m.y) * 100
//   bind_zup_cm   = bind_local                                // already in this frame
//   wpo           = target_zup_cm - bind_zup_cm
//
// The swizzle `(x, -z, y)` is the standard glTF Y-up RH → UE Z-up LH
// conversion. Combined with ×100 for the unit change it produces a
// delta in Unreal centimeters, which WPO expects.

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

float3 target_yup_m  = bounds_min + p * (bounds_max - bounds_min);
float3 target_zup_cm = float3(target_yup_m.x,
                              -target_yup_m.z,
                               target_yup_m.y) * 100.0;
return target_zup_cm - bind_local;
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

    # current_frame = Time × fps (auto-loops in the material).
    # No scalar parameter / no Tick / no Blueprint needed — a static
    # SkeletalMeshActor in the level with this material applied
    # animates forever.
    p_time = me.create_material_expression(mat,
        unreal.MaterialExpressionTime, -1000, -200)

    p_fps = me.create_material_expression(mat,
        unreal.MaterialExpressionScalarParameter, -1000, -100)
    p_fps.set_editor_property("parameter_name", "fps")
    p_fps.set_editor_property("default_value", 30.0)

    p_curr = me.create_material_expression(mat,
        unreal.MaterialExpressionMultiply, -800, -200)
    me.connect_material_expressions(p_time, "", p_curr, "A")
    me.connect_material_expressions(p_fps,  "", p_curr, "B")

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

    # Pre-skinned local position — the vertex's bind-pose coordinate
    # in object space, before WPO and before skeletal-mesh skinning
    # have been applied. We subtract this inside the Custom node so
    # WPO carries the per-frame OFFSET (target − bind) instead of an
    # absolute target. Without this the dancer collapses to ~1 cm
    # because the bake's positions are in meters and Unreal's WPO
    # is interpreted in centimeters.
    #
    # `MaterialExpressionPreSkinnedPosition` is exactly what we need
    # and exists on every UE 5.x.
    p_bind = me.create_material_expression(mat,
        unreal.MaterialExpressionPreSkinnedPosition, -800, 400)

    # Custom node carrying the openvat math.
    custom = me.create_material_expression(mat,
        unreal.MaterialExpressionCustom, -400, 0)
    custom.set_editor_property("code", read_usf_body())
    custom.set_editor_property("output_type",
        unreal.CustomMaterialOutputType.CMOT_FLOAT3)
    custom.set_editor_property("description", "OpenVAT_Vertex")

    custom_inputs = []
    for name in ("pos_tex", "uv2", "current_frame", "frame_count",
                 "bind_local", "bounds_min", "bounds_max"):
        ci = unreal.CustomInput()
        ci.set_editor_property("input_name", name)
        custom_inputs.append(ci)
    custom.set_editor_property("inputs", custom_inputs)

    me.connect_material_expressions(p_tex,    "", custom, "pos_tex")
    me.connect_material_expressions(p_uv2,    "", custom, "uv2")
    me.connect_material_expressions(p_curr,   "", custom, "current_frame")
    me.connect_material_expressions(p_frames, "", custom, "frame_count")
    me.connect_material_expressions(p_bind,   "", custom, "bind_local")
    me.connect_material_expressions(p_lo,     "", custom, "bounds_min")
    me.connect_material_expressions(p_hi,     "", custom, "bounds_max")

    # The Custom node now returns a WORLD-SPACE OFFSET in centimeters,
    # ready to be summed into WPO. The HLSL does the meter→cm scale
    # and the Y-up→Z-up swizzle so a Mixamo bake plays correctly
    # without a Blueprint Tick or special component setup.
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
    # Stamp the build number into the material's asset tags so the
    # auto-runner can detect a stale graph (e.g. when this script
    # gets updated to fix a coordinate-system bug, the user reopens
    # the project, and the old M_OpenVAT.uasset is still on disk).
    # `EditorAssetLibrary.set_metadata_tag` writes to the
    # ObjectPathMetaData, persists across editor restarts, and is
    # cheap to read back without loading the full graph.
    try:
        unreal.EditorAssetLibrary.set_metadata_tag(
            mat, "OpenVATBuild", str(OPENVAT_BUILD))
    except Exception as e:
        unreal.log_warning("Could not stamp OpenVATBuild tag: " + str(e))
    unreal.EditorAssetLibrary.save_loaded_asset(mat)
    unreal.log("Built M_OpenVAT (Custom-node WPO + diffuse, build "
               + str(OPENVAT_BUILD) + ").")
    return mat


# ────────────────────────────────────────────────────────────────────
# 5. Spawn a SkeletalMeshActor into the current level using the
#    material we just built. The material is fully self-driving
#    (current_frame = Time × fps), so no Blueprint or Tick is needed.
# ────────────────────────────────────────────────────────────────────

def _editor_actor_library():
    """The Python class moved between UE versions — try both."""
    if hasattr(unreal, "EditorLevelLibrary"):
        return unreal.EditorLevelLibrary
    if hasattr(unreal, "EditorActorSubsystem"):
        return unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    return None


def _focus_viewport_on(actor):
    """Best-effort: focus the editor camera on the new actor so the
    user actually sees it. Both API entry points are wrapped in
    try/except because the exact class name moved between UE
    versions (5.0..5.3 = EditorLevelLibrary, 5.4+ = subsystems)."""
    try:
        if hasattr(unreal, "EditorLevelLibrary"):
            unreal.EditorLevelLibrary.set_selected_level_actors([actor])
            unreal.EditorLevelLibrary.editor_invalidate_viewports()
    except Exception:
        pass
    try:
        sub = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if sub: sub.editor_invalidate_viewports()
    except Exception:
        pass


def spawn_dancer_in_level():
    """Drop a SkeletalMeshActor into the open level and apply
    M_OpenVAT. Idempotent — deletes a previous instance first.

    Loud on every failure mode so a "nothing visible" result is
    diagnosable from the Output Log instead of an empty viewport.
    """
    actor_lib = _editor_actor_library()
    if actor_lib is None:
        unreal.log_warning(
            "spawn_dancer_in_level: no EditorLevelLibrary or "
            "EditorActorSubsystem available on this engine. Drag "
            "/Game/Rumba/SK_Rumba into the level manually, set its "
            "Material 0 to /Game/VATDemo/M_OpenVAT and "
            "Animation Mode = None.")
        return None

    mesh = find_skeletal_mesh()
    mat  = unreal.load_asset(DEMO_DIR + "/M_OpenVAT")
    if mesh is None:
        unreal.log_error("spawn_dancer_in_level: no SkeletalMesh found "
                         "under /Game/Rumba/ — glTF import must have "
                         "failed.")
        return None
    if mat is None:
        unreal.log_error("spawn_dancer_in_level: /Game/VATDemo/M_OpenVAT "
                         "not found — material build must have failed.")
        return None
    unreal.log("spawn_dancer_in_level: loaded SK_Rumba (%s) + M_OpenVAT (%s)"
               % (mesh, mat))

    # Delete any pre-existing OpenVAT_Dancer so reruns don't pile up.
    removed = 0
    try:
        all_actors = actor_lib.get_all_level_actors()
    except Exception as e:
        unreal.log_warning("get_all_level_actors raised: " + str(e))
        all_actors = []
    for a in all_actors:
        try:
            if a and a.get_actor_label() == "OpenVAT_Dancer":
                actor_lib.destroy_actor(a)
                removed += 1
        except Exception:
            pass
    if removed:
        unreal.log("spawn_dancer_in_level: removed %d previous "
                   "OpenVAT_Dancer instance(s)." % removed)

    # Spawn at the world origin facing +X. The material does the
    # glTF→Unreal swizzle (Y-up→Z-up, meters→cm) inside the Custom
    # node so the actor's transform doesn't need to compensate.
    location = unreal.Vector(0, 0, 0)
    rotation = unreal.Rotator(0, 0, 0)
    try:
        actor = actor_lib.spawn_actor_from_class(
            unreal.SkeletalMeshActor, location, rotation)
    except Exception as e:
        unreal.log_error("spawn_actor_from_class raised: " + str(e))
        return None
    if actor is None:
        unreal.log_error("spawn_actor_from_class returned None — "
                         "level may not be loaded.")
        return None
    actor.set_actor_label("OpenVAT_Dancer")

    skel_comp = actor.skeletal_mesh_component
    if skel_comp is None:
        unreal.log_error("Spawned actor has no skeletal_mesh_component.")
        return actor
    # Property name moved between UE versions: try all known names.
    set_ok = False
    for attempt in ("set_skeletal_mesh_asset", "set_skeletal_mesh"):
        if hasattr(skel_comp, attempt):
            try:
                getattr(skel_comp, attempt)(mesh)
                set_ok = True
                break
            except Exception as e:
                unreal.log_warning("%s failed: %s" % (attempt, e))
    if not set_ok:
        for prop in ("skeletal_mesh_asset", "skeletal_mesh"):
            try:
                skel_comp.set_editor_property(prop, mesh)
                set_ok = True
                break
            except Exception:
                pass
    if not set_ok:
        unreal.log_error("Could not assign SK_Rumba to the spawned "
                         "actor via any known property name. The actor "
                         "will render empty.")

    try:
        skel_comp.set_material(0, mat)
    except Exception as e:
        unreal.log_warning("set_material(0) failed: " + str(e))

    # Disable engine animation — VAT material drives all vertex motion.
    try:
        skel_comp.set_editor_property("animation_mode",
            unreal.AnimationMode.ANIMATION_NONE)
    except Exception:
        pass

    _focus_viewport_on(actor)
    unreal.log("Spawned OpenVAT_Dancer at (200, 0, 0) facing -X. "
               "Look in the viewport — material `Time × fps` ticks "
               "in the editor too. If you still see an empty scene, "
               "press F to frame the selection.")
    return actor


# ────────────────────────────────────────────────────────────────────
# Entry point
# ────────────────────────────────────────────────────────────────────

def main():
    unreal.log("=== QtMeshEditor OpenVAT demo bootstrap ===")
    unreal.log("step 1/5: importing bake assets")
    import_bake_assets()

    # Cross-check: the imported skeletal mesh and both textures must
    # exist before we go further. If any one of these is missing the
    # spawn step will produce an empty placeholder and the user sees
    # an empty viewport — exactly the failure mode we're trying to
    # avoid.
    mesh = find_skeletal_mesh()
    pos  = unreal.load_asset(BAKE_DIR + "/T_OpenVAT_Pos")
    diff = unreal.load_asset(BAKE_DIR + "/T_Boss_Diffuse")
    unreal.log("  SkeletalMesh    = %s" % mesh)
    unreal.log("  T_OpenVAT_Pos   = %s" % pos)
    unreal.log("  T_Boss_Diffuse  = %s" % diff)
    if mesh is None:
        unreal.log_error(
            "=== Bootstrap STOPPED: no SkeletalMesh found under "
            "/Game/Rumba/. Drag Content/Rumba/source.gltf into the "
            "Content Browser manually and re-run this script. ===")
        return

    unreal.log("step 2/5: verifying glTF carries TEXCOORD_1")
    if not verify_gltf_has_uv2():
        unreal.log_warning(
            "=== Bootstrap STOPPED: source.gltf is missing TEXCOORD_1. "
            "Re-bake with `qtmesh vat <file> --anim <name> --emit-uv2 "
            "-o <dir>` and overwrite the files under Content/Rumba/. ===")
        return

    unreal.log("step 3/5: reading sidecar")
    frames, mn, mx = read_sidecar()
    unreal.log("  frames=%d min=%s max=%s" % (frames, mn, mx))

    unreal.log("step 4/5: building material")
    mat = build_material(frames, mn, mx)
    if mat is None:
        unreal.log_error("=== Bootstrap STOPPED: material build failed. ===")
        return

    unreal.log("step 5/5: spawning actor in the open level")
    actor = spawn_dancer_in_level()
    if actor is None:
        unreal.log_warning(
            "=== Bootstrap PARTIAL: SK_Rumba + M_OpenVAT exist under "
            "/Game/, but no actor was spawned. Drag SK_Rumba into the "
            "viewport manually and set Material 0 to M_OpenVAT. ===")
        return

    unreal.log("=== Bootstrap done. OpenVAT_Dancer is selected in the "
               "viewport — press F to frame it if you don't see it. "
               "Animation runs in the editor (no Play required) because "
               "the material drives `current_frame = Time × fps`. ===")


if __name__ == "__main__":
    main()
