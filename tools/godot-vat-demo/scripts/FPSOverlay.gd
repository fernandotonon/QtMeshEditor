## FPS + frame-time overlay for the perf comparison demos.
##
## Drop as a Label child of a CanvasLayer. Updates 4×/sec — fast enough
## to feel responsive, slow enough that the readout is actually
## readable (60 Hz updates make the numbers a blur). Also tracks the
## rolling min FPS over the last second to surface frame spikes.

extends Label

@export var caption: String = ""  ## "{n}" is replaced by the instance count.
@export var instance_count: int = -1  ## -1 = don't display.
@export var bench_label: String = ""  ## Path id printed by the benchmark line.

var _bench_seconds: float = 0.0
var _bench_start_us: int = 0
var _bench_last_us: int = 0
var _bench_frames: Array[float] = []
const BENCH_WARMUP := 2.0

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
	if instance_count >= 0:
		instance_count = DemoConfig.instance_count(instance_count)
	caption = caption.replace("{n}", str(instance_count))
	_bench_seconds = DemoConfig.bench_seconds()


static func _mb(bytes: int) -> String:
	return "%.1f MB" % (float(bytes) / 1048576.0)


func _bench_tick() -> void:
	# Wall-clock timestamps, not `delta`: Godot caps the process delta when
	# frames get slow, which would overstate a heavy path's FPS.
	if Engine.has_meta("demo_setup_failed"):
		return
	var now := Time.get_ticks_usec()
	if _bench_start_us == 0:
		_bench_start_us = now
		_bench_last_us = now
		return
	var elapsed := float(now - _bench_start_us) / 1e6
	var frame := float(now - _bench_last_us) / 1e6
	_bench_last_us = now
	if elapsed < BENCH_WARMUP:
		return
	_bench_frames.append(frame)
	if elapsed < BENCH_WARMUP + _bench_seconds:
		return
	var sorted := _bench_frames.duplicate()
	sorted.sort()
	var total := 0.0
	for d in sorted:
		total += d
	var mean_ms := total / sorted.size() * 1000.0
	var p99_ms: float = sorted[int(sorted.size() * 0.99)] * 1000.0
	var rs := RenderingServer
	print("BENCH " + JSON.stringify({
		"path": bench_label,
		"instances": instance_count,
		"frames": sorted.size(),
		"fps": 1000.0 / mean_ms,
		"frame_ms": mean_ms,
		"p99_ms": p99_ms,
		"draw_calls": rs.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME),
		"texture_mem": rs.get_rendering_info(RenderingServer.RENDERING_INFO_TEXTURE_MEM_USED),
		"buffer_mem": rs.get_rendering_info(RenderingServer.RENDERING_INFO_BUFFER_MEM_USED),
		"video_mem": rs.get_rendering_info(RenderingServer.RENDERING_INFO_VIDEO_MEM_USED),
		"adapter": rs.get_video_adapter_name(),
	}))
	get_tree().quit()


func _process(delta: float) -> void:
	if _bench_seconds > 0.0:
		_bench_tick()
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
		var rs := RenderingServer
		lines.append("Draw calls: %d  |  GPU textures %s  |  buffers %s" % [
			rs.get_rendering_info(RenderingServer.RENDERING_INFO_TOTAL_DRAW_CALLS_IN_FRAME),
			_mb(rs.get_rendering_info(RenderingServer.RENDERING_INFO_TEXTURE_MEM_USED)),
			_mb(rs.get_rendering_info(RenderingServer.RENDERING_INFO_BUFFER_MEM_USED))])
		text = "\n".join(lines)

		_frames_seen = 0
		_accum_time = 0.0
