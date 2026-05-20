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
@export_file("*.gltf", "*.glb", "*.fbx") var gltf_path: String = ""

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
		var msg := "SkeletalLoader: source glTF not found at %s. " % gltf_path
		msg += "Run tools/godot-vat-test/bake_and_stage.sh to re-stage. "
		msg += "Direct `qtmesh vat -o ...` writes textures but not the source mesh."
		push_error(msg)
		return
	_load_runtime()


func _load_runtime() -> void:
	# Two load paths:
	#   - glTF / glb → GLTFDocument runtime load (works for any file
	#     on disk, no editor import dance needed).
	#   - FBX → Godot 4 has no runtime FBX parser. Goes through the
	#     editor's import pipeline as a PackedScene at `res://...`,
	#     which we then `load()` + `instantiate()`. The first run on
	#     a freshly-dropped FBX may show nothing until Godot finishes
	#     importing — re-open the scene if so.
	var scene: Node = null
	var lower := gltf_path.to_lower()
	if lower.ends_with(".gltf") or lower.ends_with(".glb"):
		var doc := GLTFDocument.new()
		var state := GLTFState.new()
		var abs_path: String = ProjectSettings.globalize_path(gltf_path)
		var err: int = doc.append_from_file(abs_path, state)
		if err != OK:
			push_error("SkeletalLoader: GLTFDocument.append_from_file('%s') failed: %d" % [abs_path, err])
			return
		scene = doc.generate_scene(state)
	else:
		var pack: Resource = load(gltf_path)
		if pack == null:
			push_error("SkeletalLoader: load('%s') returned null — wait for Godot's editor import to finish, then reopen the scene" % gltf_path)
			return
		if pack is PackedScene:
			scene = (pack as PackedScene).instantiate()
		else:
			push_warning("SkeletalLoader: %s loaded as %s, expected PackedScene (no AnimationPlayer to drive — left side will be static)" %
				[gltf_path, pack.get_class()])
			return
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
	# Force the clip to loop so the live-skinned side keeps pace
	# with the VAT side (which loops indefinitely via fposmod in
	# VATPlayer._process). Without this the glTF importer's default
	# loop mode is "none" — animation plays once and freezes on the
	# last frame, leaving the VAT side dancing alone.
	var anim_lib := anim_player.get_animation(clip)
	if anim_lib != null:
		anim_lib.loop_mode = Animation.LOOP_LINEAR
	anim_player.play(clip)
	print("SkeletalLoader: loaded %s, playing '%s' (loop=LINEAR)" % [gltf_path, clip])


func _find_animation_player(root: Node) -> AnimationPlayer:
	if root is AnimationPlayer:
		return root as AnimationPlayer
	for c in root.get_children():
		var found := _find_animation_player(c)
		if found:
			return found
	return null
