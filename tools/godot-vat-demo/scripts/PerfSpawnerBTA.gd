## Spawns `instance_count` dancers driven by a BONE TEXTURE (BTA, QtMeshEditor
## #1166) through a single MultiMeshInstance3D — the skeletal counterpart of
## PerfSpawnerVAT.
##
## What is shared across the crowd: one mesh (bind pose, 4 bone indices +
## weights per vertex in CUSTOM0/CUSTOM1), one 210x72 float texture of
## per-frame skinning matrices (70 bones x 72 frames, ~240 KB), one
## ShaderMaterial. Per instance: a transform and INSTANCE_CUSTOM =
## (clip, time offset, speed, crossfade). Nothing is ticked on the CPU per
## instance — the only per-frame CPU work is one shared `time_seconds`
## uniform. The vertex shader does a 4-bone linear blend skin from the
## texture (12 texel fetches per vertex at frame boundaries, 24 when
## interpolating between frames).
##
## Files come straight from
##   qtmesh vat "Rumba Dancing.fbx" --mode bone-texture --target godot -o assets/RumbaBones
## (Rumba.glb + Rumba_bones.exr/json + bone_texture.gdshader + BoneTextureMesh.gd).

extends Node3D

const BoneTextureMesh = preload("res://assets/RumbaBones/BoneTextureMesh.gd")

@export var instance_count: int = 1000
@export var grid_spacing: float = 1.6
@export_file("*.glb") var source_glb: String = "res://assets/RumbaBones/Rumba.glb"
@export_file("*.json") var sidecar_path: String = "res://assets/RumbaBones/Rumba_bones.json"
@export_file("*.exr") var bone_texture: String = "res://assets/RumbaBones/Rumba_bones.exr"
@export_file("*.gdshader") var shader_path: String = "res://assets/RumbaBones/bone_texture.gdshader"
## The BTA bake comes from a current QtMeshEditor (characters face +Z, the
## glTF convention); the May-2026 VAT/skeletal assets were exported before
## the importer stopped mirroring Z (#977) and face -Z, which is why their
## spawners turn every dancer by 180 degrees and this one does not.
@export var base_yaw_deg: float = 0.0

var _material: ShaderMaterial
var _time: float = 0.0


func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	instance_count = DemoConfig.instance_count(instance_count)
	DemoConfig.frame_crowd(self, instance_count, grid_spacing)
	var start_ms := Time.get_ticks_msec()

	var sidecar := BoneTextureMesh.load_sidecar(sidecar_path)
	if sidecar.is_empty():
		DemoConfig.setup_failed(self, "cannot read %s" % sidecar_path)
		return
	var pack := load(source_glb) as PackedScene
	if pack == null:
		DemoConfig.setup_failed(self, "load('%s') failed" % source_glb)
		return
	var scene := pack.instantiate()
	var mesh := BoneTextureMesh.build_mesh(scene, sidecar)
	scene.free()
	if mesh == null:
		DemoConfig.setup_failed(self, "no skinned mesh in %s" % source_glb)
		return
	var tex := load(bone_texture) as Texture2D
	if tex == null:
		DemoConfig.setup_failed(self, "load('%s') failed" % bone_texture)
		return
	_material = BoneTextureMesh.make_material(load(shader_path) as Shader, tex, sidecar, mesh)
	BoneTextureMesh.apply_material(mesh, _material)

	var duration := BoneTextureMesh.clip_duration(sidecar, 0)
	var mm := MultiMesh.new()
	mm.transform_format = MultiMesh.TRANSFORM_3D
	mm.use_custom_data = true
	mm.mesh = mesh
	mm.instance_count = instance_count

	var side := int(ceil(sqrt(instance_count)))
	var half := side / 2
	var rng := RandomNumberGenerator.new()
	rng.seed = 1
	var built := 0
	for x in range(side):
		for z in range(side):
			if built >= instance_count:
				break
			var basis := Basis().rotated(Vector3.UP, deg_to_rad(base_yaw_deg + rng.randf_range(-30, 30)))
			var origin := Vector3(
				(x - half) * grid_spacing + rng.randf_range(-0.2, 0.2),
				0,
				(z - half) * grid_spacing + rng.randf_range(-0.2, 0.2))
			mm.set_instance_transform(built, Transform3D(basis, origin))
			# (clip 0, random time offset so the crowd is desynchronised,
			#  speed 1, no crossfade)
			mm.set_instance_custom_data(built,
				Color(0.0, rng.randf_range(0.0, duration), 1.0, 0.0))
			built += 1
		if built >= instance_count:
			break

	var mmi := MultiMeshInstance3D.new()
	mmi.multimesh = mm
	# Bind-pose AABB does not cover the animated pose; same override as VAT.
	mmi.custom_aabb = AABB(Vector3(-1000, -1000, -1000), Vector3(2000, 2000, 2000))
	add_child(mmi)
	print("PerfSpawnerBTA: spawned %d instances in %d ms (%d bones, texture %dx%d)" %
		[instance_count, Time.get_ticks_msec() - start_ms,
		 (sidecar["bones"] as Array).size(), tex.get_width(), tex.get_height()])


func _process(delta: float) -> void:
	if _material == null:
		return
	_time += delta
	_material.set_shader_parameter("time_seconds", _time)
