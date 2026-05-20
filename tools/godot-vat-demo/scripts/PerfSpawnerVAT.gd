## Spawns `instance_count` VAT dancers in a grid for perf measurement.
##
## Each instance gets a random starting frame so the crowd doesn't move
## in lockstep — that's visually unrealistic AND would let the GPU
## skip a lot of redundant texture samples. With phases, every vertex
## of every instance hits a different texel, which is the worst case.

extends Node3D

# Preload via path rather than relying on `class_name` resolution —
# Godot 4 occasionally fails to resolve cross-script class_name at
# parse time when scripts are first loaded.
const VATInstanceScript := preload("res://scripts/VATInstance.gd")

@export var instance_count: int = 1000
@export var grid_spacing: float = 1.6
@export_dir var bake_dir: String = "res://assets/Rumba"
@export_file("*.gltf", "*.glb") var source_gltf: String = "res://assets/Rumba/source.gltf"


func _ready() -> void:
	var side := int(ceil(sqrt(instance_count)))
	var half := side / 2
	var rng := RandomNumberGenerator.new()
	rng.seed = 1
	var built := 0
	for x in range(side):
		for z in range(side):
			if built >= instance_count: break
			var inst: MeshInstance3D = MeshInstance3D.new()
			inst.set_script(VATInstanceScript)
			inst.bake_dir = bake_dir
			inst.source_gltf = source_gltf
			# Each instance self-drives at a random initial phase so the
			# crowd is desynchronised. Skip an explicit "current_frame"
			# initial because VATInstance starts at 0; we tweak via
			# adding a deferred offset in _process after the player has
			# loaded its frame count.
			inst.fps = 30.0
			inst.self_driven = true
			# Random phase via rotating + slight position jitter so the
			# crowd looks organic.
			inst.transform = Transform3D(
				Basis().rotated(Vector3.UP, deg_to_rad(rng.randf_range(-30, 30))).scaled(Vector3.ONE),
				Vector3(
					(x - half) * grid_spacing + rng.randf_range(-0.2, 0.2),
					0,
					(z - half) * grid_spacing + rng.randf_range(-0.2, 0.2)))
			# Rotate 180° around Y to face camera (same as the test
			# harness — Godot glTF importer's forward = -Z convention).
			inst.rotate_y(PI + deg_to_rad(rng.randf_range(-20, 20)))
			add_child(inst)
			# Stagger initial frame by 1/N of a clip per instance. We
			# can't set _current_frame directly until the bake loads,
			# so do it deferred.
			inst.call_deferred("set_current_frame", rng.randf_range(0, 70))
			built += 1
		if built >= instance_count: break
	print("PerfSpawnerVAT: spawned %d VAT instances" % built)
