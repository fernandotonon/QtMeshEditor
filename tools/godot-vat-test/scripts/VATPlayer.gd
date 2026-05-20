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

	# Per-surface materials. Multi-submesh meshes (Mixamo bodies are
	# typically 5–15 submeshes — body / hair / clothing parts) draw
	# each surface separately, and Godot's `VERTEX_ID` restarts at 0
	# for each surface. We tell every surface its starting column in
	# the global packed texture via `vertex_offset`.
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
		mat.set_shader_parameter("frame_count", _frame_count)
		mat.set_shader_parameter("vertex_count", _vertex_count)
		mat.set_shader_parameter("vertex_offset", running_offset)
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
uniform int vertex_count = 1;
uniform int vertex_offset = 0;
uniform vec3 bounds_min = vec3(0.0);
uniform vec3 bounds_max = vec3(1.0);
uniform vec3 base_color : source_color = vec3(0.85, 0.78, 0.65);

varying vec2 uv0_pass;

void vertex() {
	int global_vid = vertex_offset + VERTEX_ID;

	// Manual row arithmetic — `current_frame` is continuous, but the
	// texture is row-addressed. We split into integer current/next rows
	// and a fractional blend, then sample both rows directly via
	// `texelFetch`. Avoids sampler nearest-rounding ambiguity at the
	// half-texture boundary in packed mode (rows N-1 and N are
	// physically adjacent but live in different semantic halves —
	// position vs. normal — and a sampler that rounds the wrong way
	// produces the one-frame blob/glitch).
	int safe_frame_count = max(frame_count, 1);
	int curr_row = int(floor(current_frame)) % safe_frame_count;
	if (curr_row < 0) curr_row += safe_frame_count;
	int next_row = (curr_row + 1) % safe_frame_count;
	float blend = fract(current_frame);

	// Position: rows 0..N-1 in pos_tex regardless of mode.
	vec3 p_curr = texelFetch(pos_tex, ivec2(global_vid, curr_row), 0).rgb;
	vec3 p_next = texelFetch(pos_tex, ivec2(global_vid, next_row), 0).rgb;
	vec3 p = mix(p_curr, p_next, blend);
	VERTEX = bounds_min + p * (bounds_max - bounds_min);

	// Normal sampling diverges by mode:
	//   - Packed:   pos_tex rows [N..2N-1]
	//   - Separate: nrm_tex rows [0..N-1]
	vec3 n_curr, n_next;
	if (separate_normals) {
		n_curr = texelFetch(nrm_tex, ivec2(global_vid, curr_row), 0).rgb;
		n_next = texelFetch(nrm_tex, ivec2(global_vid, next_row), 0).rgb;
	} else {
		n_curr = texelFetch(pos_tex, ivec2(global_vid, curr_row + safe_frame_count), 0).rgb;
		n_next = texelFetch(pos_tex, ivec2(global_vid, next_row + safe_frame_count), 0).rgb;
	}
	vec3 n = mix(n_curr, n_next, blend) * 2.0 - 1.0;
	// Negate to compensate for QtMeshEditor's FBX→Ogre import path,
	// which flips winding without flipping the captured normal vector
	// (aiProcess_ConvertToLeftHanded). Blender-sourced OpenVAT bakes
	// don't need this — the negation is harmless in either case since
	// (-n) just makes the surface back-lit instead of front-lit, but
	// for Blender-native bakes consider flipping back.
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
