#ifndef MESH_SEGMENTER_H
#define MESH_SEGMENTER_H

#include <QString>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

// AI mesh part segmentation (issue #410, epic #397) — predicts a semantic part
// label (head / torso / arm / leg …) per vertex (and per face) of a mesh.
//
// Powers: Edit-Mode "Select by part", per-part material assignment, and priors
// for the auto-rigger (#407/#408).
//
// **The fourth ONNX consumer** (after #404 PbrMapSynth, #405 upscaler, #408
// UniRig, #409 RMIB). The ML backend is a PointNet++-style point-cloud
// segmentation network run via ONNX Runtime, OFF unless built with ENABLE_ONNX
// plus a first-use model download.
//
// **Permissive-data note.** The standard part-seg datasets (ShapeNet-Part,
// PartNet) are NON-COMMERCIAL, so they can't train a model we ship under the
// project's permissive bar (the same wall #408 RigNet / #409 LAFAN1 hit). The
// shipped model is therefore trained on SYNTHETIC, permissively-derived data:
// per-vertex part labels read from rigged-humanoid bone weights (a CC0
// derivation we own), sampled into point clouds. See THIRD_PARTY_AI_MODELS.md +
// scripts/export-meshseg-onnx.py.
//
// **Fallback is first-class** (per the issue + the epic's convention): a
// deterministic GEOMETRIC segmenter (connected-component "islands" + an
// up-axis/extent spatial heuristic, refined by skeleton-bone proximity when the
// mesh is rigged) is always compiled (no ONNX/Ogre) and used automatically when
// the build lacks ONNX, the model is missing/un-downloadable, or inference
// fails. It produces reasonable head/torso/limb labels on upright humanoids and
// at least a stable component labeling on anything else.
//
// This core is Ogre-free + unit-testable: it works on flat vertex/index arrays
// (and optional per-vertex bone-proximity hints) and returns per-vertex labels.
// `MeshSegmenterController` / CLI / MCP adapt it to an Ogre::Entity.
class MeshSegmenter {
public:
    // Canonical part labels — ONE shared vocabulary across every category
    // (#818 B2). The first 7 entries are the BODY model's output-channel order
    // and MUST keep matching scripts/export-meshseg-onnx.py (the meshseg.onnx
    // wire contract). Non-body categories ship their own small ONNX whose
    // LOCAL channels map into this enum via categoryChannelMap(). `Window` is
    // shared by the vehicle and building maps.
    enum class Part : uint8_t {
        Unknown = 0,
        // body (meshseg.onnx channels 1..6)
        Head,
        Torso,
        LeftArm,
        RightArm,
        LeftLeg,
        RightLeg,
        // vegetation (meshseg_vegetation.onnx channels 1..5)
        Trunk,
        Branch,
        Foliage,
        Root,
        Flower,
        // vehicle (meshseg_vehicle.onnx channels 1..5)
        VehicleBody,
        Wheel,
        Window,
        Wing,
        Rotor,
        // building (meshseg_building.onnx channels 1..6; window shared above)
        Wall,
        Roof,
        Door,
        Chimney,
        Foundation,
        Count
    };
    static int partCount();                  // == (int)Part::Count
    static QString partName(Part p);         // stable lowercase id ("head", …)
    static QString partName(int p);

    // Mesh categories (#818 B2): several specialised ~1 MB models, one per
    // category, instead of one big multi-category softmax (label imbalance,
    // coupled failures, and every category addition would force a full retrain
    // + re-download — see docs/MESH_SEGMENTATION_STRATEGY.md). `Auto` runs the
    // tiny point-cloud category classifier (meshseg_category.onnx) and falls
    // back to Body when it is unavailable.
    enum class Category : uint8_t {
        Auto = 0,
        Body,
        Vegetation,
        Vehicle,
        Building,
        CategoryCount
    };
    static QString categoryName(Category c);             // "auto", "body", …
    // Parse a category id ("vegetation", "auto", …). Unrecognised → Auto with
    // *ok=false (when provided).
    static Category categoryFromName(const QString& name, bool* ok = nullptr);
    // The category model's output channel count (incl. channel 0 = unknown).
    static int categoryChannelCount(Category c);
    // LOCAL model channel → global Part value; size == categoryChannelCount().
    static std::vector<int> categoryChannelMap(Category c);
    // Canonical model file name for a category ("meshseg.onnx",
    // "meshseg_vegetation.onnx", …). Auto maps to the body file.
    static QString modelFileName(Category c);

    // Map a SKELETON bone name → a segmentation part (or Unknown if it doesn't
    // correspond to a body region). This is the RIG-PRIOR path: when a mesh is
    // rigged, labelling each vertex by the part of the bone it's most skinned to
    // is EXACT — far better than the coordinate-only model, and it handles
    // non-human anatomy the humanoid model can't (ears/snout→head, tail→torso,
    // fingers→arm, toes→leg, wings→arm). Case-insensitive; tolerates common
    // prefixes (mixamorig:, bip01, def_, …). Pure-data + unit-testable.
    static Part partForBoneName(const QString& boneName);

    struct Options {
        Options();
        // Up axis (0=X,1=Y,2=Z, default +Y) — used by the geometric fallback's
        // head/leg heuristic and forwarded to the ONNX path.
        int upAxis = 1;
        // Force the deterministic geometric fallback even if a model is present
        // (CLI --no-model / tests).
        bool forceFallback = false;
        // Cap on points sampled for the model (PointNet++ is fixed-N; the
        // sampled labels are scattered back to all vertices by nearest point).
        int samplePoints = 4096;
        // Which category's label set / model to use. Auto = run the category
        // classifier (callers resolve via resolveCategoryBlocking(); predict()
        // itself treats a still-Auto value as Body).
        Category category = Category::Auto;

        // Floating-face cleanup: after labelling, reabsorb small disconnected
        // face-islands (a few mislabelled faces near a junction that form their
        // own tiny island within a part) into the surrounding part. Without it,
        // splitting leaves stray fragments floating next to the real part — bad
        // for 3D printing especially (#863). Default ON; opt out via CLI
        // --no-island-cleanup / MCP no_cleanup for raw model output.
        bool cleanupIslands = true;
        // An island is a stray candidate only if it has FEWER than this many
        // faces AND (see islandMaxFraction). A part's single LARGEST island is
        // never removed, so legitimately multi-island parts survive.
        int islandMinFaces = 32;
        // ...AND its face count is below this fraction of its label's total
        // faces. Both gates must hold — protects a small-but-legitimate part.
        float islandMaxFraction = 0.02f;
        // Straighten ragged part boundaries (the zigzag "fringe" teeth where the
        // torso meets the legs, #863) by flipping boundary faces that are mostly
        // surrounded by the other part. Runs BEFORE island cleanup. Gated by the
        // same `cleanupIslands` master switch. `boundarySmoothIterations` passes
        // (0 = off, each pass shaves one tooth-layer; 2-3 straightens a seam).
        int boundarySmoothIterations = 3;
        // Recut each part boundary with a straight, axis-snapped PLANE so the
        // seam is knife-clean and mirror-paired limbs cut level (#863). EXPER-
        // IMENTAL and OFF by default: the current band-reassign is too coarse and
        // scrambles parts on real characters — kept for future refinement. The
        // shipping cleanup is smoothLabelBoundaries + cleanupLabelIslands.
        bool planarRecut = false;
    };

    struct Result {
        bool ok = false;
        QString error;
        // Per-vertex part label (size == vertexCount). Values are Part as int.
        std::vector<int> vertexLabels;
        // Per-face label (size == indexCount/3): the majority label of the
        // face's 3 vertices. Convenience for Edit-Mode face selection.
        std::vector<int> faceLabels;
        bool usedModel = false;          // true = ONNX model; false = fallback
        QString fallbackReason;          // why the fallback ran (if it did)
        // The category the labels belong to (opts.category, Auto resolved).
        Category category = Category::Body;
    };

    using ProgressFn = std::function<bool(int stepsDone, int maxSteps)>;

    // True only when built with ENABLE_ONNX (model presence checked per call).
    static bool isModelBackendAvailable();

    // ---- Deterministic geometric fallback (always available) ----------------
    // Label vertices with NO model: connected components partition the mesh into
    // islands, then each island is classed by its centroid position relative to
    // the whole-mesh AABB along the up axis + lateral axis (top → Head, middle →
    // Torso, lower-left/right → Left/RightLeg, mid-left/right → Left/RightArm).
    // `boneProximity` (optional, size vertexCount) is a per-vertex hint: the
    // canonical part index a rig's nearest bone implies (or -1); when present it
    // overrides the spatial guess. Never fails for well-formed input.
    static Result segmentGeometric(const float* positions, int vertexCount,
                                   const uint32_t* indices, int indexCount,
                                   const Options& opts = {},
                                   const int* boneProximity = nullptr);

    // ---- ONNX path (model when available, else geometric fallback) ----------
    // AppData/ai_models/segment/<modelFileName(c)> (Auto → the body model).
    static QString modelPath(Category c = Category::Body);
    static bool modelPresent(Category c = Category::Body);
    // Download a category's model on first use (blocks via a local
    // ModelDownloader event loop). Returns the model path, or empty when
    // offline/disabled/failed — the caller then uses the geometric fallback.
    // Honours QTMESH_SEGMENT_NO_DOWNLOAD + base-URL override
    // QTMESH_SEGMENT_MODEL_BASE_URL / QSettings ai/segmentModelBaseUrl.
    // Call on a thread with an event loop.
    static QString ensureModelBlocking(Category c = Category::Body);

    // ---- Auto-dispatch category classifier (meshseg_category.onnx) ----------
    static QString classifierModelPath();
    static bool classifierModelPresent();
    static QString ensureClassifierModelBlocking();
    // Run the category classifier on the mesh's point cloud. Returns Body on
    // ANY failure (missing model, non-ONNX build, inference error) so Auto
    // degrades to the pre-#818 behaviour.
    static Category classifyCategory(const float* positions, int vertexCount,
                                     const QString& classifierPath,
                                     const Options& opts = {});
    // One-call Auto resolution for callers: an explicit opts.category is
    // returned as-is; Auto ensures the classifier model (first-use download)
    // and classifies, falling back to Body when unavailable/offline.
    static Category resolveCategoryBlocking(const float* positions, int vertexCount,
                                            const Options& opts = {});

    // Run the model if present; else fall back to segmentGeometric (recording
    // `fallbackReason`). Never throws.
    static Result predict(const float* positions, int vertexCount,
                          const uint32_t* indices, int indexCount,
                          const QString& modelPath,
                          const Options& opts = {},
                          const int* boneProximity = nullptr,
                          const ProgressFn& progress = {});

    // ---- Pure-data helpers (no ONNX/Ogre — unit-testable) -------------------
    // Connected components over the triangle adjacency: returns a per-vertex
    // island id (size vertexCount) and the island count. Vertices sharing a
    // triangle are in the same island.
    static int connectedComponents(int vertexCount,
                                   const uint32_t* indices, int indexCount,
                                   std::vector<int>& outIslandId);
    // Derive per-face labels (majority of the 3 vertex labels) from vertex
    // labels. Ties break toward the lowest part index.
    static std::vector<int> facesFromVertexLabels(const std::vector<int>& vertexLabels,
                                                  const uint32_t* indices, int indexCount);

    // Reabsorb small disconnected face-islands into the surrounding part.
    //
    // The model (and to a lesser degree the geometric fallback) occasionally
    // mislabels a handful of faces near a part junction — e.g. an ear-tip face
    // labelled as body, a wrist face labelled as hand — which form their own
    // tiny connected island WITHIN that part's face set. When the mesh is
    // split, each such island becomes a floating fragment next to the real
    // part (visible slivers near the ears/wrists/waist; bad for 3D printing).
    //
    // Operates on the FACE graph (faces adjacent across a shared edge). For each
    // part label it finds that label's connected face-islands; an island is a
    // stray iff it has < `minFaces` faces AND < `maxFraction` of its label's
    // total faces AND is not that label's single largest island. Each stray is
    // reassigned to the majority label among the faces adjacent across its
    // boundary (the part actually surrounding it). Iterated to stability (a
    // reabsorbed island can expose another). Pure-data + unit-testable; edits
    // `faceLabels` in place and returns the number of faces relabelled.
    static int cleanupLabelIslands(std::vector<int>& faceLabels,
                                   const uint32_t* indices, int indexCount,
                                   int minFaces = 32, float maxFraction = 0.02f);

    // Straighten ragged part boundaries on the FACE graph. A face on a label
    // boundary whose edge-neighbours are MOSTLY a single other label is a
    // zigzag "tooth" sticking into the wrong part (the green fringe where the
    // torso meets the legs, #863) — flip it to that majority-neighbour label.
    // A face flips only when a strict majority (>= `minAgree` of its up-to-3
    // edge-neighbours) shares one label different from the face's own AND that
    // agreeing label beats the face's own neighbour count — so a face flush in
    // the middle of its part (all neighbours same label) never moves. Iterated
    // `iterations` times; each pass shaves one tooth-layer, so 2-3 straightens
    // the seam without eroding a part. Pure-data; edits `faceLabels` in place,
    // returns the number of faces flipped. Run BEFORE cleanupLabelIslands (a
    // flip can leave a tiny island the island pass then reabsorbs).
    static int smoothLabelBoundaries(std::vector<int>& faceLabels,
                                     const uint32_t* indices, int indexCount,
                                     int iterations = 3, int minAgree = 2);

    // Recut adjacent part boundaries with a straight, knife-like PLANE (#863).
    // Local tooth-shaving (smoothLabelBoundaries) can't flatten a whole seam;
    // this fits a separating plane through each adjacent label-pair's boundary
    // and reassigns the faces in a band around it strictly by which side of the
    // plane their centroid falls on — a clean planar cut instead of a wavy one.
    //
    // The plane normal is the A→B centroid direction refined by the boundary,
    // and is SNAPPED to a world axis when within ~axisSnapDeg of one (a
    // torso/leg seam is near-horizontal → a perfectly level cut). Mirror-paired
    // parts (LeftLeg/RightLeg, LeftArm/RightArm) are coupled: their cut planes
    // share the same offset along the dominant axis so both legs cut at the same
    // height/size. Only faces within `bandFraction` × mesh-diagonal of the seam
    // are reconsidered, so geometry away from the joint is never disturbed.
    //
    // Needs `positions` (face centroids). Pure-data; edits `faceLabels`,
    // returns the number of faces relabelled. Run AFTER smoothing + island
    // cleanup (it wants coherent parts to fit planes to).
    static int planarBoundaryRecut(std::vector<int>& faceLabels,
                                   const float* positions, int vertexCount,
                                   const uint32_t* indices, int indexCount,
                                   float axisSnapDeg = 25.0f,
                                   float bandFraction = 0.12f);

    // Push cleaned per-face labels back onto vertices: each vertex takes the
    // majority label among the faces that reference it (ties → lowest label).
    // Keeps vertexLabels consistent with faceLabels after cleanupLabelIslands so
    // vertex-based consumers (rig priors, per-vertex export) agree with the
    // face-based split. Pure-data.
    static void vertexLabelsFromFaces(std::vector<int>& vertexLabels,
                                      const std::vector<int>& faceLabels,
                                      const uint32_t* indices, int indexCount);
};

#endif // MESH_SEGMENTER_H
