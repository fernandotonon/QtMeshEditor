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
    // Canonical part labels. Index order is the model's output-channel order and
    // MUST match scripts/export-meshseg-onnx.py. Kept small + body-centric for
    // the first model; `Unknown` is the catch-all / unlabeled.
    enum class Part : uint8_t {
        Unknown = 0,
        Head,
        Torso,
        LeftArm,
        RightArm,
        LeftLeg,
        RightLeg,
        Count
    };
    static int partCount();                  // == (int)Part::Count
    static QString partName(Part p);         // stable lowercase id ("head", …)
    static QString partName(int p);

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
    static QString modelPath();          // AppData/ai_models/segment/meshseg.onnx
    static bool modelPresent();
    // Download the model on first use (blocks via a local ModelDownloader event
    // loop). Returns the model path, or empty when offline/disabled/failed — the
    // caller then uses the geometric fallback. Honours QTMESH_SEGMENT_NO_DOWNLOAD
    // + base-URL override QTMESH_SEGMENT_MODEL_BASE_URL / QSettings
    // ai/segmentModelBaseUrl. Call on a thread with an event loop.
    static QString ensureModelBlocking();

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
};

#endif // MESH_SEGMENTER_H
