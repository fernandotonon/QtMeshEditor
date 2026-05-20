## Spawns `instance_count` SKELETAL dancers in a grid for perf comparison.
##
## This is the "best practice" skinned-mesh setup a real Godot game
## would ship with for an NPC crowd. There's no built-in MultiMesh
## equivalent for SkinnedMeshRenderer in Godot 4 — skinning is always
## per-instance (per-instance bone-matrix upload + per-instance vertex
## skin transform). What we CAN share across instances:
##
##   - PackedScene cache: load the glTF once, instantiate cheaply
##     (otherwise GLTFDocument.append_from_file would run 1000 times).
##   - Mesh resource: every instance points at the SAME ArrayMesh
##     resource (Godot does this automatically — instantiating a
##     PackedScene reuses Mesh resources).
##   - Material resource: same — surface materials are shared by
##     reference across instances.
##   - Animation resource: a single Animation drives every clone's
##     AnimationPlayer (also automatic).
##
## What we CAN'T share:
##   - Skeleton3D's bone transforms: each instance has its own pose.
##   - SkinReference: tied to the per-instance Skeleton.
##   - AnimationPlayer state: each instance ticks its own time.
##
## Comparing this to the MultiMesh-VAT spawner is the fair real-world
## comparison: both use the best Godot-native path for their respective
## approach.

extends Node3D

@export var instance_count: int = 1000
@export var grid_spacing: float = 1.6
@export_file("*.gltf", "*.glb") var source_gltf: String = "res://assets/Rumba/source.gltf"
@export var animation_name: String = ""  ## "" = first clip


func _ready() -> void:
	# Uncap FPS for honest perf measurement (matches the VAT spawner).
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0

	var start_ms := Time.get_ticks_msec()

	# Load through the resource pipeline — Godot's editor imported
	# the .gltf as a .scn PackedScene which lives in the pck.
	# Native desktop builds + web builds both resolve uniformly via
	# `load()`. The runtime GLTFDocument.append_from_file path would
	# fail in web because the raw .gltf bytes aren't shipped.
	var pack: PackedScene = load(source_gltf) as PackedScene
	if pack == null:
		push_error("PerfSpawnerSkeleton: load('%s') returned null" % source_gltf)
		return

	var side := int(ceil(sqrt(instance_count)))
	var half := side / 2
	var rng := RandomNumberGenerator.new()
	rng.seed = 2
	var built := 0
	for x in range(side):
		for z in range(side):
			if built >= instance_count: break
			var inst: Node3D = pack.instantiate()
			inst.transform = Transform3D(
				Basis().rotated(Vector3.UP, deg_to_rad(rng.randf_range(-30, 30))).scaled(Vector3.ONE),
				Vector3(
					(x - half) * grid_spacing + rng.randf_range(-0.2, 0.2),
					0,
					(z - half) * grid_spacing + rng.randf_range(-0.2, 0.2)))
			# Match VAT spawner's facing for fair comparison.
			inst.rotate_y(PI + deg_to_rad(rng.randf_range(-20, 20)))
			add_child(inst)
			# Strip per-instance MeshInstance3D shadow casting if not
			# needed — 1000 shadow-casting skinned meshes would tank
			# fps in a way that's orthogonal to the skin-vs-VAT
			# comparison. Both perf scenes already run with shadows
			# off via the scene-level light, but instances inherit
			# `cast_shadow = SHADOW_CASTING_SETTING_ON` from the
			# imported glTF; leaving it on is the realistic default.
			var ap := _find_animation_player(inst)
			if ap != null:
				var clip := animation_name
				if clip.is_empty():
					var clips := ap.get_animation_list()
					if clips.size() > 0:
						clip = clips[0]
				if not clip.is_empty():
					var anim := ap.get_animation(clip)
					if anim != null:
						anim.loop_mode = Animation.LOOP_LINEAR
					ap.play(clip)
					# Stagger the start frame so the crowd is desynced.
					ap.seek(rng.randf_range(0.0, ap.current_animation_length), true)
			built += 1
		if built >= instance_count: break

	var elapsed_ms := Time.get_ticks_msec() - start_ms
	print("PerfSpawnerSkeleton: spawned %d skeletal instances in %d ms" %
		[built, elapsed_ms])


func _find_animation_player(root: Node) -> AnimationPlayer:
	if root is AnimationPlayer:
		return root as AnimationPlayer
	for c in root.get_children():
		var found := _find_animation_player(c)
		if found:
			return found
	return null
