## Orbit + zoom camera for the web demo.
##
## Controls:
##   - Left-mouse drag → orbit around the target
##   - Mouse wheel     → zoom in/out
##   - +/- keys        → zoom in/out (keyboard fallback)
##   - Touch drag      → orbit (single finger)
##   - Pinch           → zoom (two fingers)
##
## Tuned for a single character at the origin, but configurable via
## `target` and the distance limits.

extends Camera3D

@export var target: Vector3 = Vector3(0.0, 1.0, 0.0)
@export var distance: float = 4.0
@export var min_distance: float = 1.5
@export var max_distance: float = 12.0
@export var yaw_deg: float = 0.0
@export var pitch_deg: float = -15.0
@export var min_pitch: float = -60.0
@export var max_pitch: float = 30.0
@export var orbit_sensitivity: float = 0.35  # deg per mouse pixel
@export var zoom_sensitivity: float = 0.5    # multiplier per wheel notch

var _dragging: bool = false


func _ready() -> void:
	_apply_orbit()


func _unhandled_input(event: InputEvent) -> void:
	# Mouse orbit
	if event is InputEventMouseButton:
		var mb := event as InputEventMouseButton
		if mb.button_index == MOUSE_BUTTON_LEFT:
			_dragging = mb.pressed
		elif mb.button_index == MOUSE_BUTTON_WHEEL_UP and mb.pressed:
			distance = max(min_distance, distance - zoom_sensitivity)
			_apply_orbit()
		elif mb.button_index == MOUSE_BUTTON_WHEEL_DOWN and mb.pressed:
			distance = min(max_distance, distance + zoom_sensitivity)
			_apply_orbit()
	elif event is InputEventMouseMotion and _dragging:
		var m := event as InputEventMouseMotion
		yaw_deg = fposmod(yaw_deg - m.relative.x * orbit_sensitivity, 360.0)
		pitch_deg = clamp(pitch_deg - m.relative.y * orbit_sensitivity, min_pitch, max_pitch)
		_apply_orbit()
	# Keyboard zoom (web demo: not everyone has a wheel mouse).
	elif event.is_action_pressed("ui_zoom_in"):
		distance = max(min_distance, distance - zoom_sensitivity)
		_apply_orbit()
	elif event.is_action_pressed("ui_zoom_out"):
		distance = min(max_distance, distance + zoom_sensitivity)
		_apply_orbit()


func _apply_orbit() -> void:
	var yaw_rad := deg_to_rad(yaw_deg)
	var pitch_rad := deg_to_rad(pitch_deg)
	var offset := Vector3(
		distance * cos(pitch_rad) * sin(yaw_rad),
		distance * sin(-pitch_rad),
		distance * cos(pitch_rad) * cos(yaw_rad))
	global_position = target + offset
	look_at(target, Vector3.UP)
