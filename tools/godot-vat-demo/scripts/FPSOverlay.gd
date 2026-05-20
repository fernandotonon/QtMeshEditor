## FPS + frame-time overlay for the perf comparison demos.
##
## Drop as a Label child of a CanvasLayer. Updates 4×/sec — fast enough
## to feel responsive, slow enough that the readout is actually
## readable (60 Hz updates make the numbers a blur). Also tracks the
## rolling min FPS over the last second to surface frame spikes.

extends Label

@export var caption: String = ""
@export var instance_count: int = -1  ## -1 = don't display.

var _frames_seen: int = 0
var _accum_time: float = 0.0
var _min_fps: float = 9999.0
var _rolling_window: Array[float] = []  ## frame deltas over the last 1s


func _ready() -> void:
	# Make the overlay readable on any background.
	add_theme_color_override("font_color", Color(1.0, 1.0, 1.0))
	add_theme_color_override("font_outline_color", Color(0.0, 0.0, 0.0))
	add_theme_constant_override("outline_size", 4)
	add_theme_font_size_override("font_size", 18)


func _process(delta: float) -> void:
	_frames_seen += 1
	_accum_time += delta
	_rolling_window.append(delta)
	# Drop frames older than 1 second.
	while not _rolling_window.is_empty() and _rolling_window.size() > Engine.get_frames_per_second():
		_rolling_window.pop_front()

	if _accum_time >= 0.25:
		var fps := float(_frames_seen) / _accum_time
		var ms := _accum_time / float(_frames_seen) * 1000.0
		var window_max_delta := 0.0
		for d in _rolling_window:
			if d > window_max_delta:
				window_max_delta = d
		var window_min_fps := (1.0 / window_max_delta) if window_max_delta > 0 else 0.0
		_min_fps = min(_min_fps, window_min_fps)

		var lines := PackedStringArray()
		if not caption.is_empty():
			lines.append(caption)
		if instance_count >= 0:
			lines.append("Instances: %d" % instance_count)
		lines.append("FPS: %.1f  (frame %.2f ms)" % [fps, ms])
		lines.append("Min (1s window): %.1f  |  Worst since start: %.1f" %
			[window_min_fps, _min_fps])
		text = "\n".join(lines)

		_frames_seen = 0
		_accum_time = 0.0
