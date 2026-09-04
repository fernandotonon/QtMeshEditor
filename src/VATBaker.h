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

#include <OgreVector.h>

#include <cstdint>
#include <vector>

namespace Ogre { class Entity; }

/**
 * @brief Vertex Animation Texture (VAT) baker — OpenVAT format.
 *
 * Bakes a skinned animation into the sharpen3d/openvat texture layout so
 * the off-the-shelf openvat shaders (Godot reference, Unity, Unreal,
 * Blender add-on) consume our output unmodified.
 *
 * Output per bake (two files):
 *   - `<basename>_pos.png` — 16-bit RGB PNG. Width = vertexCount.
 *     Height = 2 × frameCount. Top half = positions normalized to
 *     `[minBound..maxBound]`. Bottom half = unit normals encoded as
 *     `(n+1)/2`. No alpha channel.
 *   - `<basename>-remap_info.json` — the canonical `os-remap` sidecar:
 *     `{ "os-remap": { "Min": ["…"], "Max": ["…"], "Frames": <int> } }`
 *     with 8-decimal-place stringified floats, bounds rounded outward
 *     to the nearest 0.1.
 *
 * Source space is Ogre Y-up right-handed. Consumer shaders typically
 * apply a swizzle on read (the Godot reference shader does `vec3(x, z,
 * -y)` to land in Godot's convention). We document this via the
 * non-conflicting `_axes: "y-up-rh"` + `_producer: "QtMeshEditor"`
 * extension keys — openvat shaders ignore unknown top-level fields.
 *
 * Pure-data API on purpose — no QObject, no Ogre::Root assumption beyond
 * the entity being live; easier to unit-test.
 */
class VATBaker
{
public:
    struct Options {
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
        int      bitDepth     = 16;

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

        /// Mirror Z into EXPORT space (set when the source mesh entered the
        /// editor through the Assimp import's ConvertToLeftHanded — the glTF
        /// exporter applies the inverse for such meshes, and the bake must
        /// live in the same space as the exported mesh it pairs with).
        /// Native-provenance meshes (.mesh, generated, PS1) export
        /// pass-through, so their bakes stay unmirrored.
        bool exportSpaceMirrorZ = false;
    };

    struct BakeResult {
        bool      ok = false;
        QString   error;          ///< Populated when !ok.
        QString   posTexPath;     ///< On-disk path to the packed position+normal texture.
        QString   jsonPath;       ///< On-disk path to the `<basename>-remap_info.json` sidecar.
        int       frameCount = 0;
        int       vertexCount = 0;
        Ogre::Vector3 minBound = Ogre::Vector3::ZERO;
        Ogre::Vector3 maxBound = Ogre::Vector3::ZERO;
    };

    /**
     * @brief Bake `entity`'s animation `opts.animationName` into a VAT.
     *
     * Required preconditions:
     *   - `entity` is non-null and has a skeleton.
     *   - The animation `opts.animationName` exists on the entity.
     *   - The source mesh exposes vertex normals (VES_NORMAL) on every
     *     submesh — the packed-normal texture needs them.
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
