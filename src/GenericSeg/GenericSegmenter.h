#pragma once
// Generic (any-object) part segmentation (#1179, docs/GENERIC_SEGMENTATION_DESIGN.md).
//
// Our own implementation of the SAMesh method (Tang et al., arXiv 2408.13679):
// render the mesh from many views (normals + shape diameter), get 2D masks for
// each image, lift the masks onto the faces through the face-id buffer, group
// masks that cover the same faces across views with Leiden, vote a part per
// face, then clean up. Output: unnamed part instances, part_01 = largest.
//
// The 2D segmenter is behind `MaskProvider`, so everything here is pure data
// and testable with a fake provider; the SAM 2.1 ONNX provider is slice 2.

#include "MeshRaster.h"
#include "ShapeDiameter.h"

#include <QImage>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <vector>

namespace GenericSeg {

enum class ImageKind { Normal, Sdf, Matte };
QString imageKindName(ImageKind k);

enum class Granularity { Coarse, Medium, Fine };
QString granularityName(Granularity g);
Granularity granularityFromName(const QString& name, bool* ok = nullptr);

struct ViewImage {
    int view = 0;              // index into the view directions
    ImageKind kind = ImageKind::Normal;
    QImage image;              // what the 2D segmenter looks at (RGB32, black background)
    const Raster* raster = nullptr;   // face ids for the same pixels
    OrthoCamera camera;
};

struct Mask {
    std::vector<uint8_t> pixels;   // size*size, non-zero = inside, row 0 = top
    float score = 1.0f;            // segmenter confidence in (0, 1]
};

class MaskProvider {
public:
    virtual ~MaskProvider() = default;
    // Masks for one image. Returning nothing is fine (the view is skipped).
    virtual std::vector<Mask> masksFor(const ViewImage& view) = 0;
};

struct Options {
    // Rendered views: 12, 20, or any count (Fibonacci sphere); 0 = automatic
    // (viewCountFor). Measured on the benchmark (docs/GENERIC_SEGMENTATION_DESIGN.md):
    // normal + SDF meshes score the same or better with 12 views as with 20
    // at 60 % of the SAM time, matte (low-poly) meshes lose 0.05-0.08 F at 12,
    // i.e. what matters is about 20-24 images.
    int views = 0;
    int resolution = 512;           // raster size in pixels
    bool normalImages = true;
    bool sdfImages = true;
    // Matte images replace the normal + SDF images below this face count:
    // low-poly meshes give poor normal/SDF renders (SAMesh's observation).
    int matteBelowFaces = 2000;

    Granularity granularity = Granularity::Medium;
    // < 0 = from `granularity`.
    double leidenResolution = -1.0;
    float minPartFraction = -1.0f;  // parts below this share of the surface merge into a neighbour

    // Masks are lifted onto surface patches about this many pixels wide.
    float patchPixels = 2.0f;
    int minPatchPixels = 2;         // a patch counts as seen from a view with >= this many pixels
    float minCoverage = 0.3f;       // a mask covers a patch when it holds >= this share of its pixels
    // Two masks are compared on the patches both their views see: an edge
    // when their IoU there >= overlapThreshold, and only if that common
    // region is >= minCommonFraction of the smaller mask (else the views see
    // too little of each other to say anything).
    float overlapThreshold = 0.6f;
    float minCommonFraction = 0.1f;
    // Per image, a pixel belongs only to the smallest mask covering it
    // (SAMesh). Off only for providers that return disjoint masks anyway.
    bool smallestMaskWins = true;
    // Graph-cut refinement of the voted labels (alpha-expansion): each face
    // pays -log of its vote share for the part it gets, each edge between two
    // parts pays smoothness * edge length (much less along concave creases),
    // and short "through" links tie the two sides of a thin part (a tail, a
    // hand) so they cannot end up in different parts. Lengths and areas are
    // in the normalised mesh (unit bounding sphere).
    bool graphCut = true;
    // Votes are weighted by (median mask area / mask area)^exponent: a small
    // mask is more specific than the large one that also covers the patch
    // (in views where SAM gave no mask for a wheel, the whole-car mask votes
    // "body" on it). The cross-view version of smallestMaskWins.
    float voteAreaExponent = 0.5f;
    float voteSmoothing = 0.3f;     // pseudo-vote per part: few views = weak evidence
    float smoothness = 0.1f;        // cost per unit of boundary length on smooth surface
    float creaseAngleDegrees = 15.0f;   // concave edges sharper than this are cheap to cut
    float throughWeight = 4.0f;     // cost per unit area of splitting a thin part's two sides
    float throughThickness = 0.15f; // thickness where the through link has fallen to 1/e
    bool cleanup = true;            // boundary smoothing + small-island cleanup
    int smoothIterations = 3;
    SdfOptions sdf;
    uint32_t seed = 42;

    // Progress: (stage, done, total). Return false to cancel.
    std::function<bool(const QString& stage, int done, int total)> progress;
};

struct Stats {
    int views = 0;
    int images = 0;
    int masks = 0;            // masks returned by the provider
    int patches = 0;
    int nodes = 0;            // masks that cover at least one patch
    int edges = 0;
    int communities = 0;
    int unseenFaces = 0;      // faces no view saw (labelled from neighbours)
    int parts = 0;
};

struct Result {
    bool ok = false;
    bool cancelled = false;
    QString error;
    std::vector<int> faceLabels;     // per input triangle: 0 = unknown, 1..parts
    std::vector<int> vertexLabels;   // per input vertex (majority of its faces)
    QStringList labelNames;          // index = label: "unknown", "part_01", ...
    int parts = 0;
    Stats stats;
};

// `positions`: xyz per vertex, `indices`: 3 per triangle (any units).
Result segment(const float* positions, int vertexCount,
               const uint32_t* indices, int indexCount,
               MaskProvider& provider, const Options& opts = {});

// Views and images per view a mesh with `faceCount` triangles gets.
bool usesMatteImages(int faceCount, const Options& opts);
int viewCountFor(int faceCount, const Options& opts);
int imagesPerViewFor(int faceCount, const Options& opts);

// Granularity presets (resolution, min part fraction).
double leidenResolutionFor(Granularity g);
float minPartFractionFor(Granularity g);

} // namespace GenericSeg
