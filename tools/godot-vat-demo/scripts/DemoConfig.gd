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


## Pull the orbit camera back so the whole `count`-instance grid is in view
## (frustum-culled skeletal instances would otherwise make the comparison
## unfair at large counts).
static func frame_crowd(node: Node, count: int, spacing: float) -> void:
	var cam := node.get_viewport().get_camera_3d()
	if cam == null or not ("distance" in cam):
		return
	var side := ceilf(sqrt(float(count)))
	var d := maxf(12.0, side * spacing * 0.95)
	cam.set("max_distance", maxf(float(cam.get("max_distance")), d * 1.5))
	cam.set("distance", d)
	if cam.has_method("_apply_orbit"):
		cam.call("_apply_orbit")
