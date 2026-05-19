## Main scene controller — bookkeeping for the side-by-side test rig.
##
## Two MeshInstance3D children:
##   - $Ground/Skeletal — original glTF with AnimationPlayer (live ground truth).
##   - $Ground/VAT      — same glTF mesh, driven by VATPlayer.gd via the
##                        position texture.
##
## Both should look identical in motion. Any drift = bake bug.

extends Node3D

@onready var skeletal_root: Node3D = $Ground/Skeletal
# Duck-typed instead of `VATPlayer` so Main.gd doesn't depend on
# class_name resolution order at parse time (Godot 4 occasionally
# fails to resolve cross-script class_name on first load when both
# scripts are referenced from a single .tscn).
@onready var vat_player: MeshInstance3D = $Ground/VAT
@onready var status_label: Label = $UI/Status
@onready var pause_button: Button = $UI/Controls/PauseButton
@onready var encoding_label: Label = $UI/Encoding

var _paused: bool = false


func _ready() -> void:
	# SkeletalLoader on the left node handles its own glTF load +
	# AnimationPlayer start. We just hook up the shared pause button.
	pause_button.pressed.connect(_on_pause_pressed)
	_update_status()


func _process(_delta: float) -> void:
	_update_status()


func _update_status() -> void:
	if vat_player == null:
		return
	var cf := 0.0
	if not vat_player._surface_materials.is_empty():
		cf = float(vat_player._surface_materials[0].get_shader_parameter("current_frame"))
	status_label.text = "Frame %.1f / %d   FPS %.1f" % [
		cf, vat_player._frame_count, vat_player._fps]


func _on_pause_pressed() -> void:
	_paused = not _paused
	# Walk under Skeletal to find any AnimationPlayer (instantiated
	# at runtime by SkeletalLoader after _ready, so we resolve it on
	# demand rather than caching at _ready time).
	for ap in _find_animation_players(skeletal_root):
		ap.speed_scale = 0.0 if _paused else 1.0
	vat_player.set_process(not _paused)
	pause_button.text = "Resume" if _paused else "Pause"


func _find_animation_players(root: Node) -> Array[AnimationPlayer]:
	var out: Array[AnimationPlayer] = []
	if root is AnimationPlayer:
		out.append(root as AnimationPlayer)
	for c in root.get_children():
		out.append_array(_find_animation_players(c))
	return out


# Keyboard shortcuts for the demo video / manual test:
#   Space — pause/resume both
#   R     — reset VAT current_frame to 0
#   ←/→   — step VAT by ±1 frame while paused
#   1     — toggle left (live-skinned) side visibility
#   2     — toggle right (VAT) side visibility
#
# `1` is the most important key for verifying VAT actually works:
# hide the skeletal side and if the right character keeps dancing,
# it's 100% the VAT shader doing the work — no rig involved.
func _unhandled_key_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_SPACE:
				_on_pause_pressed()
			KEY_R:
				if vat_player and not vat_player._surface_materials.is_empty():
					vat_player._current_frame = 0.0
					for mat in vat_player._surface_materials:
						mat.set_shader_parameter("current_frame", 0.0)
			KEY_LEFT, KEY_RIGHT:
				if vat_player and not vat_player._surface_materials.is_empty() and _paused:
					var step := -1.0 if event.keycode == KEY_LEFT else 1.0
					vat_player._current_frame = fposmod(
						vat_player._current_frame + step,
						float(vat_player._frame_count))
					for mat in vat_player._surface_materials:
						mat.set_shader_parameter("current_frame",
							vat_player._current_frame)
			KEY_1:
				skeletal_root.visible = not skeletal_root.visible
				print("Skeletal side: %s" % ("VISIBLE" if skeletal_root.visible else "HIDDEN"))
			KEY_2:
				vat_player.visible = not vat_player.visible
				print("VAT side: %s" % ("VISIBLE" if vat_player.visible else "HIDDEN"))
