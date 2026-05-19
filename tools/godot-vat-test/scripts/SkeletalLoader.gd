## SkeletalLoader — loads the staged glTF as a runtime scene and
## drops it under this node so the left side of the test rig plays
## the original skeletal animation as ground truth.
##
## We load at runtime (via GLTFDocument) rather than embedding the
## resource so the test harness works for any baked asset — point
## the staging script at a new mesh + animation and both halves of
## the scene update without editor intervention.

class_name SkeletalLoader
extends Node3D

## Path to a `.gltf` (or `.glb`) on disk relative to the project's
## res://. Should match VATPlayer.bake_dir + "/source.gltf".
@export_file("*.gltf", "*.glb") var gltf_path: String = ""

## Auto-play the first animation found. Set to a specific name to
## play a different one.
@export var animation_name: String = ""

var _loaded_root: Node3D


func _ready() -> void:
	if gltf_path.is_empty():
		push_warning("SkeletalLoader: gltf_path is empty — left side will stay as placeholder")
		return
	# Pre-flight: surface a helpful hint when the file is missing so the
	# user knows to run bake_and_stage.sh (the typical cause is calling
	# `qtmesh vat -o <stage_dir>` directly without the glTF conversion).
	if not FileAccess.file_exists(gltf_path):
		push_error("SkeletalLoader: source glTF not found at %s. "
			"Run tools/godot-vat-test/bake_and_stage.sh to re-stage. "
			"Direct `qtmesh vat -o ...` writes textures but not the "
			"source mesh." % gltf_path)
		return
	_load_runtime()


func _load_runtime() -> void:
	# Godot 4 resolves res:// at runtime through ResourceLoader for
	# imported resources, but glTF files we drop in via the staging
	# script aren't through the editor's import pipeline yet. Use
	# GLTFDocument directly so the very first run works without the
	# editor having scanned the folder.
	var doc := GLTFDocument.new()
	var state := GLTFState.new()
	var abs_path: String = ProjectSettings.globalize_path(gltf_path)
	var err: int = doc.append_from_file(abs_path, state)
	if err != OK:
		push_error("SkeletalLoader: GLTFDocument.append_from_file('%s') failed: %d" % [abs_path, err])
		return
	var scene: Node = doc.generate_scene(state)
	if scene == null:
		push_error("SkeletalLoader: generate_scene returned null for %s" % gltf_path)
		return

	# Strip the editor placeholder once we have something real.
	for child in get_children():
		if child is MeshInstance3D and child.name == "Placeholder":
			child.queue_free()

	_loaded_root = scene as Node3D
	add_child(_loaded_root)

	# Find and start the AnimationPlayer Godot's GLTFDocument creates.
	var anim_player := _find_animation_player(_loaded_root)
	if anim_player == null:
		push_warning("SkeletalLoader: no AnimationPlayer in %s — left side will be static" % gltf_path)
		return
	var clip := animation_name
	if clip.is_empty():
		var clips := anim_player.get_animation_list()
		if clips.size() == 0:
			push_warning("SkeletalLoader: AnimationPlayer has no clips")
			return
		clip = clips[0]
	anim_player.play(clip)
	print("SkeletalLoader: loaded %s, playing '%s'" % [gltf_path, clip])


func _find_animation_player(root: Node) -> AnimationPlayer:
	if root is AnimationPlayer:
		return root as AnimationPlayer
	for c in root.get_children():
		var found := _find_animation_player(c)
		if found:
			return found
	return null
