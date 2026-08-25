/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — cavity / curvature / AO derived-map generation
(Paint v2 Slice G, issue #550)

Generates per-mesh scalar maps used as brush colour sources or layer masks:
edge wear from convex curvature, crevice dirt from cavity, weathering from AO.

Pure data: inputs are an EditableMesh (+ optional pre-baked occlusion samples),
outputs are scalar maps in UV0 space, so the core is unit-testable headlessly.
The Ogre-scene side (depth-map rendering for AO, texture upload) lives in the
controller — see DerivedMapCache for the on-disk side.

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#ifndef DERIVEDMAPGENERATOR_H
#define DERIVEDMAPGENERATOR_H

#include "EditableMesh.h"

#include <QString>

#include <cstdint>
#include <vector>

/// Which derived map to generate. Values are persisted in the on-disk cache
/// (as a filename component), so do NOT renumber — append only.
enum class DerivedMapKind {
    Cavity = 0,     ///< concave = 1 (dark/dirt), convex+flat = 0
    Curvature = 1,  ///< signed, remapped to 0..1 with 0.5 = flat
    AmbientOcclusion = 2,
};

/// A single-channel float map in UV0 space, plus the coverage mask the
/// rasteriser produced (1 = a triangle wrote this texel, 0 = background).
/// Coverage is kept alongside the values because dilation must know which
/// texels are real — a texel whose value happens to equal the background is
/// otherwise indistinguishable from an unwritten one (the same reasoning as
/// VertexColorBaker's explicit coverage vector).
struct DerivedMap {
    int width = 0;
    int height = 0;
    std::vector<float> values;      ///< size w*h, row-major, top-left origin
    std::vector<uint8_t> coverage;  ///< size w*h, 1 = written by rasterisation

    bool empty() const { return width <= 0 || height <= 0 || values.empty(); }
    /// Nearest-texel sample; clamps out-of-range UV to the edge. `v` follows
    /// the project's top-left-origin convention (v=0 is the first row), the
    /// same as TexturePaintBuffer::uvToPixel.
    float sample(float u, float v) const;
};

class DerivedMapGenerator
{
public:
    struct Options {
        /// Output map size (square).
        int resolution = 1024;
        /// Pixels of seam dilation after rasterisation (0 = none). UV islands
        /// need this or bilinear/MIP sampling bleeds background across seams.
        int dilationPixels = 4;
        /// Value written where no triangle covers a texel. 0.5 is neutral for
        /// curvature (flat) and for cavity/AO means "no occlusion" once the
        /// caller's convention is applied; see kind-specific defaults in
        /// backgroundFor().
        float background = 0.0f;
        /// Cavity/curvature: scales the raw concavity before clamping. Higher
        /// = more contrast. 1.0 leaves the geometric value untouched.
        float contrast = 1.0f;
        /// Curvature only: values within this of flat are pinned to neutral,
        /// which suppresses tessellation noise on nominally flat panels.
        float flatTolerance = 0.02f;
    };

    struct Report {
        bool ok = false;
        QString error;
        int texelsRasterised = 0;
        int texelsDilated = 0;
        int trianglesSkippedNoUv = 0;
        float minValue = 0.0f;
        float maxValue = 0.0f;
    };

    /// Per-vertex concavity in -1..1: negative = convex (edge/ridge),
    /// positive = concave (crevice), ~0 = flat. Computed from the angle
    /// between the vertex normal and the direction to each 1-ring neighbour:
    /// a neighbour lying "in front of" the normal plane means the surface
    /// bends away (concave). Averaged over the ring.
    ///
    /// `mesh` is welded across submeshes internally (via HalfEdgeMesh) so a UV
    /// seam or material split does not read as a crease.
    /// Returns one value per WELDED vertex; use `weldedVertexCount()` to size.
    static std::vector<float> vertexConcavity(const EditableMesh& mesh);

    /// Rasterise per-welded-vertex scalars into a UV-space map.
    /// `perVertex` must be sized as `vertexConcavity` returns.
    static DerivedMap rasterise(const EditableMesh& mesh,
                                const std::vector<float>& perVertex,
                                const Options& options,
                                Report* report = nullptr);

    /// Generate cavity or curvature. AmbientOcclusion is NOT handled here —
    /// it needs scene-side occlusion sampling; use `fromVertexOcclusion`.
    static DerivedMap generate(const EditableMesh& mesh,
                               DerivedMapKind kind,
                               const Options& options,
                               Report* report = nullptr);

    /// Build an AO map from per-welded-vertex occlusion in 0..1 (1 = fully
    /// occluded). The caller supplies these from whatever visibility source it
    /// has (the controller uses hemisphere depth maps); keeping that out of
    /// here is what lets the rasterisation path stay headless-testable.
    static DerivedMap fromVertexOcclusion(const EditableMesh& mesh,
                                          const std::vector<float>& occlusion,
                                          const Options& options,
                                          Report* report = nullptr);

    /// Number of welded vertices `vertexConcavity` will return / that
    /// `perVertex` inputs must match.
    static size_t weldedVertexCount(const EditableMesh& mesh);

    /// Map the -1..1 concavity signal to the 0..1 stored range for `kind`.
    /// Cavity keeps only the concave half; curvature centres flat at 0.5.
    /// Pure math, exposed for unit tests.
    static float remapForKind(float concavity, DerivedMapKind kind,
                              const Options& options);

    /// Default background value for a kind (the value meaning "no effect").
    static float backgroundFor(DerivedMapKind kind);

    /// Stable short name, used as the on-disk filename component.
    static const char* kindName(DerivedMapKind kind);
};

#endif // DERIVEDMAPGENERATOR_H
