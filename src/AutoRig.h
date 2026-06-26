#ifndef AUTO_RIG_H
#define AUTO_RIG_H

#include <QString>
#include <QJsonObject>
#include <QList>
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace Ogre {
    class Entity;
    class Mesh;
    class Skeleton;
}

// Native automatic rigging — predicts a skeleton for an unrigged mesh
// (issue #407, epic #397).
//
// The issue proposes wrapping **Pinocchio** (Baran & Popović, SIGGRAPH
// 2007). Pinocchio's *core library* is **LGPL-2.1-or-later** (only its
// demo CLI is MIT). Statically vendoring an LGPL library imposes
// relink / object-file obligations that conflict with this project's
// statically-linked, permissively-redistributed binaries (Homebrew /
// Snap / WinGet / Docker) and its permissive-license stance — the same
// reason #401 (Instant Meshes / QuadriFlow) and #402 (libigl BBW needs
// GPL TetGen) shipped native heuristics instead. Pinocchio's *algorithm*
// (embed a skeleton template into the mesh interior via a distance field)
// is published and unencumbered; only its code is LGPL, so this is a
// from-scratch native implementation of the approach with **zero new
// dependencies**.
//
// Algorithm (heuristic embedding):
//   1. Read mesh vertices → axis-aligned bounding box (AABB) + the up
//      axis (default +Y).
//   2. Each skeleton template is a proportional joint graph expressed in
//      a normalised unit box [0,1]^3 (origin = min corner, y = up).
//      Map every joint's normalised position into the mesh AABB.
//   3. Refine: for each joint, recentre it toward the mesh's mass at
//      that height by snapping its in-plane (non-up) coordinates to the
//      centroid of the vertices in a thin slab around the joint's up
//      coordinate. This pulls the spine onto the body's medial line and
//      lands limb roots inside the silhouette instead of on the AABB
//      shell. Joints whose slab is empty keep their AABB-proportional
//      position.
//
// The result is an Ogre::Skeleton in bind pose, ready to bind to the
// mesh and (optionally) feed into #402 SkinWeights for a one-click
// rig + skin. **Quality limits** (documented per the issue): like
// Pinocchio, this works best on roughly upright, single-component,
// manifold, T/A-pose meshes whose up axis is +Y. It is a heuristic — it
// does not detect limbs from topology, so exotic proportions or non-
// upright poses can misplace joints.

class AutoRig {
public:
    // Built-in skeleton templates.
    enum class Template {
        Humanoid,    // pelvis/spine/head + 2 arms + 2 legs (≈ Mixamo-lite)
        Biped,       // simplified humanoid: spine + 2 legs + stub arms
        Quadruped,   // spine + 4 legs + head + tail
        Generic      // a simple 3-joint spine chain (fallback for anything)
    };

    // Skeleton-prediction backend (issue #408).
    //   Pinocchio — the native template-embedding heuristic (#407). Default:
    //               zero deps, fully offline, deterministic.
    //   UniRig    — ML model (UniRig, SIGGRAPH 2025, MIT) via ONNX Runtime
    //               (#404 infra). An autoregressive transformer that predicts a
    //               skeleton from the mesh geometry, handling arbitrary topology
    //               / non-humanoid shapes better than a fixed template. Needs
    //               ENABLE_ONNX + a first-run model download; FALLS BACK to
    //               Pinocchio when the model or ONNX runtime is unavailable (the
    //               report records which backend actually ran + the fallback
    //               reason).
    enum class Algorithm {
        Pinocchio,
        UniRig
    };

    // One joint of a template / placed skeleton.
    struct Joint {
        QString name;
        int     parent = -1;             // index into the joint list (-1 = root)
        // For a TEMPLATE: normalised position in the unit box [0,1]^3.
        // For a PLACED skeleton: world-space position in mesh local space.
        std::array<double, 3> pos = {0, 0, 0};
        // When true, the refinement step recentres this joint's in-plane
        // coords toward the mesh slab centroid (spine/limb-root joints).
        // When false, the joint keeps its proportional position (e.g. the
        // tip of a limb, which should reach toward the AABB edge).
        bool recenter = true;
    };

    struct Options {
        // NOTE: declared (not defined) here so `Options{}` default args on the
        // member functions below don't force aggregate init of this nested
        // struct while the enclosing AutoRig class is still incomplete (which
        // GCC rejects: "default member initializer for 'tmpl' needed ...").
        Options();
        Template tmpl = Template::Humanoid;
        // Skeleton-prediction backend. Default Pinocchio (offline, deterministic).
        Algorithm algorithm = Algorithm::Pinocchio;
        // Up axis: 0=X, 1=Y, 2=Z. Default +Y (the in-app / glTF / FBX
        // convention after import normalisation).
        int upAxis = 1;
        // Slab half-thickness for the centroid recentre, as a fraction of
        // the mesh extent along the up axis. Larger = smoother spine,
        // less responsive to local mass. Range (0, 0.5]; default 0.06.
        double slabFraction = 0.06;
        // Pre-predicted joints (mesh-local). When non-empty AND algorithm is
        // UniRig, rigEntity SKIPS the (slow, ONNX) prediction and builds the
        // Ogre skeleton directly from these. The GUI uses this to run UniRig
        // inference on a worker thread (off the UI thread, with a progress bar)
        // and then build the skeleton on the main thread. Empty = predict inline.
        std::vector<Joint> prePredictedJoints;
    };

    // Mixamo-style placement markers (humanoid). The user clicks these on the
    // mesh surface; the marker positions anchor the corresponding joints and
    // the limb chains interpolate between them, so the rig follows the actual
    // body proportions instead of a fixed proportional template. Every marker
    // is OPTIONAL — an unset marker leaves its joint(s) at the template fit.
    enum class MarkerId {
        Chin,           // anchors Head; spine/neck interpolate Hips→Chin
        LeftShoulder,   // anchors LeftShoulder (arm-chain attach point)
        RightShoulder,  // anchors RightShoulder
        LeftWrist,      // anchors LeftHand  (+ LeftArm/LeftForeArm chain)
        RightWrist,     // anchors RightHand (+ RightArm/RightForeArm chain)
        LeftUpLeg,      // anchors LeftUpLeg (leg-chain attach / hip socket)
        RightUpLeg,     // anchors RightUpLeg
        LeftKnee,       // anchors LeftLeg   (+ LeftFoot extrapolated)
        RightKnee,      // anchors RightLeg  (+ RightFoot extrapolated)
        Hips,           // anchors Hips (pelvis height/centre)
        Count
    };

    struct Marker {
        MarkerId id = MarkerId::Count;
        bool set = false;                 // false = not placed → joint uses template
        std::array<double, 3> pos = {0, 0, 0};   // mesh-local position
    };

    // Stable label for a marker slot (UI + tests).
    static QString markerLabel(MarkerId id);
    // The ordered marker set the humanoid flow asks for (10 markers).
    static std::vector<MarkerId> humanoidMarkerOrder();

    struct Report {
        QString meshName;
        QString skeletonName;
        QString templateName;
        int     boneCount         = 0;
        int     verticesSampled   = 0;
        int     jointsRecentered  = 0;
        int     markersApplied    = 0;     // how many placed markers drove the fit
        // Which backend actually produced the skeleton (RigNet falls back to
        // Pinocchio when its model / ONNX runtime is unavailable).
        Algorithm algorithmUsed   = Algorithm::Pinocchio;
        // Set when the requested algorithm wasn't usable and we fell back
        // (empty when the requested algorithm ran). Surfaced to the user.
        QString  fallbackReason;
        bool     applied          = false;
        QString  error;
    };

    // --- Ogre-facing entry point (CLI / MCP / GUI) -----------------------

    // Generate a skeleton from `opts.tmpl`, fit it to `entity`'s mesh,
    // bind it (mesh->_notifySkeleton + setBindingPose), and return a
    // report. The entity must be a static (skeleton-less) mesh — an
    // already-rigged mesh returns applied=false with an error (unless
    // it has no usable geometry). After this returns applied=true, the
    // caller may chain SkinWeights::computeAndApply(entity) for weights.
    static Report rigEntity(Ogre::Entity* entity, const Options& opts = {});

    // --- Threaded UniRig helpers (GUI worker path) -----------------------
    // Split so the GUI can run the slow ONNX inference OFF the UI thread while
    // keeping Ogre access on the main thread:
    //   1. gatherGeometry(entity, ...) — MAIN thread: reads the mesh's vertex
    //      positions + triangle indices (locks Ogre HW buffers) into plain
    //      vectors. Returns false if there's no readable geometry.
    //   2. predictUniRig(verts, indices, upAxis, progress) — WORKER thread:
    //      pure ONNX (no Ogre), returns predicted joints (empty on failure/
    //      cancel; `outError` gets the reason). Feed the joints back via
    //      Options::prePredictedJoints + rigEntity() on the MAIN thread.
    static bool gatherGeometry(Ogre::Entity* entity,
                               std::vector<float>& outVerts,
                               std::vector<uint32_t>& outIndices);
    static std::vector<Joint> predictUniRig(
        const std::vector<float>& verts,
        const std::vector<uint32_t>& indices,
        int upAxis,
        const std::function<bool(int,int)>& progress,
        QString* outError);

    // Marker-guided variant: same as rigEntity but anchors the placed markers
    // (and interpolates the limb chains between them) before building the
    // skeleton. Markers are in mesh-local space. Unset markers fall back to the
    // proportional template fit. report.markersApplied counts the placed ones.
    static Report rigEntityWithMarkers(Ogre::Entity* entity,
                                       const std::vector<Marker>& markers,
                                       const Options& opts = {});

    // Revert a mesh auto-rigged by rigEntity[WithMarkers] back to a static
    // (skeleton-less) mesh: clears every submesh's (and the shared) bone
    // assignments, detaches the skeleton, re-initialises the entity, and
    // removes the `*_autorig` SkeletonManager resource. This is the undo
    // primitive for AutoRigCommand — it only makes sense for a mesh that was
    // static before rigging (which is the only thing auto-rig accepts), so it
    // unconditionally strips the skeleton rather than restoring a prior one.
    // Returns true if a skeleton was present and removed.
    static bool unrigEntity(Ogre::Entity* entity);

    // --- Pure-data core (unit-testable, no Ogre) -------------------------

    // The proportional joint graph for a template (positions in [0,1]^3).
    static std::vector<Joint> templateJoints(Template tmpl);

    // Fit `templateJoints` to a vertex cloud: map into the AABB, then
    // recentre toward per-slab centroids. `vertexPositions` is tightly
    // packed xyz (3 floats per vertex). Returns placed joints in the
    // same order/parenting as the template, positions now in mesh local
    // space. `outRecentered` (optional) receives the count of joints
    // that were recentred against a non-empty slab.
    static std::vector<Joint> fitTemplate(const std::vector<Joint>& tmpl,
                                           const float* vertexPositions,
                                           int vertexCount,
                                           const Options& opts,
                                           int* outRecentered = nullptr);

    // Marker-driven fit: runs fitTemplate, then anchors the placed markers and
    // interpolates the limb chains between them (unset markers keep the
    // template fit). `outMarkersApplied` (optional) receives how many set
    // markers actually drove a joint. Pure-data — the heart of the marker flow.
    static std::vector<Joint> fitTemplateWithMarkers(const std::vector<Joint>& tmpl,
                                                     const float* vertexPositions,
                                                     int vertexCount,
                                                     const std::vector<Marker>& markers,
                                                     const Options& opts,
                                                     int* outRecentered = nullptr,
                                                     int* outMarkersApplied = nullptr);

    static QString    templateToString(Template t);
    static Template   templateFromString(const QString& s);
    static QString    algorithmToString(Algorithm a);   // "pinocchio" | "rignet"
    static Algorithm  algorithmFromString(const QString& s);  // unknown → Pinocchio
    static QJsonObject reportToJson(const Report& r);
    static QString     reportToText(const Report& r);
};

#endif // AUTO_RIG_H
