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
#   attribute directly into the glTF. Unreal's StaticMesh importer
#   reorders vertices for cache locality, but a vertex attribute
#   travels WITH its vertex through any reorder — so TEXCOORD_1
#   still points at the right column in the imported mesh. No
#   runtime UV2-baking, no bind-sidecar matcher, no engine-version-
#   specific Geometry Script paths. The shader just reads
#   TexCoord[1] and indexes the position texture by that.
#
#   We force StaticMesh import (overriding Interchange's pipeline
#   stack) because UE's SkeletalMesh importer renormalises every
#   render section's secondary UVs to its own [0,1] range — which
#   destroys the absolute column index. Skipping the skeleton is
#   fine here: the VAT material drives every vertex from the
#   position texture, so the skeleton is unused at runtime anyway.

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
OPENVAT_BUILD = 11


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
    UE 5.0+; no separate plugin required), forcing **static mesh**
    import so the secondary UV channel (TEXCOORD_1, holding the
    per-vertex VAT column index) survives intact.

    Why static instead of skeletal:
      * The VAT replaces skinning entirely — the material's WPO
        drives every vertex from the position texture, the skeleton
        is unused at runtime.
      * UE's SkeletalMesh importer renormalises every render
        section's secondary UV channel into its own [0,1] range, so
        primitive #2's TEXCOORD_1 (originally columns 1024..1437)
        comes back rescaled and reads the wrong slice of the position
        texture. This shows up in-editor as "head and arms are
        static; torso is animated but with chaotic triangles." The
        StaticMesh importer preserves UV values as-is.
      * Using a static mesh also lets us spawn a plain
        StaticMeshActor — no AnimationMode toggle, no skeletal-mesh
        component property-name dance.

    Returns the imported StaticMesh, or None on failure.
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

    # Override the default Interchange pipeline stack so the glTF is
    # imported as a StaticMesh (skinning disabled) regardless of the
    # source's <skin> presence.
    #
    # UE 5.7 structure (verified against the engine headers):
    #   InterchangeGenericAssetsPipeline
    #     ├ common_meshes_properties        (UInterchangeGenericCommonMeshesProperties)
    #     │   └ force_all_mesh_as_type      (EInterchangeForceMeshType)
    #     └ mesh_pipeline                   (UInterchangeGenericMeshPipeline)
    #         ├ b_import_static_meshes      (bool, default True)
    #         └ b_import_skeletal_meshes    (bool, default True)
    #
    # Earlier attempts set `force_all_mesh_as_type` on `mesh_pipeline`
    # (wrong sub-object) so the override silently no-op'd and we
    # ended up with a SkeletalMesh named `SM_Rumba`. Set it on
    # `common_meshes_properties` and also disable skeletal-mesh
    # creation on `mesh_pipeline` for belt-and-braces.
    pipeline_obj = None
    try:
        pipeline_obj = unreal.InterchangeGenericAssetsPipeline()

        common = pipeline_obj.get_editor_property("common_meshes_properties")
        if common is not None:
            try:
                common.set_editor_property("force_all_mesh_as_type",
                    unreal.InterchangeForceMeshType.IFMT_STATIC_MESH)
                unreal.log("Interchange: common_meshes_properties."
                           "force_all_mesh_as_type = IFMT_STATIC_MESH")
            except Exception as e:
                unreal.log_warning("Could not set force_all_mesh_as_type: "
                                   + str(e))
            # Keep every primitive as its own render section. Without
            # this, Interchange merges primitives that share a material
            # name (Mixamo: Skin_MAT covers head + arms + feet — three
            # primitives → one section). The merged section
            # renumbers its TEXCOORD_1 within its own vertex buffer,
            # so the column index points at the wrong texture column
            # and head/arms render at body positions or vice versa.
            # Symptom: submeshes appear to render through each other
            # (the "no z-index" look — actually a per-vertex collapse).
            #
            # UE 5.7's Python binding rejects `b_keep_sections_separate`
            # even though that's the auto-snake-case translation of
            # the C++ name; the actually-exposed Python name is
            # `keep_sections_separate` (the `b` prefix is stripped for
            # this particular bool — likely because the C++ side has a
            # `ScriptName` UFUNCTION getter/setter pair). Try every
            # variant and log which one took.
            ks_done = False
            for name in ("keep_sections_separate",
                         "b_keep_sections_separate",
                         "bKeepSectionsSeparate"):
                try:
                    common.set_editor_property(name, True)
                    unreal.log("Interchange: common_meshes_properties."
                               + name + " = True (each glTF primitive "
                               "becomes its own section).")
                    ks_done = True
                    break
                except Exception:
                    pass
            if not ks_done:
                unreal.log_warning("Could not set keep-sections-separate "
                                   "under any known name — render-section "
                                   "merging will collapse Mixamo's "
                                   "material-shared primitives and the "
                                   "VAT will render with column-index "
                                   "mismatches.")
        else:
            unreal.log_warning("Interchange pipeline has no "
                               "`common_meshes_properties` sub-object on "
                               "this UE version — static-mesh override "
                               "may not take effect.")

        mesh_pipe = pipeline_obj.get_editor_property("mesh_pipeline")
        if mesh_pipe is not None:
            try:
                mesh_pipe.set_editor_property("b_import_skeletal_meshes",
                                              False)
                unreal.log("Interchange: mesh_pipeline."
                           "b_import_skeletal_meshes = False")
            except Exception as e:
                unreal.log_warning("Could not disable skeletal-mesh "
                                   "import: " + str(e))

        # `override_pipelines` is a TArray<FSoftObjectPath>. The transient
        # pipeline object we just built has no on-disk path, so the
        # SoftObjectPath route returns an empty path that the engine
        # ignores (silently keeping the default skeletal-import stack).
        # The reliable path is to pass the pipeline object directly via
        # `pipelines` (Python list of Interchange pipeline objects); UE
        # 5.7 accepts that and uses it in place of the default stack.
        if hasattr(params, "pipelines"):
            params.pipelines = [pipeline_obj]
            unreal.log("Interchange: params.pipelines = [transient "
                       "pipeline override]")
        else:
            params.override_pipelines = [
                unreal.SoftObjectPath(pipeline_obj.get_path_name())
            ]
        unreal.log("Interchange: forcing static-mesh import to "
                   "preserve TEXCOORD_1 column index intact.")
    except Exception as e:
        unreal.log_warning("Could not override Interchange pipeline "
                           "(" + str(e) + "); falling back to default "
                           "import. If you see static head/arms with "
                           "chaotic body triangles, the secondary UV "
                           "channel got renormalised — drag source.gltf "
                           "in manually and choose Static Mesh in the "
                           "import dialog.")

    mgr = unreal.InterchangeManager.get_interchange_manager_scripted()
    try:
        mgr.import_asset(BAKE_DIR, src_data, params)
    except Exception as e:
        unreal.log_warning("Interchange import failed: " + str(e)
            + " — please drag the .gltf in manually.")
        return None

    return unreal.load_asset(BAKE_DIR + "/" + dst_name + "." + dst_name)


def find_imported_mesh():
    """Locate the imported Rumba mesh (StaticMesh preferred, falling
    back to SkeletalMesh for backward compatibility with bakes that
    were imported by an earlier version of this script).

    Interchange's `destination_name` parameter is advisory — when
    importing a glTF, the framework writes a subtree under
    `/Game/Rumba/source/...` (mesh, skeleton, animation, materials)
    and does NOT honour our requested `SK_Rumba` short name. So the
    asset can land at any of several paths depending on UE / Interchange
    version and whether the import was static or skeletal.

    We try every known path, then fall back to an AssetRegistry
    search under `/Game/Rumba/` for the first StaticMesh, and finally
    SkeletalMesh, asset. Returns (asset, is_static) so the caller
    knows which actor class to spawn.
    """
    static_candidates = [
        "/Game/Rumba/SM_Rumba",
        "/Game/Rumba/source/StaticMeshes/SM_Rumba",
        "/Game/Rumba/Rumba_Dancing_mesh",
        "/Game/Rumba/source/StaticMeshes/Rumba_Dancing_mesh",
    ]
    skel_candidates = [
        "/Game/Rumba/SK_Rumba",
        "/Game/Rumba/source/SkeletalMeshes/SK_Rumba",
        "/Game/Rumba/Rumba_Dancing_mesh",
        "/Game/Rumba/source/SkeletalMeshes/Rumba_Dancing_mesh",
    ]
    for path in static_candidates:
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            asset = unreal.load_asset(path)
            if asset is not None and isinstance(asset, unreal.StaticMesh):
                unreal.log("find_imported_mesh: StaticMesh at " + path)
                return asset, True
    for path in skel_candidates:
        if unreal.EditorAssetLibrary.does_asset_exist(path):
            asset = unreal.load_asset(path)
            if asset is not None and isinstance(asset, unreal.SkeletalMesh):
                unreal.log("find_imported_mesh: SkeletalMesh at "
                           + path + " (fallback — UV2 may be "
                           "renormalised; expect rendering glitches).")
                return asset, False

    # Last resort — AssetRegistry sweep under /Game/Rumba/, preferring
    # StaticMesh over SkeletalMesh.
    try:
        ar = unreal.AssetRegistryHelpers.get_asset_registry()
        assets = ar.get_assets_by_path("/Game/Rumba", recursive=True)
        static_hit, skel_hit = None, None
        for ad in assets:
            try:
                cls = str(ad.get_class().get_name())
            except Exception:
                cls = ""
            if cls == "StaticMesh" and static_hit is None:
                static_hit = ad.get_asset()
            elif cls == "SkeletalMesh" and skel_hit is None:
                skel_hit = ad.get_asset()
        if static_hit:
            unreal.log("find_imported_mesh: AssetRegistry → "
                       "StaticMesh fallback.")
            return static_hit, True
        if skel_hit:
            unreal.log("find_imported_mesh: AssetRegistry → "
                       "SkeletalMesh fallback (UV2 renormalisation "
                       "may apply).")
            return skel_hit, False
    except Exception as e:
        unreal.log_warning("AssetRegistry sweep failed: " + str(e))
    return None, False


# Back-compat shim for any external code that imported the old name.
def find_skeletal_mesh():
    mesh, _is_static = find_imported_mesh()
    return mesh


def verify_imported_uv_channels(mesh):
    """Log the imported mesh's class so the user can confirm the
    static-mesh override took effect. Also log per-section vertex
    counts and material slot index — useful for diagnosing whether
    Interchange's material-merge step is corrupting the per-vertex
    TEXCOORD_1 column index."""
    if not mesh:
        return
    if isinstance(mesh, unreal.StaticMesh):
        unreal.log("verify_uv: imported as StaticMesh ✓.")
        try:
            num_lods = mesh.get_num_lods() if hasattr(mesh, "get_num_lods") else 1
            for lod in range(num_lods):
                if not hasattr(mesh, "get_num_sections"):
                    continue
                nsec = mesh.get_num_sections(lod)
                unreal.log("verify_uv: LOD " + str(lod) + " has "
                           + str(nsec) + " section(s).")
                for s in range(nsec):
                    try:
                        info = mesh.get_section_info(lod, s)
                        unreal.log("    section " + str(s) +
                                   ": material_slot=" + str(info.material_index))
                    except Exception:
                        pass
        except Exception as e:
            unreal.log_warning("section dump failed: " + str(e))
    elif isinstance(mesh, unreal.SkeletalMesh):
        unreal.log_error(
            "*** verify_uv: imported as SkeletalMesh — the "
            "static-mesh override failed.")
    else:
        unreal.log_warning("verify_uv: imported mesh is "
                           + str(type(mesh)) + " (unexpected).")


def import_bake_assets():
    """Bring textures + glTF in under /Game/Rumba.

    The bake's POSITION texture needs very specific import settings —
    Unreal's default Texture2D import gamma-corrects + DXT-compresses
    the data, which corrupts the per-vertex floats. We re-open after
    import and override.
    """
    _import_texture("mixamo.com_pos.png", "T_OpenVAT_Pos")
    _import_texture("Boss_diffuse.png",   "T_Boss_Diffuse")

    # Wipe any prior mesh-related imports under /Game/Rumba/ before
    # re-importing. Interchange will happily reuse an existing asset
    # rather than honouring our updated pipeline settings, so a
    # leftover SM_Rumba from build N would carry N's section-merging
    # config even after build N+1 changes the override flags. Nuke
    # all the mesh-side assets every run; textures stay (their import
    # is idempotent and re-import is slow).
    try:
        ar = unreal.AssetRegistryHelpers.get_asset_registry()
        prior = ar.get_assets_by_path("/Game/Rumba", recursive=True)
        for ad in prior:
            try:
                cls = str(ad.get_class().get_name())
            except Exception:
                cls = ""
            if cls in ("StaticMesh", "SkeletalMesh", "Skeleton",
                       "PhysicsAsset", "AnimSequence", "Material",
                       "MaterialInstance", "MaterialInstanceConstant"):
                pkg = str(ad.package_name) if hasattr(ad, "package_name") else None
                if pkg:
                    unreal.EditorAssetLibrary.delete_asset(pkg)
    except Exception as e:
        unreal.log_warning("Could not pre-clean prior mesh imports: "
                           + str(e))

    _import_gltf_via_interchange("source.gltf", "SM_Rumba")

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

// Sample the bake at the current/next frame AND at frame 0 (the
// bake's first frame, which is the animation's bind/T-pose since
// Mixamo authoring always starts there). Subtracting frame 0
// inside the same coordinate space sidesteps every "what coord
// system is bind_local in?" question — we never need to know how
// Interchange chose to swizzle/scale the mesh on import. The
// result is a delta in glTF Y-up meters space, which we then
// swizzle + scale to Unreal Z-up cm for WPO.
float3 p_curr = pos_tex.Load(int3(col, base_row + curr, 0)).rgb;
float3 p_next = pos_tex.Load(int3(col, base_row + nxt,  0)).rgb;
float3 p_bind = pos_tex.Load(int3(col, base_row + 0,    0)).rgb;
float3 p = lerp(p_curr, p_next, blend);

float3 delta_norm  = p - p_bind;
float3 delta_yup_m = delta_norm * (bounds_max - bounds_min);
float3 delta_zup_cm = float3(delta_yup_m.x,
                             -delta_yup_m.z,
                              delta_yup_m.y) * 100.0;
return delta_zup_cm;
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

    # Force the surface modes explicitly. MaterialFactoryNew's defaults
    # vary across UE versions (5.7 has a project setting that can
    # default new materials to Masked or Translucent, which disables
    # depth-writes and produces the "no z-index" look — see-through
    # body parts overlapping each other in arbitrary draw order).
    # Hardcode Opaque/Surface/DefaultLit/single-sided so the mesh
    # depth-tests correctly regardless of project settings.
    try:
        mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_OPAQUE)
        mat.set_editor_property("material_domain",
            unreal.MaterialDomain.MD_SURFACE)
        mat.set_editor_property("shading_model",
            unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
        mat.set_editor_property("two_sided", False)
        unreal.log("Material modes: Opaque / Surface / DefaultLit / "
                   "single-sided.")
    except Exception as e:
        unreal.log_warning("Could not set material modes: " + str(e))

    me = unreal.MaterialEditingLibrary

    # current_frame = Time × fps (auto-loops in the material).
    # No scalar parameter / no Tick / no Blueprint needed — a static
    # actor in the level with this material applied animates forever.
    # bIgnorePause keeps it ticking even when the editor world is
    # paused (e.g. user is in Simulate but not Play).
    p_time = me.create_material_expression(mat,
        unreal.MaterialExpressionTime, -1000, -200)
    try:
        p_time.set_editor_property("b_ignore_pause", True)
    except Exception:
        pass

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

    # Bind-pose object-space coordinate. We subtract this inside the
    # Custom node so WPO carries the per-frame OFFSET (target − bind)
    # instead of an absolute target. Without this the dancer collapses
    # to ~1 cm because the bake's positions are in meters and Unreal's
    # WPO is interpreted in centimeters.
    #
    # We import as StaticMesh (see _import_gltf_via_interchange) so
    # MaterialExpressionLocalPosition is the right source — it's the
    # vertex's bind-pose local-space coordinate, exactly what we need.
    # PreSkinnedPosition would be wrong here: it's only defined on
    # skeletal-mesh materials, and we deliberately bypass the skeleton.
    #
    # IMPORTANT: set IncludedOffsets = ExcludeOffsets so the position
    # we read here is the *pre-WPO* bind vertex, not the post-WPO
    # current vertex. The default (IncludeOffsets) creates a feedback
    # loop — WPO = target - LocalPosition where LocalPosition already
    # includes the previous WPO — and the dancer converges to its bind
    # pose with only a few oscillating vertices, which is exactly the
    # symptom we were seeing before this line went in.
    p_bind = me.create_material_expression(mat,
        unreal.MaterialExpressionLocalPosition, -800, 400)
    try:
        p_bind.set_editor_property("included_offsets",
            unreal.PositionIncludedOffsets.EXCLUDE_OFFSETS)
        unreal.log("LocalPosition.IncludedOffsets = ExcludeOffsets "
                   "(breaks the WPO feedback loop).")
    except Exception as e:
        unreal.log_warning("Could not set LocalPosition.IncludedOffsets "
                           "to ExcludeOffsets: " + str(e)
                           + " — dancer may converge to bind pose.")

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
    """Drop a StaticMeshActor (preferred) or SkeletalMeshActor
    (fallback for legacy imports) into the open level and apply
    M_OpenVAT. Idempotent — deletes a previous instance first.

    Loud on every failure mode so a "nothing visible" result is
    diagnosable from the Output Log instead of an empty viewport.
    """
    actor_lib = _editor_actor_library()
    if actor_lib is None:
        unreal.log_warning(
            "spawn_dancer_in_level: no EditorLevelLibrary or "
            "EditorActorSubsystem available on this engine. Drag "
            "the imported mesh into the level manually, set its "
            "Material 0 to /Game/VATDemo/M_OpenVAT.")
        return None

    mesh, is_static = find_imported_mesh()
    mat  = unreal.load_asset(DEMO_DIR + "/M_OpenVAT")
    if mesh is None:
        unreal.log_error("spawn_dancer_in_level: no mesh found under "
                         "/Game/Rumba/ — glTF import must have failed.")
        return None
    if mat is None:
        unreal.log_error("spawn_dancer_in_level: /Game/VATDemo/M_OpenVAT "
                         "not found — material build must have failed.")
        return None
    unreal.log("spawn_dancer_in_level: loaded "
               + ("StaticMesh" if is_static else "SkeletalMesh")
               + " (" + str(mesh) + ") + M_OpenVAT (" + str(mat) + ")")

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

    # Spawn at the world origin. The material does the glTF→Unreal
    # swizzle (Y-up→Z-up, meters→cm) inside the Custom node so the
    # actor's transform doesn't need to compensate.
    location = unreal.Vector(0, 0, 0)
    rotation = unreal.Rotator(0, 0, 0)
    actor_class = (unreal.StaticMeshActor if is_static
                   else unreal.SkeletalMeshActor)
    try:
        actor = actor_lib.spawn_actor_from_class(
            actor_class, location, rotation)
    except Exception as e:
        unreal.log_error("spawn_actor_from_class raised: " + str(e))
        return None
    if actor is None:
        unreal.log_error("spawn_actor_from_class returned None — "
                         "level may not be loaded.")
        return None
    actor.set_actor_label("OpenVAT_Dancer")

    if is_static:
        sm_comp = actor.static_mesh_component
        if sm_comp is None:
            unreal.log_error("Spawned actor has no static_mesh_component.")
            return actor
        set_ok = False
        for attempt in ("set_static_mesh",):
            if hasattr(sm_comp, attempt):
                try:
                    getattr(sm_comp, attempt)(mesh)
                    set_ok = True
                    break
                except Exception as e:
                    unreal.log_warning("%s failed: %s" % (attempt, e))
        if not set_ok:
            try:
                sm_comp.set_editor_property("static_mesh", mesh)
                set_ok = True
            except Exception:
                pass
        if not set_ok:
            unreal.log_error("Could not assign StaticMesh to the "
                             "spawned actor. The actor will render empty.")
        # Apply M_OpenVAT to *every* material slot. Rumba's glTF has 11
        # primitives → 11 slots → setting only slot 0 leaves the other
        # 10 submeshes using the Interchange-imported PBR materials
        # (Skin_MAT, Clothes_MAT, …) that have no WPO, so they render
        # in bind pose while slot 0 alone animates. That's exactly the
        # "static head + animated vest" symptom in the screenshot.
        num_slots = mesh.get_num_sections(0) if hasattr(
            mesh, "get_num_sections") else len(mesh.static_materials)
        unreal.log("Applying M_OpenVAT to " + str(num_slots) + " slots.")
        for i in range(num_slots):
            try:
                sm_comp.set_material(i, mat)
            except Exception as e:
                unreal.log_warning("set_material(" + str(i) + ") failed: "
                                   + str(e))
    else:
        skel_comp = actor.skeletal_mesh_component
        if skel_comp is None:
            unreal.log_error("Spawned actor has no skeletal_mesh_component.")
            return actor
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
            unreal.log_error("Could not assign SkeletalMesh to the "
                             "spawned actor. The actor will render empty.")
        # Apply to every slot — see static-mesh branch above.
        try:
            num_slots = len(mesh.materials) if hasattr(mesh, "materials") else 1
        except Exception:
            num_slots = 1
        for i in range(num_slots):
            try:
                skel_comp.set_material(i, mat)
            except Exception as e:
                unreal.log_warning("set_material(" + str(i) + ") failed: "
                                   + str(e))
        # Disable engine animation — VAT material drives all vertex motion.
        try:
            skel_comp.set_editor_property("animation_mode",
                unreal.AnimationMode.ANIMATION_NONE)
        except Exception:
            pass

    _focus_viewport_on(actor)
    unreal.log("Spawned OpenVAT_Dancer at (0, 0, 0). Material `Time × fps` "
               "ticks in the editor — no Play required. If the viewport "
               "looks empty, press F to frame the selection.")
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
    mesh, _is_static = find_imported_mesh()
    pos  = unreal.load_asset(BAKE_DIR + "/T_OpenVAT_Pos")
    diff = unreal.load_asset(BAKE_DIR + "/T_Boss_Diffuse")
    unreal.log("  Mesh            = %s" % mesh)
    unreal.log("  T_OpenVAT_Pos   = %s" % pos)
    unreal.log("  T_Boss_Diffuse  = %s" % diff)
    if mesh is None:
        unreal.log_error(
            "=== Bootstrap STOPPED: no mesh found under "
            "/Game/Rumba/. Drag Content/Rumba/source.gltf into the "
            "Content Browser manually (choose Static Mesh in the "
            "import dialog) and re-run this script. ===")
        return

    unreal.log("step 2/5: verifying glTF + imported mesh carry UV2")
    verify_imported_uv_channels(mesh)
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
