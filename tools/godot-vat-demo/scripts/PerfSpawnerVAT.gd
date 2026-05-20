## Spawns `instance_count` VAT dancers via a SINGLE MultiMeshInstance3D
## for honest perf measurement.
##
## Why MultiMesh instead of N MeshInstance3Ds:
##   - One draw call per surface × 1000 instances (batched on the GPU)
##     instead of 1000 separate draw calls per surface. The original
##     "spawn N nodes" approach drowned in draw-call overhead and was
##     slower than the skeletal spawner — exactly backwards from what
##     VAT is supposed to demonstrate.
##   - Mesh + bake texture + ShaderMaterial loaded ONCE, shared by
##     every instance via Godot's GPU instance rendering. The
##     skeletal spawner instantiates a PackedScene 1000× and Godot's
##     SkinnedMeshRenderer still pays per-instance CPU costs (animation
##     tick + bone matrix upload), so MultiMesh + VAT crushes it.
##   - Per-instance frame phase comes from `INSTANCE_CUSTOM.x`, set
##     per-instance via `MultiMesh.set_instance_custom_data(i, color)`.
##     The shader adds it to `current_frame` so every instance dances
##     at a different point in the loop without any CPU work per frame.

extends Node3D

@export var instance_count: int = 1000
@export var grid_spacing: float = 1.6
@export_dir var bake_dir: String = "res://assets/Rumba"
@export_file("*.gltf", "*.glb") var source_gltf: String = "res://assets/Rumba/source.gltf"
@export var fps: float = 30.0

var _frame_count: int = 0
var _bounds_min: Vector3
var _bounds_max: Vector3
var _material: ShaderMaterial
var _multimesh: MultiMesh
var _start_time: float = 0.0


func _ready() -> void:
	_start_time = Time.get_ticks_msec()

	# Load the source mesh ONCE.
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	if doc.append_from_file(ProjectSettings.globalize_path(source_gltf), state) != OK:
		push_error("PerfSpawnerVAT: glTF load failed"); return
	var scene: Node = doc.generate_scene(state)
	var found_mesh: ArrayMesh = _first_mesh_in(scene)
	scene.queue_free()
	if found_mesh == null:
		push_error("PerfSpawnerVAT: no mesh in source"); return

	# Load the bake ONCE.
	var dir := DirAccess.open(bake_dir)
	if dir == null:
		push_error("PerfSpawnerVAT: cannot open bake_dir"); return
	var json_path := ""; var basename := ""
	for f in dir.get_files():
		if f.ends_with("-remap_info.json"):
			json_path = bake_dir.path_join(f)
			basename = f.substr(0, f.length() - "-remap_info.json".length())
			break
	if json_path.is_empty():
		push_error("PerfSpawnerVAT: no sidecar"); return
	var pos_path: String = bake_dir.path_join(basename + "_pos.png")
	var sidecar: Variant = JSON.parse_string(FileAccess.get_file_as_string(json_path))
	var os: Dictionary = sidecar["os-remap"]
	_frame_count = int(os["Frames"])
	var mn: Array = os["Min"]; var mx: Array = os["Max"]
	_bounds_min = Vector3(float(mn[0]), float(mn[1]), float(mn[2]))
	_bounds_max = Vector3(float(mx[0]), float(mx[1]), float(mx[2]))

	var img := Image.load_from_file(pos_path)
	var pos_tex: ImageTexture = ImageTexture.create_from_image(img)

	# Synthesize UV2 on the shared mesh (once).
	var rebuilt := ArrayMesh.new()
	var running_offset := 0
	var tex_w: int = pos_tex.get_width()
	for i in range(found_mesh.get_surface_count()):
		var arrays: Array = found_mesh.surface_get_arrays(i)
		var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var src_mat: Material = found_mesh.surface_get_material(i)
		var uv2 := PackedVector2Array()
		uv2.resize(positions.size())
		for j in range(positions.size()):
			var gvid := running_offset + j
			uv2[j] = Vector2(float(gvid % tex_w), float(gvid / tex_w))
		arrays[Mesh.ARRAY_TEX_UV2] = uv2
		rebuilt.add_surface_from_arrays(found_mesh.surface_get_primitive_type(i), arrays)
		if src_mat != null:
			rebuilt.surface_set_material(i, src_mat)
		running_offset += positions.size()

	# Shared ShaderMaterial. Per-instance phase comes from INSTANCE_CUSTOM.
	var shader := Shader.new()
	shader.code = _shader_code()
	_material = ShaderMaterial.new()
	_material.shader = shader
	_material.set_shader_parameter("pos_tex", pos_tex)
	_material.set_shader_parameter("frame_count", _frame_count)
	_material.set_shader_parameter("bounds_min", _bounds_min)
	_material.set_shader_parameter("bounds_max", _bounds_max)
	_material.set_shader_parameter("current_frame", 0.0)
	# Pick up albedo from the first surface (Mixamo Rumba uses one
	# texture across all 11 submeshes — Boss_diffuse.png).
	for i in range(rebuilt.get_surface_count()):
		var sm := rebuilt.surface_get_material(i)
		if sm is StandardMaterial3D and (sm as StandardMaterial3D).albedo_texture != null:
			_material.set_shader_parameter("albedo_tex", (sm as StandardMaterial3D).albedo_texture)
			_material.set_shader_parameter("has_albedo_tex", true)
			break

	# Apply the shared material to every surface of the shared mesh.
	# MultiMesh uses the mesh's own materials (or surface_override if
	# set on the MultiMeshInstance3D node — but per-surface override
	# isn't how MultiMesh works; we set them on the mesh resource).
	for i in range(rebuilt.get_surface_count()):
		rebuilt.surface_set_material(i, _material)

	# Build the MultiMesh.
	_multimesh = MultiMesh.new()
	_multimesh.transform_format = MultiMesh.TRANSFORM_3D
	_multimesh.use_custom_data = true
	_multimesh.mesh = rebuilt
	_multimesh.instance_count = instance_count

	# Lay out the crowd.
	var side := int(ceil(sqrt(instance_count)))
	var half := side / 2
	var rng := RandomNumberGenerator.new()
	rng.seed = 1
	var built := 0
	for x in range(side):
		for z in range(side):
			if built >= instance_count: break
			# 180° face-camera + a bit of random spin so the crowd looks organic.
			var basis := Basis().rotated(Vector3.UP, PI + deg_to_rad(rng.randf_range(-30, 30)))
			var origin := Vector3(
				(x - half) * grid_spacing + rng.randf_range(-0.2, 0.2),
				0,
				(z - half) * grid_spacing + rng.randf_range(-0.2, 0.2))
			_multimesh.set_instance_transform(built, Transform3D(basis, origin))
			# INSTANCE_CUSTOM.r carries this instance's frame-phase
			# offset (0..frame_count). The shader adds it to
			# current_frame and wraps. Random per instance so the
			# crowd is desynchronised — VAT's worst case (every vert
			# of every instance hits a different texel; no texture-
			# cache reuse).
			var phase := rng.randf_range(0.0, float(_frame_count))
			_multimesh.set_instance_custom_data(built,
				Color(phase, 0.0, 0.0, 0.0))
			built += 1
		if built >= instance_count: break

	# Wrap the MultiMesh in a MultiMeshInstance3D for the scene tree.
	var mmi := MultiMeshInstance3D.new()
	mmi.multimesh = _multimesh
	# Override culling so the GPU doesn't skip the batch when one
	# instance's bind-pose AABB happens to leave the camera frustum
	# while VAT pushes its vertices into view.
	var pad: Vector3 = (_bounds_max - _bounds_min) * 0.1
	mmi.custom_aabb = AABB(
		Vector3(-1000, -1000, -1000), Vector3(2000, 2000, 2000))
	add_child(mmi)

	var elapsed_ms := Time.get_ticks_msec() - _start_time
	print("PerfSpawnerVAT: spawned %d instances in %d ms (one MultiMesh draw call per surface)" %
		[instance_count, elapsed_ms])


func _process(delta: float) -> void:
	if _material == null or _frame_count <= 0: return
	# Single shared uniform tick. Per-instance phase = current_frame +
	# INSTANCE_CUSTOM.r, wrapped in the shader.
	var current_frame: float = fposmod(
		_material.get_shader_parameter("current_frame") + delta * fps,
		float(_frame_count))
	_material.set_shader_parameter("current_frame", current_frame)


func _first_mesh_in(root: Node) -> ArrayMesh:
	if root is MeshInstance3D:
		var m := (root as MeshInstance3D).mesh
		if m is ArrayMesh: return m as ArrayMesh
	for c in root.get_children():
		var f := _first_mesh_in(c)
		if f: return f
	return null


## Inline shader — same texelFetch path as VATInstance.gd's
## `_shader_code`, plus `INSTANCE_CUSTOM.r` is added to `current_frame`
## for per-instance phase. INSTANCE_CUSTOM is Godot's built-in 4-float
## per-instance attribute, available in MultiMesh + SkinnedMeshRenderer
## vertex shaders.
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
	int col = int(UV2.x);
	int row_block = int(UV2.y);
	int N = max(frame_count, 1);

	// Per-instance frame phase — randomised at spawn time, stored
	// in INSTANCE_CUSTOM.r. Without it the entire crowd dances in
	// lockstep, which is visually unrealistic AND lets the GPU's
	// texture cache re-use the same texels across the batch (so
	// the bench would understate VAT's typical cost).
	float instance_phase = INSTANCE_CUSTOM.r;
	float frame_f = current_frame + instance_phase;
	int curr = int(floor(frame_f));
	curr = ((curr % N) + N) % N;
	int next = (curr + 1) % N;
	float blend = fract(frame_f);

	int base_row = row_block * N;
	vec3 p_curr = texelFetch(pos_tex, ivec2(col, base_row + curr), 0).rgb;
	vec3 p_next = texelFetch(pos_tex, ivec2(col, base_row + next), 0).rgb;
	vec3 p = mix(p_curr, p_next, blend);
	VERTEX = bounds_min + p * (bounds_max - bounds_min);

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
