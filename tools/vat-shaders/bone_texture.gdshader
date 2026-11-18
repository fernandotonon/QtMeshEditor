// Bone texture animation (BTA) — QtMeshEditor `qtmesh vat --mode bone-texture`
// (issue #1166, texture layout `qtmesh-bone-texture-v1`).
//
// GPU linear blend skinning from a baked bone-matrix texture: the mesh keeps
// its bind pose, each vertex carries 4 bone indices (CUSTOM0) and weights
// (CUSTOM1), and every frame of every clip is a row of 3x4 skinning matrices
// in `bone_tex`. One mesh + one texture serve any number of instances; each
// instance picks its own clip, time offset, speed and crossfade through
// INSTANCE_CUSTOM (MultiMesh custom data), so nothing is ticked on the CPU
// per instance.
//
// Build the mesh with BoneTextureMesh.gd (it remaps the imported skin's
// joints onto the texture's bone order BY NAME and writes CUSTOM0/1), then
// use BoneTextureMesh.make_material() to fill the uniforms below.
//
// Texture layout (see `<basename>_bones.json`):
//   texel (bone*3 + r, row) = row r of the bone's 3x4 matrix
//   rows: clips stacked vertically; clip c spans clips[c].x .. + clips[c].y - 1
//   each clip samples both ends (last row = clip end), so rows of one clip
//   never blend into the next.
// Import the EXR uncompressed (Import > Compress Mode: VRAM Uncompressed,
// mipmaps off) — a lossy/VRAM-compressed matrix texture is garbage.
//
// INSTANCE_CUSTOM (per instance):
//   r = clip index
//   g = time offset in seconds (desynchronises a crowd)
//   b = playback speed (0 is treated as 1, so a plain MeshInstance3D plays)
//   a = crossfade: integer part = second clip index, fractional part = its
//       weight (0 = no crossfade). Both clips play at the same local time.
//
// Interpolation between frames (and the crossfade) blends matrices
// component-wise: exact on frames, slightly shrinks large per-frame
// rotations in between (measured 1.2 mm on a fast 2-bone test at 20 fps).

shader_type spatial;
render_mode cull_back, depth_draw_opaque;

uniform highp sampler2D bone_tex : filter_nearest, repeat_disable;
// x = first row, y = frame count, z = effective fps, w = loop (1/0)
uniform vec4 clips[32];
uniform int clip_count = 1;
uniform float time_seconds = 0.0;

uniform sampler2D albedo_tex : source_color, filter_linear_mipmap, repeat_enable, hint_default_white;
uniform vec4 albedo_color : source_color = vec4(1.0);
uniform float roughness_value : hint_range(0.0, 1.0) = 0.6;

mat4 bone_row(int bone, int row) {
	vec4 r0 = texelFetch(bone_tex, ivec2(bone * 3,     row), 0);
	vec4 r1 = texelFetch(bone_tex, ivec2(bone * 3 + 1, row), 0);
	vec4 r2 = texelFetch(bone_tex, ivec2(bone * 3 + 2, row), 0);
	// Texels are matrix ROWS; GLSL mat4 is built from COLUMNS.
	return mat4(vec4(r0.x, r1.x, r2.x, 0.0),
	            vec4(r0.y, r1.y, r2.y, 0.0),
	            vec4(r0.z, r1.z, r2.z, 0.0),
	            vec4(r0.w, r1.w, r2.w, 1.0));
}

// Fractional texture row of `clip` at local time `t`.
float clip_row(int clip, float t) {
	vec4 info = clips[clamp(clip, 0, max(clip_count - 1, 0))];
	if (info.y <= 1.0 || info.z <= 0.0) return info.x;
	float duration = (info.y - 1.0) / info.z;
	float lt = info.w > 0.5 ? mod(t, duration) : clamp(t, 0.0, duration);
	return info.x + lt * info.z;
}

mat4 bone_at(int bone, int clip, float row) {
	vec4 info = clips[clamp(clip, 0, max(clip_count - 1, 0))];
	if (info.y <= 1.0) return bone_row(bone, int(info.x));
	int r0 = min(int(floor(row)), int(info.x + info.y) - 2);
	float f = row - float(r0);
	return bone_row(bone, r0) * (1.0 - f) + bone_row(bone, r0 + 1) * f;
}

mat4 skin_matrix(vec4 idx, vec4 w, int clip, float row) {
	float total = w.x + w.y + w.z + w.w;
	if (total <= 0.0) return mat4(1.0);
	mat4 m = bone_at(int(idx.x), clip, row) * w.x;
	if (w.y > 0.0) m += bone_at(int(idx.y), clip, row) * w.y;
	if (w.z > 0.0) m += bone_at(int(idx.z), clip, row) * w.z;
	if (w.w > 0.0) m += bone_at(int(idx.w), clip, row) * w.w;
	return m * (1.0 / total);
}

void vertex() {
	int clip_a = int(INSTANCE_CUSTOM.r + 0.5);
	float speed = INSTANCE_CUSTOM.b == 0.0 ? 1.0 : INSTANCE_CUSTOM.b;
	float t = time_seconds * speed + INSTANCE_CUSTOM.g;

	mat4 skin = skin_matrix(CUSTOM0, CUSTOM1, clip_a, clip_row(clip_a, t));
	float fade = fract(INSTANCE_CUSTOM.a);
	if (fade > 0.0) {
		int clip_b = int(floor(INSTANCE_CUSTOM.a));
		mat4 skin_b = skin_matrix(CUSTOM0, CUSTOM1, clip_b, clip_row(clip_b, t));
		skin = skin * (1.0 - fade) + skin_b * fade;
	}

	// Skin in mesh space; Godot then applies the instance/model-view
	// transform as usual.
	mat3 skin3 = mat3(skin);
	VERTEX = (skin * vec4(VERTEX, 1.0)).xyz;
	NORMAL = normalize(skin3 * NORMAL);
	TANGENT = normalize(skin3 * TANGENT);
	BINORMAL = normalize(skin3 * BINORMAL);
}

void fragment() {
	vec4 base = texture(albedo_tex, UV) * albedo_color;
	ALBEDO = base.rgb;
	ROUGHNESS = roughness_value;
	METALLIC = 0.0;
}
