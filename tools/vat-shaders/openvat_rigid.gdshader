// QtMeshEditor OpenVAT — Godot 4 RIGID-BODY spatial shader
// =======================================================
// Plays back a rigid-body VAT baked by `qtmesh vat --mode rigid`:
// one rotation + pivot per CHUNK (chunk = submesh / glTF primitive)
// per frame, instead of one position per vertex. Useful for
// destruction caches (Houdini RBD), mechanical assemblies, anything
// made of pieces that move rigidly.
//
// TEXTURE LAYOUT (from the bake's `_rigid` sidecar block)
// ------------------------------------------------------
//   width  = chunk count            (one column per chunk)
//   height = 2 × Frames
//   rows [0 .. Frames)              pivot position of the chunk this
//                                   frame, normalized to os-remap
//                                   Min..Max (RGB; A unused)
//   rows [Frames .. 2*Frames)       rotation quaternion (x, y, z, w)
//                                   encoded (q + 1) / 2 in RGBA
//   sidecar `_rigid.chunks[i].pivot` = the chunk's BIND-pose pivot
//
// Runtime:   p' = q * (p - pivot_bind) + pivot_frame
//
// USAGE
// -----
// 1. Create one ShaderMaterial PER SURFACE (Godot surface == glTF
//    primitive == bake chunk, in order) and set `chunk_index` to the
//    surface index. The bake's `source.gltf` keeps submesh order, so
//    chunk i is surface i. (`--emit-uv2` on a rigid bake also writes
//    the chunk index into UV2.x so a single material can read it —
//    set `chunk_from_uv2 = true` for that path.)
// 2. From `<basename>-remap_info.json` set:
//      vat_tex      <- ImageTexture from <basename>_pos.png (data texture:
//                      sRGB OFF, filter nearest, no mipmaps, clamp)
//      bounds_min   <- os-remap.Min      bounds_max <- os-remap.Max
//      frame_count  <- os-remap.Frames
//      pivot_bind   <- _rigid.chunks[chunk_index].pivot
// 3. Drive `current_frame` in [0, frame_count) from _process.
//
// The bake is Y-up right-handed (Ogre). Godot is also Y-up RH, so no
// swizzle is applied here; for Unity (Y-up LH) negate X and the
// quaternion's x/w, for Unreal (Z-up LH) swap Y/Z — see the README.
//
// LICENSE: MIT.

shader_type spatial;
render_mode cull_disabled, depth_draw_opaque, depth_test_default;

uniform sampler2D vat_tex : filter_nearest, repeat_disable, hint_default_white;
uniform vec3 bounds_min = vec3(0.0);
uniform vec3 bounds_max = vec3(1.0);
uniform int frame_count = 1;
uniform float current_frame = 0.0;
uniform int chunk_index = 0;
uniform bool chunk_from_uv2 = false;
uniform vec3 pivot_bind = vec3(0.0);
uniform bool exr_raw_positions = false; // true for `--encoding exr` bakes (positions are raw, not normalized)

uniform sampler2D albedo_tex : filter_linear_mipmap, repeat_enable, source_color;
uniform bool has_albedo_tex = false;
uniform vec3 base_color : source_color = vec3(0.85, 0.78, 0.65);

varying vec2 uv0_pass;

vec3 quat_rotate(vec4 q, vec3 v) {
	// q = (x, y, z, w); v' = v + 2*cross(q.xyz, cross(q.xyz, v) + q.w*v)
	vec3 t = 2.0 * cross(q.xyz, v);
	return v + q.w * t + cross(q.xyz, t);
}

void vertex() {
	int safe_frames = max(frame_count, 1);
	int curr = int(floor(current_frame)) % safe_frames;
	if (curr < 0) curr += safe_frames;
	int next = (curr + 1) % safe_frames;
	float blend = fract(current_frame);

	int col = chunk_from_uv2 ? int(UV2.x) : chunk_index;

	vec3 pc = texelFetch(vat_tex, ivec2(col, curr), 0).rgb;
	vec3 pn = texelFetch(vat_tex, ivec2(col, next), 0).rgb;
	vec4 qc = texelFetch(vat_tex, ivec2(col, safe_frames + curr), 0) * 2.0 - 1.0;
	vec4 qn = texelFetch(vat_tex, ivec2(col, safe_frames + next), 0) * 2.0 - 1.0;

	vec3 pivot_frame = mix(pc, pn, blend);
	if (!exr_raw_positions) {
		pivot_frame = bounds_min + pivot_frame * (bounds_max - bounds_min);
	}
	// The bake keeps adjacent frames in the same hemisphere, so a
	// plain lerp + normalize is a valid short-arc blend.
	vec4 q = normalize(mix(qc, qn, blend));

	VERTEX = quat_rotate(q, VERTEX - pivot_bind) + pivot_frame;
	NORMAL = normalize(quat_rotate(q, NORMAL));

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
