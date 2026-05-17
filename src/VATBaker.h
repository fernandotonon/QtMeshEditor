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

#include <vector>

namespace Ogre { class Entity; }

/**
 * @brief Vertex Animation Texture (VAT) baker.
 *
 * Bakes a skinned animation into a 2D texture where each row encodes one
 * animation frame and each column is one vertex's post-skinning position.
 * The companion JSON sidecar carries the metadata a runtime shader needs
 * to decode the texture (frame count, vertex count, bounds, fps).
 *
 * This is the soft-body / per-vertex VAT variant — works for any skinned
 * animation regardless of how the deformation is parametrised. The
 * runtime shader is one texture sample per vertex per frame: cheap on
 * mobile / VR / instanced crowds where a full skeletal palette would
 * dominate the GPU budget.
 *
 * Slice 1 ships:
 *   - Engine-agnostic target (no axis swizzle, no per-engine sidecar)
 *   - RGBA8 normalised encoding (~3 mm error on a 1 m model)
 *   - Positions only (no normals yet)
 *
 * Future slices add: RGBA16 / EXR encodings, normal texture, Unity /
 * Unreal / Godot variants, and the Inspector + MCP wiring.
 *
 * Pure-data API on purpose — no QObject, no Ogre::Root assumption beyond
 * the entity being live; easier to unit-test.
 */
class VATBaker
{
public:
    enum class Encoding {
        RGBA8 = 0,  ///< 8-bit per channel, normalised against bounds.
        // Reserved for slice 2:
        // RGBA16,
        // RGBAF,
    };

    enum class Target {
        Agnostic = 0,
        // Reserved for slice 3:
        // Unity,
        // Unreal,
        // Godot,
    };

    struct Options {
        QString  animationName;           ///< Required — no default to avoid silently baking the wrong clip.
        double   fps          = 30.0;     ///< Frames per second to sample at.
        double   startTime    = -1.0;     ///< < 0 → animation start (0).
        double   endTime      = -1.0;     ///< < 0 → animation length.
        Encoding encoding     = Encoding::RGBA8;
        Target   target       = Target::Agnostic;
        QString  outputDir;               ///< Required.
        QString  basename;                ///< Without extension. Defaults to animationName when empty.
    };

    struct BakeResult {
        bool      ok = false;
        QString   error;            ///< Populated when !ok.
        QString   posTexPath;       ///< On-disk path to the position texture.
        QString   jsonPath;         ///< On-disk path to the sidecar JSON.
        int       frameCount = 0;
        int       vertexCount = 0;
        Ogre::Vector3 minBound = Ogre::Vector3::ZERO;
        Ogre::Vector3 maxBound = Ogre::Vector3::ZERO;
    };

    /**
     * @brief Bake `entity`'s animation `opts.animationName` into a VAT.
     *
     * Steps the entity's animation state at 1/fps intervals from
     * `startTime` to `endTime`, reads post-skinning vertex positions
     * via `_getSkelAnimVertexData()`, and writes:
     *
     *   - `<outputDir>/<basename>_pos.png` — position texture
     *     (one row per frame, one column per vertex).
     *   - `<outputDir>/<basename>.json` — sidecar describing layout.
     *
     * Pure function — no signals, no Sentry hookups here; the caller
     * decides how to report progress (the CLI subcommand prints to
     * stdout, the future Inspector controller will emit Qt signals).
     *
     * Required preconditions:
     *   - `entity` is non-null and has a skeleton.
     *   - The animation `opts.animationName` exists on the entity.
     *   - `opts.outputDir` exists (or is creatable) and is writable.
     *   - `opts.fps > 0`.
     *
     * On failure, `result.ok == false` and `result.error` describes why.
     */
    static BakeResult bake(Ogre::Entity* entity, const Options& opts);

    /// Encode a flat vector of positions into 8-bit RGBA pixels,
    /// normalised against `[minBound, maxBound]`. Public for tests.
    ///
    /// Layout: `pixels[(frame * vertexCount + vertex) * 4 + {0..3}]`.
    /// Channel mapping: R=x, G=y, B=z, A=255 (reserved).
    static std::vector<unsigned char> encodeRGBA8(
        const std::vector<Ogre::Vector3>& flatPositions,
        int frameCount,
        int vertexCount,
        const Ogre::Vector3& minBound,
        const Ogre::Vector3& maxBound);

    /// Inverse of `encodeRGBA8`. Public for round-trip tests.
    static std::vector<Ogre::Vector3> decodeRGBA8(
        const std::vector<unsigned char>& pixels,
        int frameCount,
        int vertexCount,
        const Ogre::Vector3& minBound,
        const Ogre::Vector3& maxBound);

    /// Build the JSON sidecar string. Public so tests can snapshot it
    /// without writing to disk.
    static QString buildSidecarJson(const BakeResult& result, const Options& opts);
};

#endif // VATBAKER_H
