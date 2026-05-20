## Spawns `instance_count` SKELETAL dancers in a grid for perf comparison.
##
## Each instance is a runtime-loaded glTF scene (Skeleton3D + SkinReference
## + SkinnedMeshRenderer + AnimationPlayer). The skeletal path Godot
## takes here is the same one a real game would use for skinned NPCs —
## per-instance bone-matrix upload to the GPU + per-instance animation
## tick on the CPU. With 1000 instances this gets expensive fast;
## comparing against the VAT scene shows the win.
##
## Each instance gets a random animation phase via AnimationPlayer's
## `seek()` so the crowd doesn't move in lockstep.

extends Node3D

@export var instance_count: int = 1000
@export var grid_spacing: float = 1.6
@export_file("*.gltf", "*.glb") var source_gltf: String = "res://assets/Rumba/source.gltf"
@export var animation_name: String = ""  ## "" = first clip


func _ready() -> void:
	# Load the glTF scene ONCE and instantiate from a cached PackedScene.
	# GLTFDocument.append_from_file at runtime is too slow to call 1000
	# times.
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var abs_path: String = ProjectSettings.globalize_path(source_gltf)
	if doc.append_from_file(abs_path, state) != OK:
		push_error("PerfSpawnerSkeleton: GLTFDocument failed for %s" % source_gltf)
		return
	var prototype: Node = doc.generate_scene(state)
	if prototype == null:
		push_error("PerfSpawnerSkeleton: generate_scene returned null")
		return

	# Cache the loaded scene as a PackedScene so we can instantiate
	# cheap copies. Without this, every instance would re-parse the
	# entire glTF on add_child.
	var pack := PackedScene.new()
	pack.pack(prototype)
	prototype.queue_free()

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
	print("PerfSpawnerSkeleton: spawned %d skeletal instances" % built)


func _find_animation_player(root: Node) -> AnimationPlayer:
	if root is AnimationPlayer:
		return root as AnimationPlayer
	for c in root.get_children():
		var found := _find_animation_player(c)
		if found:
			return found
	return null
