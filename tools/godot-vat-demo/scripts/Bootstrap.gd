## Bootstrap — entry-point scene that picks the actual demo based on
## the `?scene=` URL parameter on the web build.
##
## Recognised values (case-insensitive):
##   web              — single VAT dancer + orbit camera (default)
##   perf_vat         — 1000 VAT instances + FPS overlay
##   perf_skeleton    — 1000 skeletal instances + FPS overlay
##
## Anything else falls back to `web`. Native desktop runs default to
## the same `web` route since they don't have URL params.
##
## The website embeds the three demos as three separate <iframe>s in
## the same Vite bundle:
##   demo/index.html?scene=web            (the splash demo)
##   demo/index.html?scene=perf_vat       (perf comparison — VAT)
##   demo/index.html?scene=perf_skeleton  (perf comparison — skeletal)
##
## We use a single Godot Web export to keep the WASM+pck shared
## across all three (38 MB once, not 38 MB × 3). Switching tabs
## reloads the iframe with a different `?scene=` param, which
## triggers a quick scene change in this Bootstrap.

extends Node


func _ready() -> void:
	var scene_id := _scene_id_from_url()
	var target_scene: String = {
		"web":            "res://scenes/demo_web.tscn",
		"perf_vat":       "res://scenes/demo_perf_vat.tscn",
		"perf_skeleton":  "res://scenes/demo_perf_skeleton.tscn",
	}.get(scene_id, "res://scenes/demo_web.tscn")
	# `call_deferred` so we don't change scene during _ready.
	get_tree().call_deferred("change_scene_to_file", target_scene)


func _scene_id_from_url() -> String:
	# JavaScript bridge for the URL on the web export. On native we
	# just fall through to the default.
	if not OS.has_feature("web"):
		return "web"
	if not JavaScriptBridge.eval:
		return "web"
	var raw: Variant = JavaScriptBridge.eval(
		"new URLSearchParams(window.location.search).get('scene') || ''",
		true)
	if raw == null:
		return "web"
	var s: String = str(raw).strip_edges().to_lower()
	if s.is_empty():
		return "web"
	return s
