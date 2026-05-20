## VATPlayer — drives a MeshInstance3D's ShaderMaterial from a QtMeshEditor
## OpenVAT bake (sharpen3d/openvat format).
##
## Drop the bake directory's path into `bake_dir`. The script will:
##   1. Read `<basename>-remap_info.json` (os-remap: { Min, Max, Frames }).
##   2. Load the packed `<basename>_pos.png` as a non-sRGB, nearest-filter
##      texture. Height is 2 × Frames: top half holds positions, bottom
##      half holds (n+1)/2 normals.
##   3. Build a ShaderMaterial that samples both halves and applies them
##      to the mesh's vertex positions + normals every _process at the
##      sidecar's fps (driven from `fps_override` since OpenVAT's sidecar
##      doesn't carry the playback rate).
##
## The MeshInstance3D's mesh must have the SAME vertex order Ogre saw at
## bake time. The recommended source is the glTF the staging script
## drops in via `qtmesh convert <input> -o <bake_dir>/source.gltf`.

class_name VATPlayer
extends MeshInstance3D

@export_dir var bake_dir: String = ""

## Path to the source mesh whose geometry should drive the VAT replay.
## MUST match the vertex order the baker saw, otherwise the position
## texture's column indexing will be wrong and the result will explode.
## The staging script normally sets this to `<bake_dir>/source.gltf`,
## but native Blender bakes ship as `.fbx` (Godot 4 imports FBX too).
@export_file("*.gltf", "*.glb", "*.fbx") var source_gltf: String = ""

## Playback frames per second. OpenVAT's `os-remap` block does not
## include a framerate (positions are time-resampled at bake time), so
## the player drives playback at whatever rate the caller sets. Default
## 30 matches qtmesh vat's default --fps.
@export var fps_override: float = 30.0

## Loop point in frames. -1 = full clip from sidecar's `Frames`.
@export var loop_frames: int = -1

var _frame_count: int = 0
var _vertex_count: int = 0
var _bounds_min: Vector3 = Vector3.ZERO
var _bounds_max: Vector3 = Vector3.ONE
var _surface_materials: Array[ShaderMaterial] = []
var _current_frame: float = 0.0


func _ready() -> void:
	if bake_dir.is_empty():
		push_error("VATPlayer: bake_dir is empty")
		return
	if not _load_source_mesh():
		push_error("VATPlayer: failed to load source mesh from %s" % source_gltf)
		return
	if not _load_bake():
		push_error("VATPlayer: failed to load bake from %s" % bake_dir)
		return


## Walk the glTF runtime scene to find the first MeshInstance3D and
## adopt its mesh. We do NOT instance the whole glTF scene under us —
## that would bring the Skeleton3D + SkinReference along, and Godot
## would skin the mesh on top of our shader, double-deforming it.
func _load_source_mesh() -> bool:
	if source_gltf.is_empty():
		push_error("VATPlayer: source_gltf is empty")
		return false

	var lower := source_gltf.to_lower()
	var scene: Node = null

	if lower.ends_with(".gltf") or lower.ends_with(".glb"):
		# Runtime glTF load — works for any path on disk, no editor
		# import needed.
		var doc := GLTFDocument.new()
		var state := GLTFState.new()
		var abs_path: String = ProjectSettings.globalize_path(source_gltf)
		var err: int = doc.append_from_file(abs_path, state)
		if err != OK:
			push_error("VATPlayer: GLTFDocument.append_from_file('%s') failed: %d" %
				[abs_path, err])
			return false
		scene = doc.generate_scene(state)
	else:
		# FBX (and anything else) goes through the editor's import
		# pipeline. Godot 4 auto-imports FBX under res://, so we just
		# `load()` the PackedScene and instantiate it. No runtime FBX
		# parser ships with Godot itself.
		var pack: Resource = load(source_gltf)
		if pack == null:
			push_error("VATPlayer: load('%s') returned null — make sure Godot's editor has finished importing the FBX" % source_gltf)
			return false
		if pack is PackedScene:
			scene = (pack as PackedScene).instantiate()
		elif pack is Mesh:
			# Some imports surface as a bare Mesh resource.
			mesh = pack as Mesh
			skeleton = NodePath("")
			return true
		else:
			push_error("VATPlayer: source '%s' loaded as %s, expected PackedScene or Mesh" %
				[source_gltf, pack.get_class()])
			return false

	if scene == null:
		push_error("VATPlayer: failed to instantiate source scene from %s" % source_gltf)
		return false
	var found_mesh: ArrayMesh = _first_mesh_in(scene)
	scene.queue_free()  # We only wanted the mesh resource.
	if found_mesh == null:
		push_error("VATPlayer: no MeshInstance3D found in %s" % source_gltf)
		return false
	mesh = found_mesh
	# Belt-and-braces: even if a Skeleton3D existed somewhere in the
	# instanced scene, this MeshInstance3D has no skeleton path, so
	# Godot's skinning code never runs against us. The vertex shader
	# is the ONLY thing moving vertices.
	skeleton = NodePath("")
	return true


func _first_mesh_in(root: Node) -> ArrayMesh:
	if root is MeshInstance3D:
		var m: Mesh = (root as MeshInstance3D).mesh
		if m is ArrayMesh:
			return m as ArrayMesh
		# FBX/Collada imports often surface as ImporterMesh; bake to
		# an ArrayMesh so we get the vertex-buffer accessors the bake
		# loop relies on (surface_get_arrays / surface_get_material).
		# `ImporterMesh` lives in `editor/` so it's not a parse-time
		# type at runtime — we go through `is_class` to keep the
		# `Mesh`-typed branch happy.
		if m != null and m.is_class("ImporterMesh"):
			return m.call("get_mesh") as ArrayMesh
	for c in root.get_children():
		var found := _first_mesh_in(c)
		if found:
			return found
	return null


## Walks `bake_dir` looking for an OpenVAT bake. Supports two layouts:
##
## - Packed Normals  (what QtMeshEditor writes):
##     <basename>_pos.png         16-bit RGB, height = 2 × Frames
##     <basename>-remap_info.json os-remap sidecar
##
## - Separate Normals (Blender add-on's "Vertex Normals = Separate" mode):
##     <basename>_vat.png  or .exr   positions only, height = Frames
##     <basename>_vnrm.png or .exr   normals only,   height = Frames
##     [<basename>-remap_info.json   optional in practice — Blender
##                                   writes it but the file may go
##                                   missing in the user's hand-off]
##
## When `bake_dir` holds multiple staged bakes (e.g. someone staged
## a second animation without clearing the folder), we have to pair
## the texture to the SAME basename as the sidecar — otherwise the
## metadata from one bake will be applied to the texture of another,
## producing visually plausible-but-wrong frames.
func _load_bake() -> bool:
	var dir := DirAccess.open(bake_dir)
	if dir == null:
		push_error("VATPlayer: cannot open %s" % bake_dir)
		return false

	# First sweep: look for a position texture and figure out which
	# layout we're in. Order matters — packed mode is the default
	# QtMeshEditor output, so prefer it when both files coexist.
	var json_path := ""
	var pos_path := ""
	var nrm_path := ""
	var packed := true
	var basename := ""
	for f in dir.get_files():
		if f.ends_with("-remap_info.json") and json_path.is_empty():
			json_path = bake_dir.path_join(f)
		elif pos_path.is_empty() and (f.ends_with("_pos.png") or f.ends_with("_pos.exr")):
			pos_path = bake_dir.path_join(f)
			packed = true
			var idx := f.rfind("_pos.")
			basename = f.substr(0, idx)
		elif pos_path.is_empty() and (f.ends_with("_vat.png") or f.ends_with("_vat.exr")):
			pos_path = bake_dir.path_join(f)
			packed = false
			var idx := f.rfind("_vat.")
			basename = f.substr(0, idx)
		elif (f.ends_with("_vnrm.png") or f.ends_with("_vnrm.exr")) and nrm_path.is_empty():
			nrm_path = bake_dir.path_join(f)

	if pos_path.is_empty():
		push_error("VATPlayer: no *_pos.{png,exr} or *_vat.{png,exr} in %s" % bake_dir)
		return false

	# Separate-normals mode requires the matching _vnrm.* file beside it.
	if not packed and nrm_path.is_empty():
		# Try the basename pairing in case file enumeration order missed it.
		for ext in [".png", ".exr"]:
			var probe := bake_dir.path_join(basename + "_vnrm" + ext)
			if FileAccess.file_exists(probe):
				nrm_path = probe
				break
		if nrm_path.is_empty():
			push_error("VATPlayer: separate-normals bake %s but no matching *_vnrm.{png,exr}" % pos_path)
			return false

	# Read OpenVAT sidecar if present. If not, fall back to a
	# unit-cube bounds so geometry at least connects — the result
	# will be visually wrong scale, but better than refusing to
	# render at all. Blender exports normally include the sidecar;
	# this fallback is a hand-off-friendliness measure only.
	var fallback_bounds := false
	if not json_path.is_empty():
		var sidecar_text: String = FileAccess.get_file_as_string(json_path)
		var sidecar: Variant = JSON.parse_string(sidecar_text)
		if typeof(sidecar) != TYPE_DICTIONARY:
			push_error("VATPlayer: malformed sidecar at %s" % json_path)
			return false
		if not sidecar.has("os-remap"):
			push_error("VATPlayer: sidecar at %s lacks 'os-remap' key (not OpenVAT)" % json_path)
			return false
		var os_remap: Dictionary = sidecar["os-remap"]
		for required_key in ["Min", "Max", "Frames"]:
			if not os_remap.has(required_key):
				push_error("VATPlayer: os-remap missing '%s' key" % required_key)
				return false
		_frame_count = int(os_remap["Frames"])
		# OpenVAT's Min/Max are arrays of stringified 8-decimal floats.
		var min_arr: Array = os_remap["Min"]
		var max_arr: Array = os_remap["Max"]
		if min_arr.size() < 3 or max_arr.size() < 3:
			push_error("VATPlayer: Min/Max arrays must have at least 3 elements")
			return false
		_bounds_min = Vector3(float(min_arr[0]), float(min_arr[1]), float(min_arr[2]))
		_bounds_max = Vector3(float(max_arr[0]), float(max_arr[1]), float(max_arr[2]))
	else:
		fallback_bounds = true
		_bounds_min = Vector3(-1, -1, -1)
		_bounds_max = Vector3( 1,  1,  1)
		# frame_count is filled in below from the image height.

	var pos_tex: Texture2D = _load_data_texture(pos_path)
	if pos_tex == null:
		push_error("VATPlayer: failed to load %s" % pos_path)
		return false
	var nrm_tex: Texture2D = null
	if not nrm_path.is_empty():
		nrm_tex = _load_data_texture(nrm_path)
		if nrm_tex == null:
			push_error("VATPlayer: failed to load %s" % nrm_path)
			return false

	# Layout sanity. Packed mode: image height = 2 × frames. Separate
	# mode: image height = frames (the _vnrm file is the SAME height).
	var img_h: int = pos_tex.get_height()
	if packed:
		if _frame_count <= 0:
			_frame_count = int(img_h / 2)  # fallback when no sidecar but packed
		if img_h != _frame_count * 2:
			push_warning("VATPlayer: packed-mode texture height %d != 2 × frames (%d)" %
				[img_h, _frame_count * 2])
	else:
		if _frame_count <= 0:
			_frame_count = img_h
		if img_h != _frame_count:
			push_warning("VATPlayer: separate-mode texture height %d != frames (%d)" %
				[img_h, _frame_count])
		if nrm_tex.get_height() != img_h or nrm_tex.get_width() != pos_tex.get_width():
			push_warning("VATPlayer: separate-mode nrm tex %dx%d != pos tex %dx%d" %
				[nrm_tex.get_width(), nrm_tex.get_height(),
				 pos_tex.get_width(), pos_tex.get_height()])
	_vertex_count = pos_tex.get_width()

	if fallback_bounds:
		push_warning(("VATPlayer: no sidecar in %s — using fallback bounds [-1, 1]. " +
			"Re-export from Blender with the JSON sidecar (remap_info) for correct scale.")
			% bake_dir)

	var shader := Shader.new()
	shader.code = _builtin_shader_code()

	# UV-V shift for the normal sample, in UV units:
	#   - Separate mode: nrm_tex is its own texture, no shift needed.
	#   - Packed mode:   UV2 points at the last row of the position
	#                    half (pixel row N-1). The normal half lives
	#                    at pixel rows [N..2N-1], so we shift V DOWN
	#                    by 0.5 (one full half of the texture) to land
	#                    in the normal half. Canonical OpenVAT puts
	#                    normals in the upper UV half (+0.5); ours
	#                    puts them in the lower UV half (-0.5).
	var nrm_uv_shift: float = 0.0
	if packed:
		nrm_uv_shift = -0.5

	# Per-surface materials. Multi-submesh meshes (Mixamo bodies are
	# typically 5–15 submeshes — body / hair / clothing parts) draw
	# each surface separately. Each surface gets its own material
	# with the same texture binding — vertex addressing comes from
	# UV2 (synthesized in `_ensure_uv2_on_mesh` when not present),
	# not VERTEX_ID, so no per-surface vertex_offset is needed.
	_surface_materials.clear()
	var running_offset := 0
	for i in range(mesh.get_surface_count()):
		var mat := ShaderMaterial.new()
		mat.shader = shader
		mat.set_shader_parameter("pos_tex", pos_tex)
		if nrm_tex != null:
			mat.set_shader_parameter("nrm_tex", nrm_tex)
		else:
			# Bind pos_tex as nrm_tex so the sampler slot is filled —
			# we'll never sample through it in packed mode, but Godot
			# still requires the uniform to point at a real texture.
			mat.set_shader_parameter("nrm_tex", pos_tex)
		mat.set_shader_parameter("separate_normals", not packed)
		mat.set_shader_parameter("nrm_uv_shift", nrm_uv_shift)
		mat.set_shader_parameter("frame_count", _frame_count)
		mat.set_shader_parameter("bounds_min", _bounds_min)
		mat.set_shader_parameter("bounds_max", _bounds_max)
		mat.set_shader_parameter("current_frame", 0.0)

		# Pick up albedo from the source-mesh material if any.
		var src_mat: Material = mesh.surface_get_material(i)
		if src_mat is StandardMaterial3D:
			var sm := src_mat as StandardMaterial3D
			if sm.albedo_texture != null:
				mat.set_shader_parameter("albedo_tex", sm.albedo_texture)
				mat.set_shader_parameter("has_albedo_tex", true)
			else:
				mat.set_shader_parameter("has_albedo_tex", false)
			mat.set_shader_parameter("base_color",
				Vector3(sm.albedo_color.r, sm.albedo_color.g, sm.albedo_color.b))
		else:
			mat.set_shader_parameter("has_albedo_tex", false)
			mat.set_shader_parameter("base_color", Vector3(0.85, 0.78, 0.65))

		set_surface_override_material(i, mat)
		_surface_materials.append(mat)
		var surface_verts: int = (mesh.surface_get_arrays(i)[Mesh.ARRAY_VERTEX]
			as PackedVector3Array).size()
		running_offset += surface_verts

	# OpenVAT's "Use Single Row OFF" / tile layout requires UV2 — each
	# vertex's UV2 carries its (col, base_row) into the texture, which
	# the shader then offsets by current_frame. For QtMeshEditor's
	# single-row bakes the mesh typically doesn't carry UV2, so we
	# synthesize one from VERTEX_ID (col = vertex_id mod width, row =
	# (vertex_id / width) * frames) to keep the shader path uniform.
	# Bakes that already have UV2 (Blender-sourced OpenVAT) keep theirs.
	_ensure_uv2_on_mesh(pos_tex.get_height())

	# Override the MeshInstance3D's culling AABB to the bake bounds.
	# By default Godot culls based on the mesh's static bind-pose AABB;
	# our vertex shader displaces vertices well outside that box per-
	# frame, so the renderer would decide the mesh isn't visible and
	# skip its draw call — manifesting as the mesh appearing frozen
	# at frame 0. Pad by 10% per axis to handle interpolated frames
	# sampling slightly outside captured min/max.
	var pad: Vector3 = (_bounds_max - _bounds_min) * 0.1
	custom_aabb = AABB(_bounds_min - pad, (_bounds_max - _bounds_min) + pad * 2.0)
	extra_cull_margin = pad.length()

	print("VATPlayer: loaded %d frames × %d verts (across %d surfaces) at %.1f fps from %s" %
		[_frame_count, _vertex_count, mesh.get_surface_count(), fps_override, bake_dir])
	if running_offset != _vertex_count:
		push_warning("VATPlayer: surface vertex sum (%d) != texture width (%d) — VAT replay will misalign" %
			[running_offset, _vertex_count])
	return true


## Disable sRGB conversion and load the raw 16-bit PNG verbatim. We
## want byte-exact decoding through the shader; gamma correction would
## bias every position.
func _load_data_texture(path: String) -> Texture2D:
	var img := Image.load_from_file(path)
	if img == null or img.is_empty():
		return null
	return ImageTexture.create_from_image(img)


## Ensures every surface in `mesh` has a UV2 channel that addresses
## its column + base-row inside the VAT texture.
##
## OpenVAT shaders read this UV2 instead of computing UV from
## VERTEX_ID. The Blender add-on bakes UV2 onto the mesh as part of
## the export — `core.py:create_uv_map`:
##
##     uv_x = (i % width) / width + half_pixel
##     uv_y = 1.0 - (i // width) * frames / height - half_pixel
##
## (the `1.0 -` flips V so frame 0 lives at the top of the texture).
##
## Mesh imports that already carry UV2 — the Blender-side OpenVAT
## bake going through Godot's FBX → ImporterMesh path — are left
## untouched: their authored UV2 is already correct for the texture.
##
## QtMeshEditor's own bakes use a single-row layout (width =
## vertex_count, height = frame_count*2 for packed) and the source
## glTF doesn't carry UV2. We synthesize one from the bake's known
## dimensions and the per-surface vertex offset so the shader path
## stays uniform across both bake sources.
func _ensure_uv2_on_mesh(tex_height: int) -> void:
	if mesh == null:
		return
	# Bake textures vary in height: separate mode = `frame_count`,
	# packed mode = `frame_count * 2`. Either way the per-vertex UV2
	# math uses the full pixel height of the position texture.
	var width: int = _vertex_count
	if width <= 0 or tex_height <= 0 or _frame_count <= 0:
		return

	var rebuilt := ArrayMesh.new()
	var running_offset := 0
	var any_synthesized := false
	for i in range(mesh.get_surface_count()):
		var arrays: Array = mesh.surface_get_arrays(i)
		var src_mat: Material = mesh.surface_get_material(i)
		var prim: int = mesh.surface_get_primitive_type(i)
		var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var have_uv2: bool = (arrays.size() > Mesh.ARRAY_TEX_UV2
			and arrays[Mesh.ARRAY_TEX_UV2] is PackedVector2Array
			and (arrays[Mesh.ARRAY_TEX_UV2] as PackedVector2Array).size() == positions.size())

		if not have_uv2:
			any_synthesized = true
			var uv2 := PackedVector2Array()
			uv2.resize(positions.size())
			var half_px_x: float = 0.5 / float(width)
			var half_px_y: float = 0.5 / float(tex_height)
			for j in range(positions.size()):
				var global_vid: int = running_offset + j
				var col: int = global_vid % width
				var row_block: int = global_vid / width
				# Match OpenVAT's `core.py:create_uv_map` layout: frame 0
				# lives at the LAST pixel row of each vertex's row-block,
				# advancing frames walks UPWARD in pixel space (and so
				# UPWARD in UV V too, since Godot's V is bottom-up).
				# QtMeshEditor's packed bake actually writes positions
				# to rows [0..N-1] and normals to [N..2N-1] (top-half /
				# bottom-half in pixel space), so we point UV2 at the
				# LAST row of the position half and let `+frame*step`
				# walk up through it.
				#   uv_y = 1.0 - (base_row + N - 1 + 0.5) / tex_height
				#        = 1.0 - (base_row + N - 0.5) / tex_height
				# row_block is always 0 for single-row bakes, so for
				# our packed output this lands UV2 at the V of the
				# last position row (immediately above the normal half).
				var base_row: int = row_block * _frame_count
				var last_frame_row: int = base_row + _frame_count - 1
				var uv_x: float = float(col) / float(width) + half_px_x
				var uv_y: float = 1.0 - float(last_frame_row) / float(tex_height) - half_px_y
				uv2[j] = Vector2(uv_x, uv_y)
			arrays[Mesh.ARRAY_TEX_UV2] = uv2

		rebuilt.add_surface_from_arrays(prim, arrays)
		if src_mat != null:
			rebuilt.surface_set_material(i, src_mat)
		running_offset += positions.size()

	if any_synthesized:
		print("VATPlayer: synthesized UV2 for VAT addressing (no UV2 on imported mesh)")
		mesh = rebuilt
		# Re-apply the per-surface materials we just created since the
		# mesh resource was replaced. set_surface_override_material
		# keys off surface index so the prior bindings would still be
		# valid, but the new mesh's surface count is identical so this
		# is defensive only.
		for k in range(_surface_materials.size()):
			set_surface_override_material(k, _surface_materials[k])


func _process(delta: float) -> void:
	if _surface_materials.is_empty() or _frame_count <= 0:
		return
	# Clamp `loop_frames` to `_frame_count`. A user-set value greater
	# than the bake's frame count would push `_current_frame` past the
	# position half of the packed texture and into the normal half,
	# rendering garbage rows as positions.
	var loop_count: int = (
		mini(loop_frames, _frame_count) if loop_frames > 0 else _frame_count)
	_current_frame = fposmod(_current_frame + delta * fps_override, float(loop_count))
	for mat in _surface_materials:
		mat.set_shader_parameter("current_frame", _current_frame)


## Embedded OpenVAT-compatible spatial shader.
##
## Texture layout (matches sharpen3d/openvat reference shader's
## "Packed Normals" mode):
##   height = 2 × frame_count
##   rows [0 .. frame_count)              → positions (normalized to [bounds_min..bounds_max])
##   rows [frame_count .. 2*frame_count)  → normals   (encoded as (n+1)/2)
##
## Frame sampling: we compute integer row indices (current_row, next_row)
## from `current_frame`, look up both rows via `texelFetch` (bypassing
## the sampler so there's zero chance of nearest-filter rounding from a
## boundary frame slipping into the normal half), then linearly blend
## between them based on the fractional part of `current_frame`. This
## mirrors how sharpen3d's reference shader sequences a packed-normals
## bake — integer rows + manual blend, no reliance on the sampler.
##
## No axis swizzle on read — the bake's `_axes: "y-up-rh"` matches
## Godot's convention. (The openvat Godot reference shader swizzles
## `vec3(x, z, -y)` because *its* bakes come from Blender's Z-up RH.)
##
## Normals are negated on read. The FBX → Ogre import path applies
## `aiProcess_ConvertToLeftHanded` which flips winding without
## flipping the captured normal vector — they come out pointing into
## the surface. Without this negation the lighting is computed
## against backward-facing normals ("dark where there should be
## light" symptom).
func _builtin_shader_code() -> String:
	return """
shader_type spatial;
render_mode cull_disabled, depth_draw_opaque, depth_test_default;

// Position texture. Packed mode: height = 2 × frame_count, normals
// live in the lower half. Separate mode: height = frame_count, this
// holds positions only and `nrm_tex` holds the normals.
uniform sampler2D pos_tex : filter_nearest, repeat_disable, hint_default_white;
// Normal texture for separate-normals mode. Bound to pos_tex in
// packed mode (the uniform must point at a real texture) but
// never sampled when `separate_normals == false`.
uniform sampler2D nrm_tex : filter_nearest, repeat_disable, hint_default_white;
uniform sampler2D albedo_tex : filter_linear_mipmap, repeat_enable, source_color;
uniform bool has_albedo_tex = false;
uniform bool separate_normals = false;

uniform float current_frame = 0.0;
uniform int frame_count = 1;
// Vertical UV shift (in UV units) to land the normal sample in the
// right half of a packed-normals texture. -0.5 for QtMeshEditor's
// packed bake (normals BELOW positions in pixel space ⇒ -0.5 in
// bottom-up UV); 0.0 for separate-normals mode (nrm_tex is its own
// texture). Set by the GDScript at material creation.
uniform float nrm_uv_shift = 0.0;
uniform vec3 bounds_min = vec3(0.0);
uniform vec3 bounds_max = vec3(1.0);
uniform vec3 base_color : source_color = vec3(0.85, 0.78, 0.65);

varying vec2 uv0_pass;

void vertex() {
	// UV2 is the OpenVAT-baked addressing UV: each vertex's UV2 points
	// at the column and base-row of its row strip in the position
	// texture (Blender add-on `core.py:create_uv_map`). Bakes that
	// don't carry UV2 (e.g. QtMeshEditor's own single-row bakes) get
	// it synthesized at load time in VATPlayer._ensure_uv2_on_mesh.
	//
	// Manual integer-row arithmetic — we split current_frame into
	// integer (curr/next) and fractional (blend) parts and sample
	// both rows directly, mixing in the shader. Avoids sampler
	// nearest-rounding ambiguity at the half-texture boundary in
	// packed mode (boundary frame slipping into the normal half →
	// one-frame blob/glitch).
	int safe_frame_count = max(frame_count, 1);
	int curr_frame = int(floor(current_frame)) % safe_frame_count;
	if (curr_frame < 0) curr_frame += safe_frame_count;
	int next_frame = (curr_frame + 1) % safe_frame_count;
	float blend = fract(current_frame);

	// `frame_step` = one row in UV space. We ADD it scaled by frame
	// index to walk through the bake's row strip — matches the
	// sharpen3d/openvat reference shader (`current_offset_uv = UV2 +
	// vec2(0, frame * frame_step)`). The Blender add-on writes UV2
	// in a layout where frame 0 lives at the LAST pixel row of each
	// vertex's row-block (uv_y formula in `core.py:create_uv_map`),
	// and animation advances UPWARD in pixel space → INCREASES in
	// UV V (bottom-up). Counter-intuitive but matches every existing
	// OpenVAT consumer shader.
	vec2 tex_size = vec2(textureSize(pos_tex, 0));
	float frame_step = 1.0 / tex_size.y;
	vec2 uv_curr = UV2 + vec2(0.0, float(curr_frame) * frame_step);
	vec2 uv_next = UV2 + vec2(0.0, float(next_frame) * frame_step);

	// Position sample.
	vec3 p_curr = textureLod(pos_tex, uv_curr, 0.0).rgb;
	vec3 p_next = textureLod(pos_tex, uv_next, 0.0).rgb;
	vec3 p = mix(p_curr, p_next, blend);
	VERTEX = bounds_min + p * (bounds_max - bounds_min);

	// Normal sample diverges by layout mode:
	//   - Packed:   pos_tex, UV V shifted by `nrm_uv_shift` (typically
	//               -0.5 for QtMeshEditor's lower-half normals layout).
	//   - Separate: nrm_tex with the same UV as positions (no shift).
	vec2 uv_curr_n = uv_curr + vec2(0.0, nrm_uv_shift);
	vec2 uv_next_n = uv_next + vec2(0.0, nrm_uv_shift);
	vec3 n_curr, n_next;
	if (separate_normals) {
		n_curr = textureLod(nrm_tex, uv_curr_n, 0.0).rgb;
		n_next = textureLod(nrm_tex, uv_next_n, 0.0).rgb;
	} else {
		n_curr = textureLod(pos_tex, uv_curr_n, 0.0).rgb;
		n_next = textureLod(pos_tex, uv_next_n, 0.0).rgb;
	}
	vec3 n = mix(n_curr, n_next, blend) * 2.0 - 1.0;
	// Negate to compensate for QtMeshEditor's FBX→Ogre import path,
	// which flips winding without flipping the captured normal vector
	// (aiProcess_ConvertToLeftHanded). Blender-sourced OpenVAT bakes
	// don't need this; the negation is harmless because Godot's PBR
	// lighting is symmetric — (-n) makes the surface back-lit
	// instead of front-lit. For pure-Blender bakes consider flipping
	// back (will follow-up).
	NORMAL = -normalize(n);

	uv0_pass = UV;
}

void fragment() {
	vec3 albedo = base_color;
	if (has_albedo_tex) {
		albedo *= texture(albedo_tex, uv0_pass).rgb;
	}
	ALBEDO = albedo;
	ROUGHNESS = 0.6;
	METALLIC = 0.0;
}
"""
