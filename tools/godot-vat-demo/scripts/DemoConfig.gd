## Shared knobs for the perf scenes.
##
## Instance count: `?n=` on the web build (read by Bootstrap into the
## `demo_n` Engine meta) or `-- --n=N` on a native run; the scene's own
## value is the default. Benchmark mode: `-- --bench=SECONDS` makes the
## FPS overlay measure for that long (after a warm-up), print one JSON
## line and quit — see tools/godot-vat-demo/bench.sh.
class_name DemoConfig
extends RefCounted

const MAX_INSTANCES := 10000


static func _user_arg(key: String) -> String:
	for a in OS.get_cmdline_user_args():
		if a.begins_with("--%s=" % key):
			return a.substr(key.length() + 3)
	return ""


static func instance_count(default_count: int) -> int:
	var n := default_count
	if Engine.has_meta("demo_n"):
		n = int(Engine.get_meta("demo_n"))
	var arg := _user_arg("n")
	if not arg.is_empty():
		n = int(arg)
	return clampi(n, 1, MAX_INSTANCES)


static func bench_seconds() -> float:
	var arg := _user_arg("bench")
	return float(arg) if not arg.is_empty() else 0.0


## Report a crowd that could not be built. In benchmark mode the run exits
## with status 2 BEFORE the overlay can print a result for an empty scene.
static func setup_failed(node: Node, message: String) -> void:
	push_error("%s: %s" % [node.name, message])
	Engine.set_meta("demo_setup_failed", message)
	if bench_seconds() > 0.0:
		printerr("BENCH_SETUP_FAILED %s" % message)
		node.get_tree().quit(2)


## Pull the orbit camera back so the whole `count`-instance grid (its far
## corners included) fits the camera's narrower field of view: a skeletal
## instance outside the frustum is culled, which would flatter that path.
static func frame_crowd(node: Node, count: int, spacing: float) -> void:
	var cam := node.get_viewport().get_camera_3d()
	if cam == null or not ("distance" in cam):
		return
	var side := ceilf(sqrt(float(count)))
	# Bounding radius of the grid footprint + one dancer's height of margin.
	var radius := side * spacing * 0.5 * sqrt(2.0) + 2.0
	var vsize := node.get_viewport().get_visible_rect().size
	var aspect := vsize.x / maxf(vsize.y, 1.0)
	var half_v := deg_to_rad(cam.fov) * 0.5
	var half_h := atan(tan(half_v) * aspect)
	var half := minf(half_v, half_h)
	var d := maxf(12.0, radius / sin(half))
	cam.set("max_distance", maxf(float(cam.get("max_distance")), d * 1.5))
	cam.set("distance", d)
	if cam.has_method("_apply_orbit"):
		cam.call("_apply_orbit")
