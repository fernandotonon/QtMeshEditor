## VATInstance — drop-in MeshInstance3D that plays back an OpenVAT bake.
##
## Streamlined version of `tools/godot-vat-test/scripts/VATPlayer.gd` —
## same shader, but loads the bake once at startup and assumes the
## QtMeshEditor packed-normals layout (height = 2 × Frames). The demo
## scenes use this to spawn many instances cheaply.
##
## Usage in the scene: set `bake_dir` and `source_gltf`, and the rest
## is automatic.

class_name VATInstance
extends MeshInstance3D

## Directory containing `<name>_pos.png` + `<name>-remap_info.json`.
@export_dir var bake_dir: String = ""

## glTF whose mesh geometry drives the VAT replay. Must match the
## vertex order the baker saw.
@export_file("*.gltf", "*.glb") var source_gltf: String = ""

## Playback FPS — OpenVAT's sidecar doesn't carry it.
@export var fps: float = 30.0

## When true, the player drives `current_frame` itself every _process.
## Set to false if a parent (e.g. a perf demo) wants to drive all
## instances from a single shared float (saves N material updates per
## frame for big crowds).
@export var self_driven: bool = true

var _frame_count: int = 0
var _bounds_min: Vector3
var _bounds_max: Vector3
var _surface_materials: Array[ShaderMaterial] = []
var _current_frame: float = 0.0


func _ready() -> void:
	if not _load_source_mesh():
		push_error("VATInstance: load source mesh from %s failed" % source_gltf)
		return
	if not _load_bake():
		push_error("VATInstance: load bake from %s failed" % bake_dir)


func set_current_frame(f: float) -> void:
	## External entry point for shared-clock perf demos.
	_current_frame = f
	for mat in _surface_materials:
		mat.set_shader_parameter("current_frame", f)


func _load_source_mesh() -> bool:
	if source_gltf.is_empty(): return false
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	if doc.append_from_file(ProjectSettings.globalize_path(source_gltf), state) != OK:
		return false
	var scene := doc.generate_scene(state)
	if scene == null: return false
	var found: ArrayMesh = _first_array_mesh(scene)
	scene.queue_free()
	if found == null: return false
	mesh = found
	skeleton = NodePath("")
	return true


func _first_array_mesh(root: Node) -> ArrayMesh:
	if root is MeshInstance3D:
		var m := (root as MeshInstance3D).mesh
		if m is ArrayMesh: return m as ArrayMesh
	for c in root.get_children():
		var f := _first_array_mesh(c)
		if f: return f
	return null


func _load_bake() -> bool:
	var dir := DirAccess.open(bake_dir)
	if dir == null: return false
	var json_path := ""
	var basename := ""
	for f in dir.get_files():
		if f.ends_with("-remap_info.json"):
			json_path = bake_dir.path_join(f)
			basename = f.substr(0, f.length() - "-remap_info.json".length())
			break
	if json_path.is_empty(): return false

	var pos_path := bake_dir.path_join(basename + "_pos.png")
	if not FileAccess.file_exists(pos_path): return false

	var sidecar: Variant = JSON.parse_string(FileAccess.get_file_as_string(json_path))
	if typeof(sidecar) != TYPE_DICTIONARY or not sidecar.has("os-remap"):
		return false
	var os: Dictionary = sidecar["os-remap"]
	_frame_count = int(os["Frames"])
	var mn: Array = os["Min"]; var mx: Array = os["Max"]
	_bounds_min = Vector3(float(mn[0]), float(mn[1]), float(mn[2]))
	_bounds_max = Vector3(float(mx[0]), float(mx[1]), float(mx[2]))

	var img := Image.load_from_file(pos_path)
	if img == null or img.is_empty(): return false
	var pos_tex: ImageTexture = ImageTexture.create_from_image(img)

	# Synthesize UV2 if absent — QtMeshEditor bakes don't carry it.
	_ensure_uv2_on_mesh(pos_tex.get_height())

	var shader := Shader.new()
	shader.code = _shader_code()

	_surface_materials.clear()
	for i in range(mesh.get_surface_count()):
		var mat := ShaderMaterial.new()
		mat.shader = shader
		mat.set_shader_parameter("pos_tex", pos_tex)
		mat.set_shader_parameter("frame_count", _frame_count)
		mat.set_shader_parameter("bounds_min", _bounds_min)
		mat.set_shader_parameter("bounds_max", _bounds_max)
		mat.set_shader_parameter("current_frame", 0.0)
		# Pick up the source mesh's albedo if any.
		var src_mat: Material = mesh.surface_get_material(i)
		if src_mat is StandardMaterial3D and (src_mat as StandardMaterial3D).albedo_texture != null:
			mat.set_shader_parameter("albedo_tex", (src_mat as StandardMaterial3D).albedo_texture)
			mat.set_shader_parameter("has_albedo_tex", true)
		set_surface_override_material(i, mat)
		_surface_materials.append(mat)

	# Expand bounding box so culling doesn't skip displaced verts.
	var pad: Vector3 = (_bounds_max - _bounds_min) * 0.1
	custom_aabb = AABB(_bounds_min - pad, (_bounds_max - _bounds_min) + pad * 2.0)
	return true


func _ensure_uv2_on_mesh(tex_height: int) -> void:
	if mesh == null or _frame_count <= 0: return
	var width: int = mesh.surface_get_arrays(0)[Mesh.ARRAY_VERTEX].size()
	var rebuilt := ArrayMesh.new()
	var any_synth := false
	for i in range(mesh.get_surface_count()):
		var arrays: Array = mesh.surface_get_arrays(i)
		var src_mat: Material = mesh.surface_get_material(i)
		var prim: int = mesh.surface_get_primitive_type(i)
		var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var have_uv2: bool = arrays.size() > Mesh.ARRAY_TEX_UV2 \
			and arrays[Mesh.ARRAY_TEX_UV2] is PackedVector2Array \
			and (arrays[Mesh.ARRAY_TEX_UV2] as PackedVector2Array).size() == positions.size()
		if not have_uv2:
			any_synth = true
			var uv2 := PackedVector2Array()
			uv2.resize(positions.size())
			var hpx := 0.5 / float(width)
			var hpy := 0.5 / float(tex_height)
			for j in range(positions.size()):
				# Single-row layout: row_block always 0. UV2 points at
				# the last position row so `+frame*step` walks up.
				var col := j % width
				var last_row := _frame_count - 1
				uv2[j] = Vector2(
					float(col) / float(width) + hpx,
					1.0 - float(last_row) / float(tex_height) - hpy)
			arrays[Mesh.ARRAY_TEX_UV2] = uv2
		rebuilt.add_surface_from_arrays(prim, arrays)
		if src_mat != null:
			rebuilt.surface_set_material(i, src_mat)
	if any_synth:
		mesh = rebuilt


func _process(delta: float) -> void:
	if not self_driven or _surface_materials.is_empty() or _frame_count <= 0:
		return
	_current_frame = fposmod(_current_frame + delta * fps, float(_frame_count))
	for mat in _surface_materials:
		mat.set_shader_parameter("current_frame", _current_frame)


func _shader_code() -> String:
	return """
shader_type spatial;
render_mode cull_disabled, depth_draw_opaque, depth_test_default;

uniform sampler2D pos_tex : filter_nearest, repeat_disable, hint_default_white;
uniform sampler2D albedo_tex : filter_linear_mipmap, repeat_enable, source_color;
uniform bool has_albedo_tex = false;
uniform float current_frame = 0.0;
uniform int frame_count = 1;
uniform vec3 bounds_min = vec3(0.0);
uniform vec3 bounds_max = vec3(1.0);
uniform vec3 base_color : source_color = vec3(0.85, 0.78, 0.65);

varying vec2 uv0_pass;

void vertex() {
	int safe_frames = max(frame_count, 1);
	int curr = int(floor(current_frame)) % safe_frames;
	if (curr < 0) curr += safe_frames;
	int next = (curr + 1) % safe_frames;
	float blend = fract(current_frame);

	vec2 tex_size = vec2(textureSize(pos_tex, 0));
	float frame_step = 1.0 / tex_size.y;
	vec2 uv_c = UV2 + vec2(0.0, float(curr) * frame_step);
	vec2 uv_n = UV2 + vec2(0.0, float(next) * frame_step);

	vec3 p_c = textureLod(pos_tex, uv_c, 0.0).rgb;
	vec3 p_n = textureLod(pos_tex, uv_n, 0.0).rgb;
	vec3 p = mix(p_c, p_n, blend);
	VERTEX = bounds_min + p * (bounds_max - bounds_min);

	vec3 n_c = textureLod(pos_tex, uv_c + vec2(0.0, -0.5), 0.0).rgb;
	vec3 n_n = textureLod(pos_tex, uv_n + vec2(0.0, -0.5), 0.0).rgb;
	vec3 n = mix(n_c, n_n, blend) * 2.0 - 1.0;
	NORMAL = -normalize(n);

	uv0_pass = UV;
}

void fragment() {
	vec3 albedo = base_color;
	if (has_albedo_tex) albedo *= texture(albedo_tex, uv0_pass).rgb;
	ALBEDO = albedo;
	ROUGHNESS = 0.6;
	METALLIC = 0.0;
}
"""
