## Bone texture animation (BTA) helpers for Godot 4 — QtMeshEditor #1166.
##
## Pairs with bone_texture.gdshader and the output of
##   qtmesh vat <rigged file> --mode bone-texture -o <dir>
## which writes <basename>.glb (bind pose + skin), <basename>_bones.exr and
## <basename>_bones.json.
##
## Typical use (one shared mesh + material for a whole crowd). The script has
## no `class_name` on purpose: every bake ships its own copy, and two copies
## registering the same global class would collide, so load it by path:
##
##   const BoneTextureMesh = preload("res://hero/BoneTextureMesh.gd")
##   var sidecar := BoneTextureMesh.load_sidecar("res://hero/hero_bones.json")
##   var scene := (load("res://hero/hero.glb") as PackedScene).instantiate()
##   var mesh := BoneTextureMesh.build_mesh(scene, sidecar)
##   scene.free()
##   var mat := BoneTextureMesh.make_material(
##       load("res://hero/bone_texture.gdshader"),
##       load("res://hero/hero_bones.exr"), sidecar, mesh)
##   BoneTextureMesh.apply_material(mesh, mat)
##   # MultiMesh with use_custom_data = true; per instance:
##   #   Color(clip_index, time_offset_s, speed, crossfade)
##   # and every frame: mat.set_shader_parameter("time_seconds", t)
##
## Why the mesh has to be rebuilt: a Godot spatial shader cannot read a
## skinned mesh's BONE_INDICES/BONE_WEIGHTS, and the imported skin numbers
## its joints in its own order. build_mesh() maps each skin joint to the
## texture's bone index BY NAME (exact name first; Godot sanitises names
## on import, so otherwise a normalised name, used only when it identifies
## ONE texture bone) and stores 4 indices/weights per vertex in
## CUSTOM0/CUSTOM1 (RGBA float), dropping the engine skin so the mesh can be
## instanced.
extends RefCounted


static func load_sidecar(path: String) -> Dictionary:
	var text := FileAccess.get_file_as_string(path)
	if text.is_empty():
		push_error("BoneTextureMesh: cannot read %s" % path)
		return {}
	var data: Variant = JSON.parse_string(text)
	if not (data is Dictionary) or (data as Dictionary).get("schema", "") != "qtmesh-bone-texture-v1":
		push_error("BoneTextureMesh: %s is not a qtmesh-bone-texture-v1 sidecar" % path)
		return {}
	return data


## Index of a clip by name (-1 when absent).
static func clip_index(sidecar: Dictionary, clip_name: String) -> int:
	var clips: Array = sidecar.get("clips", [])
	for i in range(clips.size()):
		if str((clips[i] as Dictionary).get("name", "")) == clip_name:
			return i
	return -1


## Duration in seconds of clip `index`.
static func clip_duration(sidecar: Dictionary, index: int) -> float:
	var clips: Array = sidecar.get("clips", [])
	if index < 0 or index >= clips.size():
		return 0.0
	return float((clips[index] as Dictionary).get("duration", 0.0))


static func _name_key(n: String) -> String:
	var out := ""
	for ch in n.to_lower():
		out += ch if (ch >= "a" and ch <= "z") or (ch >= "0" and ch <= "9") else "_"
	return out


static func _find_skinned(root: Node, out: Array) -> void:
	if root is MeshInstance3D and (root as MeshInstance3D).mesh != null \
			and (root as MeshInstance3D).skin != null:
		out.append(root)
	for c in root.get_children():
		_find_skinned(c, out)


## Build the instanced bone-texture mesh from an imported skinned scene.
## Every skinned MeshInstance3D's surfaces are merged into one ArrayMesh
## (their materials kept). Returns null when nothing usable was found.
static func build_mesh(scene_root: Node, sidecar: Dictionary) -> ArrayMesh:
	# Exact names first; a normalised key only when exactly one texture bone
	# produces it ("Arm" and "arm" both normalise to "arm" - ambiguous).
	var bone_of_name := {}
	var bones_of_key := {}
	var bones: Array = sidecar.get("bones", [])
	for i in range(bones.size()):
		var bn := str((bones[i] as Dictionary).get("name", ""))
		bone_of_name[bn] = i
		var k := _name_key(bn)
		if not bones_of_key.has(k):
			bones_of_key[k] = []
		(bones_of_key[k] as Array).append(i)

	var skinned: Array = []
	_find_skinned(scene_root, skinned)
	if skinned.is_empty():
		push_error("BoneTextureMesh: no skinned mesh in the scene (export the .glb with its skin)")
		return null

	var out := ArrayMesh.new()
	var unmapped := {}
	var ambiguous := {}
	for mi_v in skinned:
		var mi: MeshInstance3D = mi_v
		var skin: Skin = mi.skin
		var skeleton := mi.get_node_or_null(mi.skeleton) as Skeleton3D
		# Skin bind index -> texture bone index.
		var remap := PackedInt32Array()
		remap.resize(skin.get_bind_count())
		for b in range(skin.get_bind_count()):
			var n := str(skin.get_bind_name(b))
			if n.is_empty() and skeleton != null and skin.get_bind_bone(b) >= 0:
				n = skeleton.get_bone_name(skin.get_bind_bone(b))
			remap[b] = -1
			if bone_of_name.has(n):
				remap[b] = bone_of_name[n]
			else:
				var cands: Array = bones_of_key.get(_name_key(n), [])
				if cands.size() == 1:
					remap[b] = cands[0]
				elif cands.size() > 1:
					ambiguous[n] = true
			if remap[b] < 0 and not ambiguous.has(n):
				unmapped[n] = true

		var mesh: Mesh = mi.mesh
		for s in range(mesh.get_surface_count()):
			var arrays: Array = mesh.surface_get_arrays(s)
			var verts: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
			var vcount := verts.size()
			var src_bones: PackedInt32Array = arrays[Mesh.ARRAY_BONES] if arrays[Mesh.ARRAY_BONES] != null else PackedInt32Array()
			var src_w: PackedFloat32Array = arrays[Mesh.ARRAY_WEIGHTS] if arrays[Mesh.ARRAY_WEIGHTS] != null else PackedFloat32Array()
			var per := (src_bones.size() / vcount) if vcount > 0 else 0
			var c0 := PackedFloat32Array()
			var c1 := PackedFloat32Array()
			c0.resize(vcount * 4)
			c1.resize(vcount * 4)
			for v in range(vcount):
				# Strongest 4 mapped influences, renormalised.
				var picked: Array = []
				for k in range(per):
					var w := src_w[v * per + k]
					var bind := src_bones[v * per + k]
					if w <= 0.0 or bind < 0 or bind >= remap.size() or remap[bind] < 0:
						continue
					picked.append([w, remap[bind]])
				picked.sort_custom(func(a, b): return a[0] > b[0])
				var total := 0.0
				for k in range(min(4, picked.size())):
					total += picked[k][0]
				for k in range(4):
					if k < picked.size() and total > 0.0:
						c0[v * 4 + k] = float(picked[k][1])
						c1[v * 4 + k] = picked[k][0] / total
					else:
						c0[v * 4 + k] = 0.0
						c1[v * 4 + k] = 0.0
			arrays[Mesh.ARRAY_BONES] = null
			arrays[Mesh.ARRAY_WEIGHTS] = null
			arrays[Mesh.ARRAY_CUSTOM0] = c0
			arrays[Mesh.ARRAY_CUSTOM1] = c1
			var flags := (Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM0_SHIFT) \
				| (Mesh.ARRAY_CUSTOM_RGBA_FLOAT << Mesh.ARRAY_FORMAT_CUSTOM1_SHIFT)
			out.add_surface_from_arrays(mesh.surface_get_primitive_type(s), arrays, [], {}, flags)
			var m := mi.get_active_material(s)
			if m != null:
				out.surface_set_material(out.get_surface_count() - 1, m)
	if not ambiguous.is_empty():
		push_warning("BoneTextureMesh: %d skin joints match several texture bones once names are normalised (weights dropped): %s"
			% [ambiguous.size(), ", ".join(PackedStringArray(ambiguous.keys()))])
	if not unmapped.is_empty():
		push_warning("BoneTextureMesh: %d skin joints have no bone in the texture (weights dropped): %s"
			% [unmapped.size(), ", ".join(PackedStringArray(unmapped.keys()))])
	return out


## Shader material for `mesh` (albedo taken from its first textured surface).
static func make_material(shader: Shader, bone_tex: Texture2D, sidecar: Dictionary,
		mesh: Mesh = null) -> ShaderMaterial:
	var mat := ShaderMaterial.new()
	mat.shader = shader
	mat.set_shader_parameter("bone_tex", bone_tex)
	var table: Array = []
	var clips: Array = sidecar.get("clips", [])
	for c in clips:
		var d: Dictionary = c
		table.append(Vector4(float(d.get("firstRow", 0)), float(d.get("frameCount", 1)),
			float(d.get("fps", 0.0)), 1.0 if bool(d.get("loop", true)) else 0.0))
	while table.size() < 32:
		table.append(Vector4.ZERO)
	if clips.size() > 32:
		push_warning("BoneTextureMesh: only the first 32 clips are addressable by the shader")
	mat.set_shader_parameter("clips", table.slice(0, 32))
	mat.set_shader_parameter("clip_count", min(clips.size(), 32))
	if mesh != null:
		for s in range(mesh.get_surface_count()):
			var sm := mesh.surface_get_material(s)
			if sm is BaseMaterial3D:
				var bm := sm as BaseMaterial3D
				mat.set_shader_parameter("albedo_color", bm.albedo_color)
				if bm.albedo_texture != null:
					mat.set_shader_parameter("albedo_tex", bm.albedo_texture)
				break
	return mat


static func apply_material(mesh: ArrayMesh, mat: Material) -> void:
	for s in range(mesh.get_surface_count()):
		mesh.surface_set_material(s, mat)
