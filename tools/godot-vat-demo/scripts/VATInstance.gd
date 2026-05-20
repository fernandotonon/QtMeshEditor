## VATInstance — drop-in MeshInstance3D that plays back an OpenVAT bake.
##
## Streamlined version of `tools/godot-vat-test/scripts/VATPlayer.gd` —
## same shader, but loads the bake once at startup and assumes the
## QtMeshEditor packed-normals layout (height = 2 × Frames). The demo
## scenes use this to spawn many instances cheaply.
##
## Usage in the scene: set `bake_dir` and `source_gltf`, and the rest
## is automatic.

class_name VATInstance
extends MeshInstance3D

## Directory containing `<name>_pos.png` + `<name>-remap_info.json`.
@export_dir var bake_dir: String = ""

## glTF whose mesh geometry drives the VAT replay. Must match the
## vertex order the baker saw.
@export_file("*.gltf", "*.glb") var source_gltf: String = ""

## Playback FPS — OpenVAT's sidecar doesn't carry it.
@export var fps: float = 30.0

## When true, the player drives `current_frame` itself every _process.
## Set to false if a parent (e.g. a perf demo) wants to drive all
## instances from a single shared float (saves N material updates per
## frame for big crowds).
@export var self_driven: bool = true

var _frame_count: int = 0
var _bounds_min: Vector3
var _bounds_max: Vector3
var _surface_materials: Array[ShaderMaterial] = []
var _current_frame: float = 0.0


func _ready() -> void:
	if not _load_source_mesh():
		push_error("VATInstance: load source mesh from %s failed" % source_gltf)
		return
	if not _load_bake():
		push_error("VATInstance: load bake from %s failed" % bake_dir)


func set_current_frame(f: float) -> void:
	## External entry point for shared-clock perf demos.
	_current_frame = f
	for mat in _surface_materials:
		mat.set_shader_parameter("current_frame", f)


func _load_source_mesh() -> bool:
	if source_gltf.is_empty(): return false

	# Web builds: the glTF was imported by Godot's editor and lives in
	# the pck as a compiled .scn PackedScene resource (no raw .gltf
	# bytes for GLTFDocument to parse at runtime). Native desktop
	# builds: the same load() path works because Godot resolves
	# `res://` through the resource pipeline uniformly.
	#
	# We INTENTIONALLY don't use GLTFDocument.append_from_file here —
	# `globalize_path()` returns an absolute filesystem path that has
	# no meaning in a web sandbox, and `append_from_file` on a
	# `res://` path fails in web builds because the .gltf isn't
	# exported as a file (it's the .scn that ships).
	var pack: Resource = load(source_gltf)
	if pack == null:
		push_error("VATInstance: load('%s') returned null" % source_gltf)
		return false
	var scene: Node = null
	if pack is PackedScene:
		scene = (pack as PackedScene).instantiate()
	elif pack is Mesh:
		# Some imports surface as a bare Mesh resource.
		mesh = pack as Mesh
		skeleton = NodePath("")
		return true
	else:
		push_error("VATInstance: source '%s' loaded as %s, expected PackedScene or Mesh" %
			[source_gltf, pack.get_class()])
		return false
	if scene == null: return false
	var found: ArrayMesh = _first_array_mesh(scene)
	scene.queue_free()
	if found == null: return false
	mesh = found
	skeleton = NodePath("")
	return true


func _first_array_mesh(root: Node) -> ArrayMesh:
	if root is MeshInstance3D:
		var m := (root as MeshInstance3D).mesh
		if m is ArrayMesh: return m as ArrayMesh
	for c in root.get_children():
		var f := _first_array_mesh(c)
		if f: return f
	return null


func _load_bake() -> bool:
	var dir := DirAccess.open(bake_dir)
	if dir == null: return false
	var json_path := ""
	var basename := ""
	for f in dir.get_files():
		if f.ends_with("-remap_info.json"):
			json_path = bake_dir.path_join(f)
			basename = f.substr(0, f.length() - "-remap_info.json".length())
			break
	if json_path.is_empty(): return false

	var pos_path := bake_dir.path_join(basename + "_pos.png")

	var sidecar: Variant = JSON.parse_string(FileAccess.get_file_as_string(json_path))
	if typeof(sidecar) != TYPE_DICTIONARY or not sidecar.has("os-remap"):
		return false
	var os: Dictionary = sidecar["os-remap"]
	_frame_count = int(os["Frames"])
	var mn: Array = os["Min"]; var mx: Array = os["Max"]
	_bounds_min = Vector3(float(mn[0]), float(mn[1]), float(mn[2]))
	_bounds_max = Vector3(float(mx[0]), float(mx[1]), float(mx[2]))

	# Load the bake texture via the resource pipeline. Native desktop
	# AND web builds both work — Godot's editor imports PNG as a
	# CompressedTexture2D resource that lives at `<path>.ctex` in the
	# pck. `load("res://....png")` resolves through the import map.
	# We can't use `Image.load_from_file(pos_path)` in web builds:
	# the raw PNG bytes aren't shipped in the pck (only the imported
	# .ctex is), so FileAccess.file_exists() returns false.
	#
	# IMPORTANT: the bake PNG MUST have its editor import settings
	# tweaked (sRGB OFF, Filter Nearest, Compression Lossless, Mipmaps
	# OFF). Without this, Godot compresses the texture and corrupts
	# the per-vertex position data. The demo project's import
	# overrides handle this; see assets/Rumba/mixamo.com_pos.png.import.
	var pos_tex: Texture2D = load(pos_path) as Texture2D
	if pos_tex == null:
		push_error("VATInstance: load bake texture failed: %s" % pos_path)
		return false

	# Try to read the Ogre bind-pose sidecar so we can remap each
	# Godot vertex back to its baker-side column index. Without
	# this, Godot's importer reorders vertices on import and the
	# bake's column indexing no longer matches Godot's vertex
	# array — the mesh renders as scattered triangles. The sidecar
	# is a flat float32 array of N×3 bind positions in Ogre's
	# original vertex-buffer order (matching the bake's PNG
	# columns 1:1).
	var ogre_bind := _load_ogre_bind_sidecar(basename)
	_ensure_uv2_on_mesh(pos_tex.get_height(), pos_tex.get_width(), ogre_bind)

	var shader := Shader.new()
	shader.code = _shader_code()

	_surface_materials.clear()
	for i in range(mesh.get_surface_count()):
		var mat := ShaderMaterial.new()
		mat.shader = shader
		mat.set_shader_parameter("pos_tex", pos_tex)
		mat.set_shader_parameter("frame_count", _frame_count)
		mat.set_shader_parameter("bounds_min", _bounds_min)
		mat.set_shader_parameter("bounds_max", _bounds_max)
		mat.set_shader_parameter("current_frame", 0.0)
		# Pick up the source mesh's albedo if any.
		var src_mat: Material = mesh.surface_get_material(i)
		if src_mat is StandardMaterial3D and (src_mat as StandardMaterial3D).albedo_texture != null:
			mat.set_shader_parameter("albedo_tex", (src_mat as StandardMaterial3D).albedo_texture)
			mat.set_shader_parameter("has_albedo_tex", true)
		set_surface_override_material(i, mat)
		_surface_materials.append(mat)

	# Expand bounding box so culling doesn't skip displaced verts.
	var pad: Vector3 = (_bounds_max - _bounds_min) * 0.1
	custom_aabb = AABB(_bounds_min - pad, (_bounds_max - _bounds_min) + pad * 2.0)
	return true


## Per-vertex bind-pose record. Mirrors `BakeVertex` on the C++ side.
class OgreBindVertex:
	var position: Vector3
	var normal: Vector3
	var uv: Vector2
	var has_normal: bool
	var has_uv: bool


func _load_ogre_bind_sidecar(basename: String) -> Array:
	## Read the per-vertex Ogre bind-pose sidecar (`<basename>_ogre_bind.bin`)
	## emitted by `qtmesh vat`. Returns an Array of `OgreBindVertex`
	## in Ogre's vertex-buffer order (matching the bake's PNG columns
	## 1:1), or an empty array on missing/malformed file.
	##
	## File layout (little-endian, packed):
	##   uint32 magic   = 0x42565442 ("BTVB")
	##   uint32 version = 1
	##   uint32 count
	##   uint32 flags   (bit 0 = positions, bit 1 = normals, bit 2 = uvs)
	##   then per-vertex: 3 floats position, 3 floats normal (if flag),
	##                    2 floats uv (if flag)
	var path := bake_dir.path_join(basename + "_ogre_bind.bin")
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		return []
	const MAGIC: int = 0x42565442
	var magic := f.get_32()
	var version := f.get_32()
	var count := f.get_32()
	var flags := f.get_32()
	if magic != MAGIC or version != 1 or count <= 0:
		f.close()
		return []
	var has_normal := (flags & 0x2) != 0
	var has_uv := (flags & 0x4) != 0
	var out: Array = []
	out.resize(count)
	for i in range(count):
		var v := OgreBindVertex.new()
		v.position = Vector3(f.get_float(), f.get_float(), f.get_float())
		if has_normal:
			v.normal = Vector3(f.get_float(), f.get_float(), f.get_float())
			v.has_normal = true
		if has_uv:
			v.uv = Vector2(f.get_float(), f.get_float())
			v.has_uv = true
		out[i] = v
	f.close()
	return out


func _build_ogre_to_godot_index_map(ogre_bind: Array) -> Dictionary:
	## Build a (position, normal, uv) → ogre-index lookup so each Godot
	## vertex can find its corresponding bake column in O(1).
	##
	## Two-tier quantization: positions at 1e-5 (sub-mm, exact through
	## float32 round-trips), normals + UVs at 1e-3 (Godot's importer
	## re-normalises normals after vertex reordering, drifting by up
	## to ~1e-5 per component which a 1e-5 quantizer would split into
	## separate buckets).
	##
	## Ambiguous keys (two distinct Ogre verts with identical quantized
	## signature) store -1; the matcher then falls back to identity for
	## those verts. Empirically this happens on degenerate splits where
	## the only differentiator is bone-weight data — those verts will
	## visually overlap regardless of which side we pick.
	var out: Dictionary = {}
	for i in range(ogre_bind.size()):
		var v: OgreBindVertex = ogre_bind[i]
		var key := _sig_key(v.position,
			v.normal if v.has_normal else Vector3.ZERO, v.has_normal,
			v.uv if v.has_uv else Vector2.ZERO, v.has_uv)
		if out.has(key):
			out[key] = -1
		else:
			out[key] = i
	return out


func _sig_key(position: Vector3,
	normal: Vector3, has_normal: bool,
	uv: Vector2, has_uv: bool) -> Array:
	## Quantized 8-tuple key. Returned as `Array[int]` rather than
	## `Vector3i` because Vector3i can't carry the normal + UV fields,
	## and Godot's Dictionary supports Array keys with deep equality.
	##
	## Tolerances (per-attribute, picked from observed Godot import
	## drift on a clean glTF round-trip):
	##   - position: 1e-5 (~10 µm on a 1-unit model — exact through
	##     float32 round-trip)
	##   - normal:   1e-2 (Godot re-normalizes after vertex reorder,
	##     and on glTF-compressed normals can drift up to ~5e-3 per
	##     component; 1e-2 keeps genuine seams in separate buckets
	##     while absorbing the noise)
	##   - uv:       1e-3 (UVs round-trip well, but Godot may
	##     re-encode through a half-float compressor with ~1e-4 drift;
	##     1e-3 gives a safe margin)
	var flags := 0
	var nx := 0; var ny := 0; var nz := 0
	if has_normal:
		nx = roundi(normal.x * 1e2); ny = roundi(normal.y * 1e2); nz = roundi(normal.z * 1e2)
		flags |= 1
	var u := 0; var v := 0
	if has_uv:
		u = roundi(uv.x * 1e3); v = roundi(uv.y * 1e3)
		flags |= 2
	return [
		roundi(position.x * 1e5), roundi(position.y * 1e5), roundi(position.z * 1e5),
		nx, ny, nz, u, v, flags]


func _ensure_uv2_on_mesh(tex_height: int, tex_width: int,
	ogre_bind: Array = []) -> void:
	# UV2 encodes integer (column, row_block) per vertex, NOT a true UV.
	# The shader uses `texelFetch` with explicit pixel coordinates, so
	# there's no floating-point boundary on which row a given frame
	# samples — every frame lands inside a single texel by construction.
	#
	#   UV2.x = ogre_column % tex_width      (texture column)
	#   UV2.y = ogre_column / tex_width      (row-block index; always 0
	#                                         for single-row bakes)
	#
	# When `ogre_bind` is provided (the standard `qtmesh vat` flow), we
	# remap each Godot vertex back to its Ogre vertex index by
	# quantized-signature (position, normal, UV0) match. This is
	# required because Godot's resource importer reorders the imported
	# glTF's vertex buffer for cache locality, so Godot's per-surface
	# vertex order no longer matches the bake's column order. Without
	# the sidecar lookup the mesh renders as scattered triangles.
	#
	# When `ogre_bind` is empty (legacy bakes, missing sidecar), we fall
	# back to a flat identity mapping. That only renders correctly when
	# the consumer's importer preserves vertex order.
	if mesh == null or _frame_count <= 0 or tex_width <= 0: return
	var bind_lookup: Dictionary = {}
	var has_bind := ogre_bind.size() > 0
	if has_bind:
		bind_lookup = _build_ogre_to_godot_index_map(ogre_bind)
	var rebuilt := ArrayMesh.new()
	var any_synth := false
	var running_offset := 0
	var unmatched_total := 0
	for i in range(mesh.get_surface_count()):
		var arrays: Array = mesh.surface_get_arrays(i)
		var src_mat: Material = mesh.surface_get_material(i)
		var prim: int = mesh.surface_get_primitive_type(i)
		var positions: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		var normals: PackedVector3Array = arrays[Mesh.ARRAY_NORMAL] \
			if arrays.size() > Mesh.ARRAY_NORMAL \
			and arrays[Mesh.ARRAY_NORMAL] is PackedVector3Array \
			else PackedVector3Array()
		var uvs: PackedVector2Array = arrays[Mesh.ARRAY_TEX_UV] \
			if arrays.size() > Mesh.ARRAY_TEX_UV \
			and arrays[Mesh.ARRAY_TEX_UV] is PackedVector2Array \
			else PackedVector2Array()
		var have_uv2: bool = arrays.size() > Mesh.ARRAY_TEX_UV2 \
			and arrays[Mesh.ARRAY_TEX_UV2] is PackedVector2Array \
			and (arrays[Mesh.ARRAY_TEX_UV2] as PackedVector2Array).size() == positions.size()
		if not have_uv2:
			any_synth = true
			var uv2 := PackedVector2Array()
			uv2.resize(positions.size())
			for j in range(positions.size()):
				var ogre_col := running_offset + j
				if has_bind:
					var p: Vector3 = positions[j]
					var n: Vector3 = normals[j] if j < normals.size() else Vector3.ZERO
					var has_n := j < normals.size()
					# Godot stores V as 1 - V_ogre on glTF import to match
					# the glTF spec convention (V=0 at bottom) — Ogre keeps
					# the source asset's UV convention as-authored. Undo
					# the flip before matching against the sidecar, which
					# is in Ogre's native UV space.
					var u_raw: Vector2 = uvs[j] if j < uvs.size() else Vector2.ZERO
					var has_u := j < uvs.size()
					var u: Vector2 = Vector2(u_raw.x, 1.0 - u_raw.y) if has_u else u_raw
					var key := _sig_key(p, n, has_n, u, has_u)
					if bind_lookup.has(key):
						var mapped: int = bind_lookup[key]
						if mapped >= 0:
							ogre_col = mapped
						else:
							unmatched_total += 1
					else:
						unmatched_total += 1
				var col := ogre_col % tex_width
				var row_block := ogre_col / tex_width
				uv2[j] = Vector2(float(col), float(row_block))
			arrays[Mesh.ARRAY_TEX_UV2] = uv2
		rebuilt.add_surface_from_arrays(prim, arrays)
		if src_mat != null:
			rebuilt.surface_set_material(i, src_mat)
		running_offset += positions.size()
	if has_bind and unmatched_total > 0:
		# Verts that couldn't be uniquely matched fall back to the
		# identity (running-offset) UV2 — they render against the
		# wrong bake column, producing localised scatter at those
		# vertices. Surface the count as a warning so the user knows
		# the bake's signature was not sufficient for this mesh.
		push_warning(("VATInstance: %d / %d Godot vertices had no unique " +
			"match in the Ogre bind sidecar — they fall back to identity " +
			"UV2 and may render misaligned. The sidecar carries position " +
			"+ normal + UV0; verts that share all three (e.g. weight-only " +
			"splits) are inherently ambiguous from outside the engine.")
			% [unmatched_total, running_offset])
	if any_synth:
		mesh = rebuilt


func _process(delta: float) -> void:
	if not self_driven or _surface_materials.is_empty() or _frame_count <= 0:
		return
	_current_frame = fposmod(_current_frame + delta * fps, float(_frame_count))
	for mat in _surface_materials:
		mat.set_shader_parameter("current_frame", _current_frame)


func _shader_code() -> String:
	return """
shader_type spatial;
render_mode cull_disabled, depth_draw_opaque, depth_test_default;

uniform sampler2D pos_tex : filter_nearest, repeat_disable, hint_default_white;
uniform sampler2D albedo_tex : filter_linear_mipmap, repeat_enable, source_color;
uniform bool has_albedo_tex = false;
uniform float current_frame = 0.0;
uniform int frame_count = 1;
uniform vec3 bounds_min = vec3(0.0);
uniform vec3 bounds_max = vec3(1.0);
uniform vec3 base_color : source_color = vec3(0.85, 0.78, 0.65);

varying vec2 uv0_pass;

void vertex() {
	// UV2 holds INTEGER (column, row_block) per vertex — not a true UV.
	// VATInstance._ensure_uv2_on_mesh packs them as floats but treats
	// them as integers in here. The shader uses `texelFetch` with
	// explicit pixel coordinates, sidestepping the half-pixel boundary
	// glitches that broke the UV-based sampler path.
	int col = int(UV2.x);
	int row_block = int(UV2.y);
	int N = max(frame_count, 1);

	int curr = int(floor(current_frame));
	curr = ((curr % N) + N) % N;
	int next = (curr + 1) % N;
	float blend = fract(current_frame);

	// Position rows live at pixel rows [row_block*N .. row_block*N+N-1]
	// in the top half of the texture. Pixel row 0 = top in image
	// space = bottom in OpenGL/Godot bottom-up V space → texelFetch
	// reads rows top-down from row 0, so frame `f` of a vertex in
	// row_block `b` is at pixel row `b*N + f`.
	int base_row = row_block * N;
	vec3 p_curr = texelFetch(pos_tex, ivec2(col, base_row + curr), 0).rgb;
	vec3 p_next = texelFetch(pos_tex, ivec2(col, base_row + next), 0).rgb;
	vec3 p = mix(p_curr, p_next, blend);
	VERTEX = bounds_min + p * (bounds_max - bounds_min);

	// Normals live in the BOTTOM half of the texture, rows [N..2N-1]
	// (or row_block*N + N + frame for tile layouts).
	vec3 n_curr = texelFetch(pos_tex, ivec2(col, base_row + N + curr), 0).rgb;
	vec3 n_next = texelFetch(pos_tex, ivec2(col, base_row + N + next), 0).rgb;
	vec3 n = mix(n_curr, n_next, blend) * 2.0 - 1.0;
	NORMAL = -normalize(n);

	uv0_pass = UV;
}

void fragment() {
	vec3 albedo = base_color;
	if (has_albedo_tex) albedo *= texture(albedo_tex, uv0_pass).rgb;
	ALBEDO = albedo;
	ROUGHNESS = 0.6;
	METALLIC = 0.0;
}
"""
