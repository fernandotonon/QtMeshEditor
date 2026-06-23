#ifndef AUTO_RIG_H
#define AUTO_RIG_H

#include <QString>
#include <QJsonObject>
#include <QList>
#include <array>
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
        // Up axis: 0=X, 1=Y, 2=Z. Default +Y (the in-app / glTF / FBX
        // convention after import normalisation).
        int upAxis = 1;
        // Slab half-thickness for the centroid recentre, as a fraction of
        // the mesh extent along the up axis. Larger = smoother spine,
        // less responsive to local mass. Range (0, 0.5]; default 0.06.
        double slabFraction = 0.06;
    };

    struct Report {
        QString meshName;
        QString skeletonName;
        QString templateName;
        int     boneCount         = 0;
        int     verticesSampled   = 0;
        int     jointsRecentered  = 0;
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

    static QString    templateToString(Template t);
    static Template   templateFromString(const QString& s);
    static QJsonObject reportToJson(const Report& r);
    static QString     reportToText(const Report& r);
};

#endif // AUTO_RIG_H
