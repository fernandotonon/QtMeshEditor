#ifndef IMAGETO3D_PARTUNWRAP_H
#define IMAGETO3D_PARTUNWRAP_H

// Part-based UV islands for generated meshes.
//
// xatlas (the default unwrap of every image-to-3D bake) only grows a chart
// while its faces stay within ~75 degrees of the chart's average normal, and a
// generated surface is noisy at the triangle scale, so it stops early: a
// 25k-triangle TRELLIS.2 goblin came out as 1480 charts, the largest 326
// triangles, 959 of them 10 triangles or fewer. The baked texture is a field
// of confetti nobody can paint on. Raising xatlas's cost limits does not help
// (maxCost 2 -> 64 gave 1480 -> 1388 charts): the 75-degree rule is hard.
//
// This module makes the charts itself and leaves only the PACKING to xatlas
// (ChartOptions::useInputMeshUvs):
//   1. regions from colour: area-weighted k-means in CIELAB on a per-face
//      colour, majority-smoothed over the face graph, small regions merged
//      into their strongest neighbour — skin, cloth, leather and metal become
//      separate islands and seams run along the material boundaries;
//   2. each region is cut to a disc: a dual-spanning-tree cut graph (tree-
//      cotree, pruned of dangling branches) opens every boundary loop and
//      every handle (generated meshes are full of tiny accidental tunnels);
//   3. the disc is flattened with a Tutte embedding (provably fold-free)
//      refined by ARAP (Liu et al. 2008) so the island keeps the 3D AREA —
//      LSCM alone shrank large curved regions by orders of magnitude;
//   4. a chart whose area distortion, folds or self-overlap exceed the
//      limits is split in two (2-means on smoothed centroid + normal) and
//      retried; pieces below Options::minChartFaces are left to xatlas.
//
// Faces left to xatlas get UV (0,0): xatlas's OriginalUv pass skips zero-area
// faces and its normal chart generator takes them (debris, slivers). xatlas
// also splits any remaining flipped triangle into its own chart (its flood
// fill never crosses a winding change), so a stray fold never overlaps.
//
// Pure data (no Ogre, no GL): unit-tested in PartUnwrap_test.cpp.

#include <QString>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace PartUnwrap {

struct Options {
    // Colour clusters (0 = no colour segmentation: one region per connected
    // component, split by geometry only).
    int colourClusters = 6;
    // Regions below this fraction of the total surface are merged into the
    // neighbour they share the longest border with.
    float minRegionFraction = 0.002f;
    // Majority-filter passes over the colour labels (cleans ragged borders).
    int labelSmoothPasses = 6;
    // Chart acceptance: area-weighted mean |log(uv area / 3D area)| after a
    // global scale (0.3 ~ +-35 %), the area fraction of folded triangles, and
    // the fraction of chart texels covered twice (boundary self-overlap).
    float maxAreaDistortion = 0.30f;
    float maxFlipFraction = 0.003f;
    float maxOverlapFraction = 0.01f;
    int arapIterations = 15;
    // Taubin passes on the proxy used to decide geometric splits (noise-free
    // centroids/normals); the flattening itself uses the real positions.
    int proxySmoothPasses = 40;
    int minChartFaces = 16;
    int maxSplitDepth = 12;
    // Meshes above this face count are not unwrapped (returns ok = false with
    // a reason; the caller falls back to plain xatlas).
    size_t maxFaces = 600000;
    // Polled between charts; return true to stop.
    std::function<bool()> cancelled;
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    QString error;
    // Seam-split mesh. Triangle t of `indices` is input triangle t.
    std::vector<uint32_t> xref;      // output vertex -> input vertex
    std::vector<uint32_t> indices;
    // 2 floats per output vertex, in MESH units (island area == 3D area).
    // Vertices of faces left to xatlas are (0,0).
    std::vector<float> uvs;
    int charts = 0;            // islands made here
    int colourRegions = 0;     // regions after colour segmentation + merging
    int facesToXatlas = 0;     // faces left to xatlas's own charting
};

// `faceRgb` is 3 floats (0..1, sRGB) per input triangle, or empty.
// `faceExtra` is an optional extra feature per triangle (e.g. TRELLIS.2
// metallic, 0..1) clustered together with the colour; empty = none.
Result unwrap(const std::vector<float>& positions,
              const std::vector<uint32_t>& indices,
              const std::vector<float>& faceRgb,
              const std::vector<float>& faceExtra,
              const Options& opts);

// Triangle centroids (3 floats each): the points a caller samples colour at.
std::vector<float> faceCentroids(const std::vector<float>& positions,
                                 const std::vector<uint32_t>& indices);

// ---- unwrap + pack (the one xatlas entry point of the image-to-3D bakes) ----
struct AtlasOptions {
    int textureSize = 1024;     // xatlas resolution hint
    int padding = 4;            // texels between charts
    // true: part islands (above), xatlas only packs; falls back to plain
    // xatlas charting when the unwrap fails or the mesh is too large.
    bool partIslands = true;
    Options parts;
    std::function<bool()> cancelled;   // polled; true = stop
};

struct Atlas {
    bool ok = false;
    bool cancelled = false;
    QString error;
    int width = 0, height = 0;
    std::vector<uint32_t> xref;      // output vertex -> INPUT vertex
    std::vector<float> uv;           // 2 per output vertex, atlas texels
    std::vector<uint32_t> indices;   // triangle t = input triangle t
    int chartCount = 0;
    bool usedPartIslands = false;
    int partCharts = 0;
    int partFacesToXatlas = 0;
    QString partFallbackReason;      // why part islands were not used
};

// `faceRgb`/`faceExtra` as for unwrap() (only read with partIslands).
// xatlas ignores faces whose 3D area is below FLT_EPSILON in ABSOLUTE units,
// so the mesh is unwrapped scaled to a ~1000 unit diagonal (charting and
// packing are scale-invariant; the output maps back through xref).
Atlas buildAtlas(const std::vector<float>& positions,
                 const std::vector<uint32_t>& indices,
                 const std::vector<float>& faceRgb,
                 const std::vector<float>& faceExtra,
                 const AtlasOptions& opts);

// Exposed for tests: the CIELAB conversion of an sRGB colour (0..1).
void srgbToLab(const float rgb[3], float lab[3]);

}  // namespace PartUnwrap

#endif  // IMAGETO3D_PARTUNWRAP_H
