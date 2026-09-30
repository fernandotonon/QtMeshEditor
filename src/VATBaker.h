/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef VATBAKER_H
#define VATBAKER_H

#include <QString>
#include <QStringList>

#include <OgreVector.h>

#include <cstdint>
#include <vector>

namespace Ogre { class Entity; }

/**
 * @brief Vertex Animation Texture (VAT) baker — OpenVAT format, four modes.
 *
 * One baker, four samplers (issue #522 — the VAT family):
 *
 *   - **Skeletal** (`Mode::Skeletal`, the original #371 design): step a
 *     skeletal animation state and read the post-skinning positions via
 *     `entity->_getSkelAnimVertexData()`. Output is bit-identical to the
 *     pre-#522 baker (same texture, same `os-remap` sidecar; only the
 *     extension keys grew).
 *   - **Mesh-anim** (`Mode::MeshAnim`): a full-mesh vertex clip (Alembic
 *     cache / `VAT_POSE` stream from `VertexAnimationManager`). The
 *     sampler enables the mesh animation state and reads the software
 *     vertex-animation buffer (`_getSoftwareVertexAnimVertexData()`),
 *     or the skinned buffer when the entity also carries a skeleton
 *     (the skeleton is left in bind pose so only the vertex clip moves).
 *   - **Morph** (`Mode::Morph`): a morph-weight clip (the Slice A
 *     "MorphAnim" weight track, or an imported glTF weights animation)
 *     resolved to per-frame vertex positions through Ogre's pose
 *     blending. Same sampler as mesh-anim; the sidecar additionally
 *     lists the morph target names.
 *   - **Rigid** (`Mode::Rigid`): one rigid transform per CHUNK
 *     (chunk = submesh) per frame. Each chunk's deformed vertices are
 *     fitted against its bind pose with Horn's closed-form quaternion
 *     method; the texture holds the chunk's pivot position (top half)
 *     and rotation quaternion (bottom half, RGBA), one texel per chunk
 *     per frame. Runtime shader: `p' = q * (p - pivot) + pivot_frame`.
 *     The source can be any animation (skeletal rigid binds, an Alembic
 *     RBD cache, ...); a non-rigid chunk shows up as a large
 *     `maxResidual` in the sidecar rather than silently baking wrong.
 *
 * Output per bake (two files):
 *   - `<basename>_pos.png` / `_pos.exr` — 8- or 16-bit PNG or float32
 *     EXR. Width = vertexCount (chunkCount for rigid). Height =
 *     2 × frameCount. Top half = positions normalized to
 *     `[minBound..maxBound]`. Bottom half = unit normals encoded as
 *     `(n+1)/2` (rigid: quaternion `(q+1)/2` in RGBA).
 *   - `<basename>-remap_info.json` — the canonical `os-remap` sidecar:
 *     `{ "os-remap": { "Min": ["…"], "Max": ["…"], "Frames": <int> } }`
 *     with 8-decimal-place stringified floats, bounds rounded outward
 *     to the nearest 0.1 — plus non-conflicting extension keys:
 *     `_mode`, `_bit_depth`, `_target`, `_axes`, `_producer`, and the
 *     mode-specific `_rigid` (chunk count + pivots), `_track`
 *     (mesh-anim clip id) or `_morph_targets`.
 *
 * Source space is Ogre Y-up right-handed. Consumer shaders apply the
 * engine swizzle on read (the Godot reference shader does `vec3(x, z,
 * -y)` to land in Godot's convention); the `_target` key records which
 * engine the bake was requested for so a pipeline can pick the matching
 * template. openvat shaders ignore unknown top-level fields.
 *
 * Pure-data API on purpose — no QObject, no Ogre::Root assumption beyond
 * the entity being live; easier to unit-test.
 */
class VATBaker
{
public:
    /// Which sampler drives the bake. See the class comment.
    enum class Mode { Skeletal, Rigid, MeshAnim, Morph };

    /// Canonical string ids used by the CLI (`--mode`), MCP (`mode`),
    /// the sidecar (`_mode`) and the Inspector picker:
    /// `skeletal` | `rigid` | `mesh-anim` | `morph`.
    static QString modeId(Mode mode);
    /// Parse a mode id (case-insensitive; `mesh_anim`/`meshanim`/
    /// `vertex` accepted as aliases of `mesh-anim`). Returns false on
    /// an unknown id and leaves `*out` untouched.
    static bool modeFromId(const QString& id, Mode* out);
    /// All four ids, in picker order.
    static QStringList modeIds();

    /// Encoding ids: `rgba8` (8-bit PNG) | `rgba16` (16-bit PNG, the
    /// default) | `exr` (float32 EXR). Map to `Options::bitDepth`
    /// 8 / 16 / 32.
    static QString encodingId(int bitDepth);
    static bool bitDepthFromEncodingId(const QString& id, int* outBitDepth);
    static QStringList encodingIds();

    /// Target-engine ids recorded in the sidecar (`_target`) and used
    /// by the surfaces to pick the shader template to ship alongside:
    /// `agnostic` (default) | `unity` | `unreal` | `godot`.
    static QStringList targetIds();
    static bool isValidTargetId(const QString& id);

    struct Options {
        Mode     mode = Mode::Skeletal;   ///< Sampler. Skeletal keeps the pre-#522 behaviour bit-for-bit.
        QString  animationName;           ///< Required — no default to avoid silently baking the wrong clip.
        double   fps          = 30.0;     ///< Frames per second to sample at.
        double   startTime    = -1.0;     ///< < 0 → animation start (0).
        double   endTime      = -1.0;     ///< < 0 → animation length.
        QString  outputDir;               ///< Required.
        QString  basename;                ///< Without extension. Defaults to animationName when empty.

        /// Bit depth per channel for the position+normal texture.
        ///   16 → quantize into [0..65535] over per-axis bounds; PNG out.
        ///        Smallest file, but the position quantization step is
        ///        `(boundsMax - boundsMin) / 65535` per axis — for a
        ///        ~2 m Mixamo dance that's ~0.03 mm. Sub-mm-coplanar
        ///        geometry (Mixamo eye sphere vs. head plug) z-fights
        ///        on specific frames because two adjacent vertices
        ///        round to the same uint16 → same final position →
        ///        depth ties resolved arbitrarily by the renderer.
        ///   32 → write raw float32 positions + (n+1)/2 normals; EXR
        ///        out. No quantization; round-trips to within float
        ///        rounding error (sub-micrometre at sub-1m scales),
        ///        which is plenty to keep coplanar shells separated.
        ///        File is ~2× larger than the PNG for typical bakes.
        ///
        /// Default 16 for backward compatibility — existing consumers
        /// of `qtmesh vat` that read `_pos.png` keep working unchanged.
        /// Pass 32 from the CLI via `--bake-precision 32`.
        ///   8 → same quantization as 16 but into [0..255]; RGBA8 PNG.
        ///        ~4 mm on a 1 m model — the smallest, widest-support
        ///        option (mobile targets); `--encoding rgba8`.
        int      bitDepth     = 16;

        /// Target engine id (see `targetIds()`). Recorded in the sidecar
        /// as `_target`; the texture data is identical for every target
        /// (the engine's shader template applies the axis swizzle on
        /// read). Empty = "agnostic".
        QString  target;

        /// Per-vertex column permutation. `vertexPermutation[c]` is the
        /// texture column to write Ogre vertex `c` into. Empty = identity.
        ///
        /// Why this exists: Assimp's gltf2 exporter hardcodes
        /// `aiProcess_JoinIdenticalVertices` (assimp/code/Common/Exporter.cpp),
        /// which permutes per-primitive vertex order even when no
        /// duplicates actually get merged. The bake reads positions in
        /// Ogre's vertex-buffer order, but the consumer reads its UV2
        /// (= vertex index) from the post-Assimp glTF buffer. Without
        /// a remap the two are off and the model renders as shattered
        /// triangles. `cmdVat` builds this permutation by reading the
        /// post-export glTF and matching positions back to Ogre's
        /// vertex-buffer order, then passes it here so the bake's PNG
        /// columns land in the glTF's vertex order.
        std::vector<uint32_t> vertexPermutation;

    };

    /// One rigid chunk (rigid mode only). `vertexStart`/`vertexCount`
    /// index the bake's vertex walk (Ogre submesh order), so a consumer
    /// that aligns the source mesh via the bind sidecar can map any
    /// vertex to its chunk column.
    struct RigidChunk {
        QString       name;                        ///< Submesh name (or its index).
        Ogre::Vector3 pivot = Ogre::Vector3::ZERO;  ///< Bind-pose centroid; the rotation pivot.
        int           vertexStart = 0;
        int           vertexCount = 0;
        float         maxResidual = 0.0f;           ///< Worst |fit − actual| over all frames (source units).
    };

    struct BakeResult {
        bool      ok = false;
        QString   error;          ///< Populated when !ok.
        Mode      mode = Mode::Skeletal;
        QString   posTexPath;     ///< On-disk path to the packed position+normal texture.
        QString   jsonPath;       ///< On-disk path to the `<basename>-remap_info.json` sidecar.
        int       frameCount = 0;
        int       vertexCount = 0;   ///< Texture width. Rigid: == chunkCount.
        Ogre::Vector3 minBound = Ogre::Vector3::ZERO;
        Ogre::Vector3 maxBound = Ogre::Vector3::ZERO;

        // Mode-specific extras (mirrored into the sidecar).
        int                     chunkCount = 0;     ///< Rigid.
        std::vector<RigidChunk> chunks;             ///< Rigid.
        float                   maxRigidResidual = 0.0f; ///< Rigid — max over chunks.
        QString                 trackId;            ///< Mesh-anim: the vertex clip name.
        QStringList             morphTargets;       ///< Morph: pose names driven by the clip.
    };

    /**
     * @brief Bake `entity`'s animation `opts.animationName` into a VAT.
     *
     * Required preconditions (per mode):
     *   - Skeletal: `entity` has a skeleton and `opts.animationName` is
     *     one of its skeletal animations.
     *   - Mesh-anim / Morph: the mesh carries an animation of that name
     *     with vertex tracks (morph: the mesh also has poses).
     *   - Rigid: the animation exists (skeletal or vertex) and every
     *     submesh owns its vertex data (shared vertex data cannot be
     *     chunked per submesh).
     *   - The source mesh exposes vertex normals (VES_NORMAL) on every
     *     submesh — the packed-normal texture needs them (not rigid).
     *   - `opts.outputDir` exists (or is creatable) and is writable.
     *   - `opts.fps > 0`.
     *
     * On failure, `result.ok == false` and `result.error` describes why.
     */
    static BakeResult bake(Ogre::Entity* entity, const Options& opts);

    /// Build the OpenVAT sidecar JSON string. Public so tests can
    /// snapshot it without writing to disk.
    static QString buildSidecarJson(const BakeResult& result, const Options& opts);
};

#endif // VATBAKER_H
