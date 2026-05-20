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
// 5. The mesh MUST have UV2. There are two cases:
//      - Pre-authored UV2 ([0,1] floats from Blender's openvat add-on):
//        leave `synthesized_uv2 = false`. Shader uses textureLod with
//        the canonical reference math.
//      - Synthesized UV2 (packed integer pixel coords — what
//        QtMeshEditor's harness builds when the imported glTF lacks
//        UV2): set `synthesized_uv2 = true`. Shader uses texelFetch
//        with explicit pixel addressing. See
//        `tools/godot-vat-test/scripts/VATPlayer.gd:_ensure_uv2_on_mesh`
//        for the GDScript that builds the packed-integer UV2.
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

// Packed VAT texture. Position rows live in the upper half (pixel rows
// [row_block*N .. row_block*N + N-1]), normals in the lower half
// (rows [row_block*N + N .. row_block*N + 2N-1]). Width = column-count
// per row-block; row_block always 0 for single-row bakes.
uniform sampler2D pos_tex : filter_nearest, repeat_disable, hint_default_white;

// Bounds — must match the bake's `os-remap` Min/Max exactly.
uniform vec3 bounds_min = vec3(0.0);
uniform vec3 bounds_max = vec3(1.0);
uniform int frame_count = 1;
uniform float current_frame = 0.0;

// See header comment. true → UV2 is packed integer (col, row_block)
// pixel coords + texelFetch path. false → UV2 is canonical [0,1] +
// textureLod path.
uniform bool synthesized_uv2 = false;

// Optional albedo. If `has_albedo_tex` is true, sampled via UV (the
// mesh's first UV channel).
uniform sampler2D albedo_tex : filter_linear_mipmap, repeat_enable, source_color;
uniform bool has_albedo_tex = false;
uniform vec3 base_color : source_color = vec3(0.85, 0.78, 0.65);

varying vec2 uv0_pass;

void vertex() {
	int safe_frames = max(frame_count, 1);
	int curr = int(floor(current_frame)) % safe_frames;
	if (curr < 0) curr += safe_frames;
	int next = (curr + 1) % safe_frames;
	float blend = fract(current_frame);

	vec3 p_curr, p_next, n_curr, n_next;

	if (synthesized_uv2) {
		// texelFetch path — UV2 = (col, row_block) as integers.
		// Bypasses sampler-rounding ambiguity at the half-texture
		// boundary, which bites when frame_count exactly bisects
		// tex_height (e.g. 71 frames × 142 rows on Apple Metal).
		int col = int(UV2.x);
		int row_block = int(UV2.y);
		int base_row = row_block * safe_frames;
		p_curr = texelFetch(pos_tex, ivec2(col, base_row + curr), 0).rgb;
		p_next = texelFetch(pos_tex, ivec2(col, base_row + next), 0).rgb;
		n_curr = texelFetch(pos_tex, ivec2(col, base_row + safe_frames + curr), 0).rgb;
		n_next = texelFetch(pos_tex, ivec2(col, base_row + safe_frames + next), 0).rgb;
	} else {
		// textureLod path — UV2 is the canonical openvat float UV.
		vec2 tex_size = vec2(textureSize(pos_tex, 0));
		float frame_step = 1.0 / tex_size.y;
		vec2 uv_c = UV2 + vec2(0.0, float(curr) * frame_step);
		vec2 uv_n = UV2 + vec2(0.0, float(next) * frame_step);
		p_curr = textureLod(pos_tex, uv_c, 0.0).rgb;
		p_next = textureLod(pos_tex, uv_n, 0.0).rgb;
		n_curr = textureLod(pos_tex, uv_c + vec2(0.0, -0.5), 0.0).rgb;
		n_next = textureLod(pos_tex, uv_n + vec2(0.0, -0.5), 0.0).rgb;
	}

	vec3 p = mix(p_curr, p_next, blend);
	VERTEX = bounds_min + p * (bounds_max - bounds_min);
	vec3 n = mix(n_curr, n_next, blend) * 2.0 - 1.0;
	// Negate to compensate for the FBX → Ogre import path's left-
	// handed conversion. Remove this if you're not seeing inverted
	// lighting (e.g. for bakes from a different source pipeline).
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
