// QtMeshEditor OpenVAT — Godot 4 spatial shader
// =================================================
// Plays back a Vertex Animation Texture baked by `qtmesh vat` (or any
// sharpen3d/openvat-compatible exporter).
//
// USAGE
// -----
// 1. Drop this file into your project (e.g. res://shaders/openvat.gdshader).
// 2. Create a ShaderMaterial referencing this shader.
// 3. Assign it to your MeshInstance3D's `material_override`.
// 4. Read the bake's `<basename>-remap_info.json` and set the uniforms:
//      pos_tex       <- ImageTexture from <basename>_pos.png
//      bounds_min    <- Vector3 from os-remap.Min (parse the float strings)
//      bounds_max    <- Vector3 from os-remap.Max
//      frame_count   <- os-remap.Frames
//    Then drive `current_frame` from _process: it's a float in
//    `[0, frame_count)` that the shader interprets as `floor(f)` +
//    `fract(f)` blend between adjacent frames.
// 5. The mesh MUST have UV2 — if it doesn't (e.g. you imported via a
//    pipeline that stripped TEXCOORD_1), build UV2 in GDScript before
//    assigning the material. See `tools/godot-vat-test/scripts/VATPlayer.gd`
//    for the synthesis math (`_ensure_uv2_on_mesh`).
//
// IMPORTANT: load the bake textures as DATA textures (sRGB OFF,
// FilterMode = Nearest, Compression = None, Wrap = Clamp). Godot's
// default import will gamma-correct them and produce garbage.
//
// LICENSE
// -------
// MIT. Use freely.

shader_type spatial;
render_mode cull_disabled, depth_draw_opaque, depth_test_default;

// Packed VAT texture. Position rows live in the upper half (rows
// [0..frame_count)), normals in the lower half (rows [frame_count..
// 2*frame_count)). Width = vertex_count (or, for tile bakes, the
// row-block width).
uniform sampler2D pos_tex : filter_nearest, repeat_disable, hint_default_white;

// Bounds — must match the bake's `os-remap` Min/Max exactly.
uniform vec3 bounds_min = vec3(0.0);
uniform vec3 bounds_max = vec3(1.0);
uniform int frame_count = 1;
uniform float current_frame = 0.0;

// Optional albedo. If `has_albedo_tex` is true, sampled via UV (the
// mesh's first UV channel).
uniform sampler2D albedo_tex : filter_linear_mipmap, repeat_enable, source_color;
uniform bool has_albedo_tex = false;
uniform vec3 base_color : source_color = vec3(0.85, 0.78, 0.65);

varying vec2 uv0_pass;

void vertex() {
	// UV2 holds the per-vertex (column, base-row) into the texture
	// (OpenVAT add-on `core.py:create_uv_map`). Advance frames by
	// adding `frame * (1/texture_height)` to UV2.y.
	int safe_frames = max(frame_count, 1);
	int curr = int(floor(current_frame)) % safe_frames;
	if (curr < 0) curr += safe_frames;
	int next = (curr + 1) % safe_frames;
	float blend = fract(current_frame);

	vec2 tex_size = vec2(textureSize(pos_tex, 0));
	float frame_step = 1.0 / tex_size.y;
	vec2 uv_c = UV2 + vec2(0.0, float(curr) * frame_step);
	vec2 uv_n = UV2 + vec2(0.0, float(next) * frame_step);

	// Position: top half. Decode normalized [0,1] back to world coords.
	vec3 p_c = textureLod(pos_tex, uv_c, 0.0).rgb;
	vec3 p_n = textureLod(pos_tex, uv_n, 0.0).rgb;
	vec3 p = mix(p_c, p_n, blend);
	VERTEX = bounds_min + p * (bounds_max - bounds_min);

	// Normal: lower half, shifted down by 0.5 in UV V space. Encoded
	// as (n+1)/2, decoded as 2*c - 1.
	vec2 uv_cn = uv_c + vec2(0.0, -0.5);
	vec2 uv_nn = uv_n + vec2(0.0, -0.5);
	vec3 n_c = textureLod(pos_tex, uv_cn, 0.0).rgb;
	vec3 n_n = textureLod(pos_tex, uv_nn, 0.0).rgb;
	vec3 n = mix(n_c, n_n, blend) * 2.0 - 1.0;
	// Negate to compensate for the FBX → Ogre import path's left-
	// handed conversion. Remove this if you're not seeing inverted
	// lighting (e.g. for bakes that came from a different source mesh
	// pipeline).
	NORMAL = -normalize(n);

	uv0_pass = UV;
}

void fragment() {
	vec3 albedo = base_color;
	if (has_albedo_tex) {
		albedo *= texture(albedo_tex, uv0_pass).rgb;
	}
	ALBEDO = albedo;
	ROUGHNESS = 0.6;
	METALLIC = 0.0;
}
