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

	# Web builds: the glTF was imported by Godot's editor and lives in
	# the pck as a compiled .scn PackedScene resource (no raw .gltf
	# bytes for GLTFDocument to parse at runtime). Native desktop
	# builds: the same load() path works because Godot resolves
	# `res://` through the resource pipeline uniformly.
	#
	# We INTENTIONALLY don't use GLTFDocument.append_from_file here —
	# `globalize_path()` returns an absolute filesystem path that has
	# no meaning in a web sandbox, and `append_from_file` on a
	# `res://` path fails in web builds because the .gltf isn't
	# exported as a file (it's the .scn that ships).
	var pack: Resource = load(source_gltf)
	if pack == null:
		push_error("VATInstance: load('%s') returned null" % source_gltf)
		return false
	var scene: Node = null
	if pack is PackedScene:
		scene = (pack as PackedScene).instantiate()
	elif pack is Mesh:
		# Some imports surface as a bare Mesh resource.
		mesh = pack as Mesh
		skeleton = NodePath("")
		return true
	else:
		push_error("VATInstance: source '%s' loaded as %s, expected PackedScene or Mesh" %
			[source_gltf, pack.get_class()])
		return false
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

	var sidecar: Variant = JSON.parse_string(FileAccess.get_file_as_string(json_path))
	if typeof(sidecar) != TYPE_DICTIONARY or not sidecar.has("os-remap"):
		return false
	var os: Dictionary = sidecar["os-remap"]
	_frame_count = int(os["Frames"])
	var mn: Array = os["Min"]; var mx: Array = os["Max"]
	_bounds_min = Vector3(float(mn[0]), float(mn[1]), float(mn[2]))
	_bounds_max = Vector3(float(mx[0]), float(mx[1]), float(mx[2]))

	# Load the bake texture via the resource pipeline. Native desktop
	# AND web builds both work — Godot's editor imports PNG as a
	# CompressedTexture2D resource that lives at `<path>.ctex` in the
	# pck. `load("res://....png")` resolves through the import map.
	# We can't use `Image.load_from_file(pos_path)` in web builds:
	# the raw PNG bytes aren't shipped in the pck (only the imported
	# .ctex is), so FileAccess.file_exists() returns false.
	#
	# IMPORTANT: the bake PNG MUST have its editor import settings
	# tweaked (sRGB OFF, Filter Nearest, Compression Lossless, Mipmaps
	# OFF). Without this, Godot compresses the texture and corrupts
	# the per-vertex position data. The demo project's import
	# overrides handle this; see assets/Rumba/mixamo.com_pos.png.import.
	var pos_tex: Texture2D = load(pos_path) as Texture2D
	if pos_tex == null:
		push_error("VATInstance: load bake texture failed: %s" % pos_path)
		return false

	# Synthesize UV2 if absent — QtMeshEditor bakes don't carry it.
	_ensure_uv2_on_mesh(pos_tex.get_height(), pos_tex.get_width())

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


func _ensure_uv2_on_mesh(tex_height: int, tex_width: int) -> void:
	# UV2 encodes integer (column, row_block) per vertex, NOT a true UV.
	# The shader uses `texelFetch` with explicit pixel coordinates, so
	# there's no floating-point boundary on which row a given frame
	# samples — every frame lands inside a single texel by construction.
	#
	#   UV2.x = global_vid % tex_width      (texture column)
	#   UV2.y = global_vid / tex_width      (row-block index; always 0
	#                                        for single-row bakes)
	#
	# This is different from the canonical OpenVAT shader, which uses
	# proper [0,1] UVs + textureLod. That sampler-based path glitches
	# at half-pixel boundaries when frame_count exactly bisects the
	# texture height (our 71-frame × 142-row layout puts frame 0 on
	# the V=0.5035 boundary, where filter_nearest rounding is hardware-
	# dependent — half-up on Apple Silicon Metal lands a position
	# sample in the normal half = "egg of triangles" silhouette).
	# texelFetch sidesteps the issue entirely.
	if mesh == null or _frame_count <= 0 or tex_width <= 0: return
	var rebuilt := ArrayMesh.new()
	var any_synth := false
	var running_offset := 0
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
			for j in range(positions.size()):
				var global_vid := running_offset + j
				var col := global_vid % tex_width
				var row_block := global_vid / tex_width
				uv2[j] = Vector2(float(col), float(row_block))
			arrays[Mesh.ARRAY_TEX_UV2] = uv2
		rebuilt.add_surface_from_arrays(prim, arrays)
		if src_mat != null:
			rebuilt.surface_set_material(i, src_mat)
		running_offset += positions.size()
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
	// UV2 holds INTEGER (column, row_block) per vertex — not a true UV.
	// VATInstance._ensure_uv2_on_mesh packs them as floats but treats
	// them as integers in here. The shader uses `texelFetch` with
	// explicit pixel coordinates, sidestepping the half-pixel boundary
	// glitches that broke the UV-based sampler path.
	int col = int(UV2.x);
	int row_block = int(UV2.y);
	int N = max(frame_count, 1);

	int curr = int(floor(current_frame));
	curr = ((curr % N) + N) % N;
	int next = (curr + 1) % N;
	float blend = fract(current_frame);

	// Position rows live at pixel rows [row_block*N .. row_block*N+N-1]
	// in the top half of the texture. Pixel row 0 = top in image
	// space = bottom in OpenGL/Godot bottom-up V space → texelFetch
	// reads rows top-down from row 0, so frame `f` of a vertex in
	// row_block `b` is at pixel row `b*N + f`.
	int base_row = row_block * N;
	vec3 p_curr = texelFetch(pos_tex, ivec2(col, base_row + curr), 0).rgb;
	vec3 p_next = texelFetch(pos_tex, ivec2(col, base_row + next), 0).rgb;
	vec3 p = mix(p_curr, p_next, blend);
	VERTEX = bounds_min + p * (bounds_max - bounds_min);

	// Normals live in the BOTTOM half of the texture, rows [N..2N-1]
	// (or row_block*N + N + frame for tile layouts).
	vec3 n_curr = texelFetch(pos_tex, ivec2(col, base_row + N + curr), 0).rgb;
	vec3 n_next = texelFetch(pos_tex, ivec2(col, base_row + N + next), 0).rgb;
	vec3 n = mix(n_curr, n_next, blend) * 2.0 - 1.0;
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
