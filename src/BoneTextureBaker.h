/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef BONETEXTUREBAKER_H
#define BONETEXTUREBAKER_H

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <array>
#include <cstdint>
#include <vector>

namespace Ogre { class Entity; }

/**
 * @brief Bone texture baking (GPU skeletal animation textures) — issue #1166.
 *
 * The skeletal sibling of the vertex VAT (`VATBaker`): instead of storing
 * every vertex position per frame (O(vertices x frames)), it stores every
 * bone's SKINNING matrix per frame (O(bones x frames)); the vertex shader
 * keeps the mesh's bone indices/weights and skins on the GPU, so many
 * instances share one mesh and one texture and pick their own clip/time.
 *
 * Two layers, like the VAT baker:
 *   - **Pure data** (no Ogre): `Mat3x4`, `Layout`, packing / unpacking,
 *     the CPU reference sampler (frame interpolation + crossfade, i.e.
 *     exactly what the reference shader does), linear blend skinning, the
 *     sidecar JSON and the file writer. Unit-tested without a scene.
 *   - **Ogre adapter** (`sampleEntity`): steps each skeletal clip and
 *     reads `SkeletonInstance::_getBoneMatrices()` — Ogre's own skinning
 *     matrices (animated global x inverse bind), in bone-HANDLE order.
 *
 * ## Texture layout (`qtmesh-bone-texture-v1`)
 *   - RGBA float texture. Width = boneCount x 3, height = total frames of
 *     all clips (clips stacked vertically, each a contiguous row range
 *     starting at `Clip::firstRow`).
 *   - Texel (x = bone*3 + r, y = row) holds matrix ROW r: (m[r][0],
 *     m[r][1], m[r][2], m[r][3]). The 4th row of the affine is implicit
 *     (0,0,0,1). Skinned position: `p' = sum_i w_i * (M_i * vec4(p, 1))`.
 *   - Row 0 is the first scanline (top of the image).
 *   - Each clip samples BOTH ends: frame 0 at t = 0 and frame N-1 at
 *     t = duration, so interpolation never reads the next clip's rows and
 *     a looping clip wraps seamlessly. `Clip::fps` is the EFFECTIVE rate
 *     ((N-1)/duration) — use it, not the requested fps, to index frames.
 *
 * ## Space
 *   Matrices map BIND-POSE mesh-local positions to animated mesh-local
 *   positions in Ogre's space (Y-up, right-handed, source units). The
 *   entity's scene-node transform is NOT included. An engine with another
 *   convention applies `C * M * C^-1` (C = axis change) — the engine
 *   templates do that on read (slice C).
 */
namespace BoneTexture {

/// 3x4 affine matrix, row-major: `m[r*4 + c]`. Row 3 is (0,0,0,1).
struct Mat3x4 {
    std::array<float, 12> m{ 1,0,0,0, 0,1,0,0, 0,0,1,0 };
    float at(int r, int c) const { return m[static_cast<size_t>(r * 4 + c)]; }
    float& at(int r, int c) { return m[static_cast<size_t>(r * 4 + c)]; }
    static Mat3x4 identity() { return Mat3x4{}; }
    /// Transform a point (w = 1).
    void transformPoint(const float in[3], float out[3]) const;
    /// Transform a direction (w = 0).
    void transformVector(const float in[3], float out[3]) const;
    bool isFinite() const;
};

/// Component-wise `a*(1-t) + b*t` — what a GPU lerp of two fetched
/// matrices does. Not a rigid interpolation: large rotations between
/// frames shrink slightly (documented; frames are dense so it's small).
Mat3x4 lerp(const Mat3x4& a, const Mat3x4& b, float t);

struct Bone {
    QString name;
    int     parent = -1;   ///< Bone index of the parent, -1 for a root.
};

struct Clip {
    QString name;
    int     firstRow = 0;     ///< First texture row of this clip.
    int     frameCount = 0;   ///< Rows (>= 1). Frame 0 = t 0, last = t duration.
    double  duration = 0.0;   ///< Seconds.
    double  fps = 0.0;        ///< Effective: (frameCount-1)/duration (0 for a 1-frame clip).
    bool    loop = true;      ///< The source state's loop flag (a hint for players).
};

/// Everything the texture needs, in memory. `frames[clip][frame][bone]`.
struct BakedClips {
    std::vector<Bone> bones;
    std::vector<Clip> clips;     ///< firstRow is filled by `layoutClips`.
    std::vector<std::vector<std::vector<Mat3x4>>> frames;

    // Skin diagnostics from the source mesh (the export keeps the top 4).
    int sourceMaxInfluences = 0;
    int verticesOverLimit = 0;   ///< Vertices with > kMaxInfluences weights.
};

constexpr int kTexelsPerBone = 3;
constexpr int kMaxInfluences = 4;
constexpr int kDefaultMaxTextureSize = 8192;

/// Frame count for a clip of `duration` seconds at `fps`: both ends
/// sampled, at least 2 frames for a positive duration, 1 for a static
/// clip. Matches what `sampleEntity` bakes.
int frameCountFor(double duration, double fps);

/// Assign each clip's `firstRow` (stacked in order). Returns the total
/// row count (the texture height).
int layoutClips(std::vector<Clip>& clips);

/// Texture width for `boneCount` bones.
inline int textureWidth(int boneCount) { return boneCount * kTexelsPerBone; }

/**
 * Check the bake fits the texture limit and is internally consistent
 * (frame/bone counts, finite matrices). Returns an empty string when it
 * does, else an actionable error.
 */
QString validate(const BakedClips& baked, int maxTextureSize = kDefaultMaxTextureSize);

/// Pack into an interleaved RGBA float buffer (width x height x 4).
/// Call `layoutClips` first. Returns an empty vector when the input is
/// inconsistent (run `validate` for the reason).
std::vector<float> pack(const BakedClips& baked, int* outWidth, int* outHeight);

/// Read bone `bone`'s matrix at texture row `row` from a packed buffer.
Mat3x4 unpack(const std::vector<float>& texels, int width, int bone, int row);

/// Fractional row for time `t` (seconds) in `clip`: loop → wrap, else
/// clamp. What the reference shader computes.
double rowForTime(const Clip& clip, double t, bool loop);

/// Interpolated matrix of `bone` in `clip` at `t` (two fetches + lerp).
Mat3x4 sample(const std::vector<float>& texels, int width, const Clip& clip,
              int bone, double t, bool loop);

/// Two-clip crossfade: `lerp(sample(a, ta), sample(b, tb), weightB)`.
Mat3x4 crossfade(const std::vector<float>& texels, int width,
                 const Clip& a, double ta, const Clip& b, double tb,
                 int bone, float weightB, bool loop);

/// Linear blend skinning of one vertex with up to 4 influences. Weights
/// are renormalised over the given influences (what the export keeps).
/// `normal` (optional) is transformed by the same blended matrix and
/// renormalised — exact for rigid + uniform-scale bones, the standard
/// LBS approximation otherwise.
void skinVertex(const std::vector<Mat3x4>& boneMatrices,
                const float position[3], const float* normal,
                const int indices[kMaxInfluences], const float weights[kMaxInfluences],
                float outPosition[3], float* outNormal);

/// Encoding ids: `rgba32f` (float32 EXR, default) | `rgba16f` (half EXR).
QStringList encodingIds();
bool isValidEncoding(const QString& id);

/// Sidecar JSON (`qtmesh-bone-texture-v1`). `textureFile` is the file
/// name written into it (relative to the sidecar).
QString buildSidecarJson(const BakedClips& baked, const QString& textureFile,
                         const QString& encoding, int width, int height);

struct WriteResult {
    bool    ok = false;
    QString error;
    QString texturePath;   ///< `<dir>/<basename>_bones.exr`
    QString jsonPath;      ///< `<dir>/<basename>_bones.json`
    int     width = 0;
    int     height = 0;
};

/// Validate, pack and write the texture + sidecar. Files are written
/// through a temporary name and renamed, so a failure never leaves a
/// half-written texture next to an existing bake. `baked.clips` must be
/// laid out (`layoutClips`).
WriteResult write(const BakedClips& baked, const QString& outputDir,
                  const QString& basename, const QString& encoding,
                  int maxTextureSize = kDefaultMaxTextureSize);

// ── Ogre adapter ─────────────────────────────────────────────────────

struct SampleOptions {
    QStringList clips;        ///< Skeletal animations to bake; empty = all (skeleton order).
    double      fps = 30.0;
};

/**
 * Step `entity`'s skeletal clips and capture every bone's skinning matrix
 * per frame. The entity's animation states (enabled, time, weight, loop)
 * and its skeleton pose are restored afterwards. Fills `out` (clips laid
 * out) or returns false with `*error` set: no skeleton, unknown clip,
 * fps <= 0, invalid bind pose (non-finite / zero-scale inverse bind) or a
 * non-finite sampled matrix.
 */
bool sampleEntity(Ogre::Entity* entity, const SampleOptions& opts,
                  BakedClips* out, QString* error);

// ── Surface core (CLI `qtmesh vat --mode bone-texture`, MCP `bake_vat`,
//    Inspector VAT section) — one function so the three cannot drift ──

/// The mode id the VAT surfaces use for this bake (`bone-texture`).
QString modeId();
/// True for `bone-texture` and its aliases (`bones`, `bone_texture`,
/// `bta`), case-insensitive.
bool isModeId(const QString& id);
/// Normalise an encoding id: empty -> `rgba32f`; `exr`/`float32`/`f32`
/// -> `rgba32f`; `half`/`f16`/`float16` -> `rgba16f`. Returns false for
/// anything else (the VAT PNG encodings included: a bone matrix does not
/// survive 8/16-bit quantisation).
bool normaliseEncoding(const QString& id, QString* out);

struct BakeRequest {
    QStringList clips;            ///< Empty = every skeletal clip.
    double      fps = 30.0;
    QString     encoding;         ///< See `normaliseEncoding`; empty = rgba32f.
    QString     outputDir;
    QString     basename;         ///< Empty = the entity name.
    QStringList shaderEngines;    ///< Templates to copy next to the bake (godot).
    bool        exportMesh = true; ///< Also write `<basename>.glb` (bind pose + skin).
    int         maxTextureSize = kDefaultMaxTextureSize;
};

struct BakeReport {
    bool        ok = false;
    QString     error;
    WriteResult files;
    QString     meshPath;         ///< Empty when not exported / export failed (see warnings).
    QStringList shaders;          ///< Written template paths.
    QStringList warnings;
    QString     encoding;
    int         boneCount = 0;
    int         totalFrames = 0;
    std::vector<Clip> clips;
    int         sourceMaxInfluences = 0;
    int         verticesOverLimit = 0;
};

/// Sample `entity`, write texture + sidecar (+ mesh, + shaders) into
/// `req.outputDir`. The mesh is exported BEFORE sampling (the VAT
/// lesson: exporting after touching animation state produced a broken
/// glTF) and carries the skin, so an engine reads each vertex's bone
/// indices/weights from it and maps the skin's joints to the texture's
/// bone order BY NAME (`bones[].name` in the sidecar).
BakeReport bakeToDirectory(Ogre::Entity* entity, const BakeRequest& req);

/// Machine-readable summary (CLI `--json`, MCP result).
QJsonObject reportToJson(const BakeReport& report);

} // namespace BoneTexture

#endif // BONETEXTUREBAKER_H
