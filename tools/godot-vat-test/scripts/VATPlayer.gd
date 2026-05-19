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

## Path to the glTF whose mesh geometry should drive the VAT replay.
## MUST match the vertex order the baker saw, otherwise the position
## texture's column indexing will be wrong and the result will explode.
## The staging script always sets this to `<bake_dir>/source.gltf`.
@export_file("*.gltf", "*.glb") var source_gltf: String = ""

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
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var abs_path: String = ProjectSettings.globalize_path(source_gltf)
	var err: int = doc.append_from_file(abs_path, state)
	if err != OK:
		push_error("VATPlayer: GLTFDocument.append_from_file('%s') failed: %d" %
			[abs_path, err])
		return false
	var scene: Node = doc.generate_scene(state)
	if scene == null:
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
	for c in root.get_children():
		var found := _first_mesh_in(c)
		if found:
			return found
	return null


## Walks `bake_dir` looking for an OpenVAT pair: `*-remap_info.json` and
## the matching packed PNG (`<basename>_pos.png`, height = 2 × Frames).
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
	var json_path := ""
	for f in dir.get_files():
		if f.ends_with("-remap_info.json") and json_path.is_empty():
			json_path = bake_dir.path_join(f)
			break
	if json_path.is_empty():
		push_error("VATPlayer: no *-remap_info.json sidecar in %s — " % bake_dir +
			"is this an OpenVAT bake? Try rerunning bake_and_stage.sh.")
		return false

	# Derive the basename from the picked sidecar so the PNG we load is
	# guaranteed to come from the same bake. `<basename>-remap_info.json`
	# → strip the suffix to get `<basename>`, then look for the matching
	# `<basename>_pos.png`.
	var json_file := json_path.get_file()
	var basename := json_file.substr(0, json_file.length() - "-remap_info.json".length())
	var pos_path := bake_dir.path_join(basename + "_pos.png")
	if not FileAccess.file_exists(pos_path):
		push_error("VATPlayer: sidecar %s points at basename '%s' but %s is missing" %
			[json_file, basename, pos_path])
		return false

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

	# OpenVAT's Min/Max are arrays of stringified 8-decimal-place floats
	# (e.g. ["-0.70000000", "0.00000000", "-0.60000000"]). Cast each
	# element through float() — Godot is lenient about whether the
	# array entry is a string or a number, but we don't depend on that.
	var min_arr: Array = os_remap["Min"]
	var max_arr: Array = os_remap["Max"]
	if min_arr.size() < 3 or max_arr.size() < 3:
		push_error("VATPlayer: Min/Max arrays must have at least 3 elements")
		return false
	_bounds_min = Vector3(float(min_arr[0]), float(min_arr[1]), float(min_arr[2]))
	_bounds_max = Vector3(float(max_arr[0]), float(max_arr[1]), float(max_arr[2]))

	var pos_tex: Texture2D = _load_data_texture(pos_path)
	if pos_tex == null:
		push_error("VATPlayer: failed to load %s" % pos_path)
		return false

	# Sanity: image height should be 2 × frames.
	var img_h: int = pos_tex.get_height()
	if img_h != _frame_count * 2:
		push_warning("VATPlayer: texture height %d != 2 × frames (%d) — " %
			[img_h, _frame_count * 2] +
			"is this really an OpenVAT bake?")
	_vertex_count = pos_tex.get_width()

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
## We compute V for the position half as:
##   v_pos = (current_frame + 0.5) / (2 * frame_count)
## and for the normal half by shifting half a texture down:
##   v_nrm = v_pos + 0.5
##
## No axis swizzle on read — the bake's `_axes: "y-up-rh"` matches
## Godot's convention. (The openvat Godot reference shader swizzles
## `vec3(x, z, -y)` because *its* bakes come from Blender's Z-up RH.)
func _builtin_shader_code() -> String:
	return """
shader_type spatial;
render_mode cull_disabled, depth_draw_opaque, depth_test_default;

uniform sampler2D pos_tex : filter_nearest, repeat_disable, hint_default_white;
uniform sampler2D albedo_tex : filter_linear_mipmap, repeat_enable, source_color;
uniform bool has_albedo_tex = false;

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
	float u = (float(global_vid) + 0.5) / float(vertex_count);
	// Total texture height is 2 × frame_count; position rows live in
	// the upper half (frame_count rows), normal rows live in the
	// lower half (frame_count rows).
	float v_pos = (current_frame + 0.5) / float(frame_count * 2);
	float v_nrm = v_pos + 0.5;

	vec3 t = texture(pos_tex, vec2(u, v_pos)).rgb;
	VERTEX = bounds_min + t * (bounds_max - bounds_min);

	vec3 n = texture(pos_tex, vec2(u, v_nrm)).rgb * 2.0 - 1.0;
	NORMAL = normalize(n);

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
