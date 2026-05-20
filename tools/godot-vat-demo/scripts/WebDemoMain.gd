## WebDemoMain — scene-level controller for the website-embed demo.
##
## Restores vsync (project.godot has it OFF for the perf scenes' FPS
## ceiling measurement). The web demo wants smooth 60 Hz playback;
## uncapped FPS in a browser tab is wasted GPU work.

extends Node3D

func _ready() -> void:
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_ENABLED)
