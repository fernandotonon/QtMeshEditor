#include "AutoRig.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <string>

#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>
#include <OgreSkeleton.h>
#include <OgreSkeletonManager.h>
#include <OgreBone.h>
#include <OgreVertexIndexData.h>
#include <OgreHardwareBufferManager.h>
#include <OgreResourceGroupManager.h>

namespace {

// A template joint literal: name, parent index, normalised x/y/z in
// [0,1]^3 (y = up), and whether the refinement step recentres it.
struct TJ { const char* name; int parent; double x, y, z; bool recenter; };

// --- Skeleton templates -----------------------------------------------------
//
// Positions are in a normalised unit box: x in [0,1] left→right, y in
// [0,1] down→up, z in [0,1] back→front. The mesh's actual up axis is
// remapped from +Y at fit time via Options::upAxis. 0.5 is centre.

// Humanoid (≈ Mixamo-lite): pelvis → spine → chest → neck → head, plus
// symmetric shoulder/arm and hip/leg chains. Limb tips keep their
// proportional position (recenter=false) so they reach to the silhouette.
const TJ kHumanoid[] = {
    {"Hips",        -1, 0.50, 0.52, 0.50, true},
    {"Spine",        0, 0.50, 0.62, 0.50, true},
    {"Chest",        1, 0.50, 0.72, 0.50, true},
    {"Neck",         2, 0.50, 0.84, 0.50, true},
    {"Head",         3, 0.50, 0.92, 0.50, true},
    // Left arm (model's left = +x).
    {"LeftShoulder", 2, 0.60, 0.78, 0.50, true},
    {"LeftArm",      5, 0.70, 0.78, 0.50, false},
    {"LeftForeArm",  6, 0.82, 0.78, 0.50, false},
    {"LeftHand",     7, 0.93, 0.78, 0.50, false},
    // Right arm (-x).
    {"RightShoulder",2, 0.40, 0.78, 0.50, true},
    {"RightArm",     9, 0.30, 0.78, 0.50, false},
    {"RightForeArm",10, 0.18, 0.78, 0.50, false},
    {"RightHand",   11, 0.07, 0.78, 0.50, false},
    // Left leg.
    {"LeftUpLeg",    0, 0.58, 0.50, 0.50, true},
    {"LeftLeg",     13, 0.58, 0.27, 0.50, false},
    {"LeftFoot",    14, 0.58, 0.04, 0.55, false},
    // Right leg.
    {"RightUpLeg",   0, 0.42, 0.50, 0.50, true},
    {"RightLeg",    16, 0.42, 0.27, 0.50, false},
    {"RightFoot",   17, 0.42, 0.04, 0.55, false},
};

// Biped: spine + 2 legs + short arm stubs (simpler/cheaper than humanoid).
const TJ kBiped[] = {
    {"Hips",      -1, 0.50, 0.52, 0.50, true},
    {"Spine",      0, 0.50, 0.68, 0.50, true},
    {"Head",       1, 0.50, 0.90, 0.50, true},
    {"LeftArm",    1, 0.68, 0.74, 0.50, false},
    {"RightArm",   1, 0.32, 0.74, 0.50, false},
    {"LeftUpLeg",  0, 0.58, 0.50, 0.50, true},
    {"LeftFoot",   5, 0.58, 0.04, 0.55, false},
    {"RightUpLeg", 0, 0.42, 0.50, 0.50, true},
    {"RightFoot",  7, 0.42, 0.04, 0.55, false},
};

// Quadruped: a horizontal spine (front→back along +z), 4 legs, head, tail.
// Body lies low; "up" is still +y. Front of the body = high z.
const TJ kQuadruped[] = {
    {"SpineFront",  -1, 0.50, 0.55, 0.70, true},
    {"SpineMid",     0, 0.50, 0.55, 0.50, true},
    {"SpineBack",    1, 0.50, 0.55, 0.30, true},
    {"Neck",         0, 0.50, 0.62, 0.82, true},
    {"Head",         3, 0.50, 0.66, 0.95, true},
    {"Tail",         2, 0.50, 0.55, 0.08, false},
    // Front legs (high z).
    {"FrontLeftUpLeg",  0, 0.62, 0.45, 0.72, true},
    {"FrontLeftFoot",   6, 0.62, 0.04, 0.72, false},
    {"FrontRightUpLeg", 0, 0.38, 0.45, 0.72, true},
    {"FrontRightFoot",  8, 0.38, 0.04, 0.72, false},
    // Back legs (low z).
    {"BackLeftUpLeg",   2, 0.62, 0.45, 0.30, true},
    {"BackLeftFoot",   10, 0.62, 0.04, 0.30, false},
    {"BackRightUpLeg",  2, 0.38, 0.45, 0.30, true},
    {"BackRightFoot",  12, 0.38, 0.04, 0.30, false},
};

// Generic fallback: a 3-joint vertical spine. Always succeeds.
const TJ kGeneric[] = {
    {"Root",  -1, 0.50, 0.05, 0.50, true},
    {"Spine",  0, 0.50, 0.50, 0.50, true},
    {"Top",    1, 0.50, 0.95, 0.50, true},
};

std::vector<AutoRig::Joint> toJoints(const TJ* arr, size_t n)
{
    std::vector<AutoRig::Joint> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        AutoRig::Joint j;
        j.name      = QString::fromUtf8(arr[i].name);
        j.parent    = arr[i].parent;
        j.pos       = {arr[i].x, arr[i].y, arr[i].z};
        j.recenter  = arr[i].recenter;
        out.push_back(std::move(j));
    }
    return out;
}

} // namespace

// Out-of-line so the {} default args on the static methods resolve to a
// constructor call (not class-definition-time aggregate init). The member
// initializers in the header supply the actual default values.
AutoRig::Options::Options() = default;

std::vector<AutoRig::Joint> AutoRig::templateJoints(Template tmpl)
{
    switch (tmpl) {
        case Template::Humanoid:  return toJoints(kHumanoid,  std::size(kHumanoid));
        case Template::Biped:     return toJoints(kBiped,     std::size(kBiped));
        case Template::Quadruped: return toJoints(kQuadruped, std::size(kQuadruped));
        case Template::Generic:   return toJoints(kGeneric,   std::size(kGeneric));
    }
    return toJoints(kGeneric, std::size(kGeneric));
}

std::vector<AutoRig::Joint> AutoRig::fitTemplate(const std::vector<Joint>& tmpl,
                                                 const float* verts,
                                                 int vertexCount,
                                                 const Options& opts,
                                                 int* outRecentered)
{
    std::vector<Joint> placed = tmpl;
    if (outRecentered) *outRecentered = 0;
    if (!verts || vertexCount <= 0 || tmpl.empty()) return placed;

    // 1. AABB of the vertex cloud.
    double mn[3] = { 1e300,  1e300,  1e300};
    double mx[3] = {-1e300, -1e300, -1e300};
    for (int i = 0; i < vertexCount; ++i) {
        for (int a = 0; a < 3; ++a) {
            const double v = verts[3 * i + a];
            mn[a] = std::min(mn[a], v);
            mx[a] = std::max(mx[a], v);
        }
    }
    double ext[3];
    for (int a = 0; a < 3; ++a) ext[a] = std::max(1e-9, mx[a] - mn[a]);

    const int up = std::clamp(opts.upAxis, 0, 2);
    // The two in-plane axes (everything that isn't "up").
    const int p0 = (up == 0) ? 1 : 0;
    const int p1 = (up == 2) ? 1 : 2;

    // The template's y coordinate is "up"; its x,z are the in-plane axes.
    // Map template axis -> world axis so the box orients to the mesh's up.
    auto tmplAxisToWorld = [&](int tAxis) {
        // tAxis: 0=template-x, 1=template-y(up), 2=template-z
        if (tAxis == 1) return up;
        return (tAxis == 0) ? p0 : p1;
    };

    // 2. Map each joint's normalised position into the AABB.
    for (auto& j : placed) {
        std::array<double, 3> world = {0, 0, 0};
        for (int tAxis = 0; tAxis < 3; ++tAxis) {
            const int w = tmplAxisToWorld(tAxis);
            world[w] = mn[w] + j.pos[tAxis] * ext[w];
        }
        j.pos = world;
    }

    // 3. Recentre flagged joints toward the mesh's in-plane mass at their
    //    up-height (pulls the spine onto the medial line, lands limb roots
    //    inside the silhouette).
    const double slab = std::clamp(opts.slabFraction, 1e-3, 0.5) * ext[up];
    int recentered = 0;
    for (auto& j : placed) {
        if (!j.recenter) continue;
        const double y = j.pos[up];
        double sum0 = 0, sum1 = 0;
        long long n = 0;
        for (int i = 0; i < vertexCount; ++i) {
            if (std::abs(static_cast<double>(verts[3 * i + up]) - y) > slab) continue;
            sum0 += verts[3 * i + p0];
            sum1 += verts[3 * i + p1];
            ++n;
        }
        if (n > 0) {
            // Blend toward the slab centroid (0.75) but keep a little of the
            // template's lateral intent so symmetric joints don't all collapse
            // onto the exact centre line.
            const double c0 = sum0 / static_cast<double>(n);
            const double c1 = sum1 / static_cast<double>(n);
            const double kBlend = 0.75;
            j.pos[p0] = kBlend * c0 + (1.0 - kBlend) * j.pos[p0];
            j.pos[p1] = kBlend * c1 + (1.0 - kBlend) * j.pos[p1];
            ++recentered;
        }
    }
    if (outRecentered) *outRecentered = recentered;
    return placed;
}

QString AutoRig::markerLabel(MarkerId id)
{
    switch (id) {
        case MarkerId::Chin:          return QStringLiteral("Chin");
        case MarkerId::LeftShoulder:  return QStringLiteral("Left shoulder");
        case MarkerId::RightShoulder: return QStringLiteral("Right shoulder");
        case MarkerId::LeftWrist:     return QStringLiteral("Left wrist");
        case MarkerId::RightWrist:    return QStringLiteral("Right wrist");
        case MarkerId::LeftUpLeg:     return QStringLiteral("Left hip");
        case MarkerId::RightUpLeg:    return QStringLiteral("Right hip");
        case MarkerId::LeftKnee:      return QStringLiteral("Left knee");
        case MarkerId::RightKnee:     return QStringLiteral("Right knee");
        case MarkerId::Hips:          return QStringLiteral("Hips");
        case MarkerId::Count:         break;
    }
    return QStringLiteral("?");
}

std::vector<AutoRig::MarkerId> AutoRig::humanoidMarkerOrder()
{
    // Order = top-down, then limbs: chin, both shoulders, both wrists, both
    // hips (thigh roots), both knees, pelvis. Shoulders precede wrists, and the
    // hip sockets precede knees, so each limb chain has its attach point placed
    // before its tip. Pelvis (Hips) last so it can carry any unmarked thigh
    // roots along without overriding ones the user pinned.
    return { MarkerId::Chin,
             MarkerId::LeftShoulder, MarkerId::RightShoulder,
             MarkerId::LeftWrist, MarkerId::RightWrist,
             MarkerId::LeftUpLeg, MarkerId::RightUpLeg,
             MarkerId::LeftKnee, MarkerId::RightKnee,
             MarkerId::Hips };
}

namespace {

// Find a placed joint by name; returns nullptr if absent.
AutoRig::Joint* findJoint(std::vector<AutoRig::Joint>& js, const char* name)
{
    for (auto& j : js)
        if (j.name == QLatin1String(name)) return &j;
    return nullptr;
}

// Place `mid` between `a` and `b` at parameter t (0=a, 1=b).
std::array<double, 3> lerp3(const std::array<double, 3>& a,
                            const std::array<double, 3>& b, double t)
{
    return { a[0] + (b[0] - a[0]) * t,
             a[1] + (b[1] - a[1]) * t,
             a[2] + (b[2] - a[2]) * t };
}

// Lay an N-joint limb chain straight along anchor→marker. `names` is the
// chain in parent→child order; the FIRST joint (the anchor — e.g. the
// shoulder) keeps its template position, the LAST goes to the marker, and
// every joint in between is distributed evenly by index (a straight rest-pose
// limb). Distributing ALL the intermediate joints — not just one midpoint —
// is what makes the whole limb reach toward the marker; anchoring only the
// tip + a single mid leaves the upper segment tucked at its template position.
// Any named joint that's missing is skipped (the rest still lay out from the
// surviving anchor/tip).
void layChain(std::vector<AutoRig::Joint>& js,
              std::initializer_list<const char*> names,
              const std::array<double, 3>& tipMarker)
{
    if (names.size() < 2) return;
    AutoRig::Joint* anchor = findJoint(js, *names.begin());
    if (!anchor) return;
    const auto a = anchor->pos;             // copy: stays put, drives the lerp
    const int last = static_cast<int>(names.size()) - 1;
    int i = 0;
    for (const char* n : names) {
        if (i > 0) {                        // i==0 is the anchor; leave it
            if (auto* j = findJoint(js, n))
                j->pos = lerp3(a, tipMarker, static_cast<double>(i) / last);
        }
        ++i;
    }
}

} // namespace

std::vector<AutoRig::Joint> AutoRig::fitTemplateWithMarkers(
        const std::vector<Joint>& tmpl,
        const float* verts, int vertexCount,
        const std::vector<Marker>& markers,
        const Options& opts,
        int* outRecentered, int* outMarkersApplied)
{
    // Start from the proportional fit; markers refine it.
    std::vector<Joint> placed = fitTemplate(tmpl, verts, vertexCount, opts, outRecentered);
    if (outMarkersApplied) *outMarkersApplied = 0;

    auto get = [&](MarkerId id) -> const Marker* {
        for (const auto& m : markers)
            if (m.id == id && m.set) return &m;
        return nullptr;
    };
    int applied = 0;

    // Hips: anchor the pelvis directly, and carry the thigh roots
    // (LeftUpLeg / RightUpLeg — children of Hips in the template) along with it
    // by the same delta, so marking the hips moves the whole pelvis+thigh-root
    // cluster as a unit instead of leaving the thighs floating at their
    // template position. (An explicit L/R-hip marker below overrides its root.)
    if (const Marker* m = get(MarkerId::Hips)) {
        if (auto* hips = findJoint(placed, "Hips")) {
            const std::array<double,3> d = { m->pos[0] - hips->pos[0],
                                             m->pos[1] - hips->pos[1],
                                             m->pos[2] - hips->pos[2] };
            hips->pos = m->pos;
            for (const char* leg : {"LeftUpLeg", "RightUpLeg"}) {
                if (auto* j = findJoint(placed, leg))
                    j->pos = { j->pos[0]+d[0], j->pos[1]+d[1], j->pos[2]+d[2] };
            }
            ++applied;
        }
    }
    // Chin: anchor Head, then lay the SPINE straight from the pelvis up to the
    // head so the torso follows the marked hips↔chin span instead of leaving
    // Spine/Chest/Neck stranded at their template heights. The spine joints are
    // distributed evenly between Hips and Head by index (cartoon torsos vary a
    // lot in length, so a proportional template guess is usually wrong).
    if (const Marker* m = get(MarkerId::Chin)) {
        if (auto* head = findJoint(placed, "Head")) {
            head->pos = m->pos;
            ++applied;
            // Anchor at the (marked-or-template) Hips; lay Spine→Chest→Neck→Head.
            if (auto* hips = findJoint(placed, "Hips")) {
                // Spine chain joints in parent→child order, Head is the tip.
                static const char* kSpine[] =
                    { "Spine", "Chest", "Neck" };  // between Hips and Head
                const auto a = hips->pos;
                const int last = static_cast<int>(std::size(kSpine)) + 1; // +Head
                for (int i = 0; i < static_cast<int>(std::size(kSpine)); ++i) {
                    if (auto* j = findJoint(placed, kSpine[i]))
                        j->pos = lerp3(a, m->pos,
                                       static_cast<double>(i + 1) / last);
                }
            } else if (auto* neck = findJoint(placed, "Neck")) {
                // No hips reference — fall back to the old neck lift.
                if (auto* chest = findJoint(placed, "Chest"))
                    neck->pos = lerp3(chest->pos, m->pos, 0.5);
            }
        }
    }
    // Shoulders: anchor the arm-chain attach point. Applied BEFORE the wrist
    // chains so layChain (which uses the shoulder as its fixed anchor) lays the
    // arm out from the marked shoulder rather than the template one. A shoulder
    // marker on its own (no wrist) still repositions the attach point.
    if (const Marker* m = get(MarkerId::LeftShoulder)) {
        if (auto* j = findJoint(placed, "LeftShoulder")) { j->pos = m->pos; ++applied; }
    }
    if (const Marker* m = get(MarkerId::RightShoulder)) {
        if (auto* j = findJoint(placed, "RightShoulder")) { j->pos = m->pos; ++applied; }
    }
    // Arms: the wrist marker is the hand position. Lay the whole arm chain
    // straight from the SHOULDER (anchor — its marked-or-template position) out
    // to the marker — LeftShoulder → LeftArm → LeftForeArm → LeftHand(=marker) —
    // distributing the upper-arm/forearm joints along the way so the entire
    // arm reaches toward the wrist, not just the hand.
    if (const Marker* m = get(MarkerId::LeftWrist)) {
        layChain(placed, {"LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand"}, m->pos);
        ++applied;
    }
    if (const Marker* m = get(MarkerId::RightWrist)) {
        layChain(placed, {"RightShoulder", "RightArm", "RightForeArm", "RightHand"}, m->pos);
        ++applied;
    }
    // Hip sockets: anchor each thigh root (UpLeg) at its marker. Applied BEFORE
    // the knee chains so layLeg lays the lower leg from the marked socket. This
    // OVERRIDES the hips-carried position above, so an explicit hip marker wins
    // (matters for cartoon models where the thighs splay out at odd angles a
    // template/pelvis-carry can't capture). A hip marker on its own (no knee)
    // still repositions the socket.
    if (const Marker* m = get(MarkerId::LeftUpLeg)) {
        if (auto* j = findJoint(placed, "LeftUpLeg")) { j->pos = m->pos; ++applied; }
    }
    if (const Marker* m = get(MarkerId::RightUpLeg)) {
        if (auto* j = findJoint(placed, "RightUpLeg")) { j->pos = m->pos; ++applied; }
    }
    // Legs: the knee marker is the knee (LeftLeg) position. The thigh root
    // (UpLeg) is the anchor — it sits at the hip socket (its marked position,
    // else carried by the hips marker, else the template fit). Anchor the knee
    // at the marker
    // and continue the foot below it along the thigh→knee direction (~equal
    // length), so the WHOLE leg — thigh root → knee → foot — lays out to follow
    // the marked hips + knee instead of leaving the upper leg at its template
    // position. (UpLeg→Leg is a 2-joint segment: anchor + tip, so layChain
    // would just set the knee; we keep the explicit form to also place the
    // extrapolated foot.)
    auto layLeg = [&](const char* up, const char* knee, const char* foot,
                      const std::array<double, 3>& kneePos) {
        AutoRig::Joint* hip = findJoint(placed, up);
        AutoRig::Joint* kn  = findJoint(placed, knee);
        if (!hip || !kn) return;
        kn->pos = kneePos;
        // Foot continues below the knee, same direction as thigh→knee, ~equal len.
        if (auto* ft = findJoint(placed, foot)) {
            const auto d = std::array<double,3>{ kneePos[0]-hip->pos[0],
                                                 kneePos[1]-hip->pos[1],
                                                 kneePos[2]-hip->pos[2] };
            ft->pos = { kneePos[0] + d[0], kneePos[1] + d[1], kneePos[2] + d[2] };
        }
    };
    if (const Marker* m = get(MarkerId::LeftKnee))  { layLeg("LeftUpLeg",  "LeftLeg",  "LeftFoot",  m->pos); ++applied; }
    if (const Marker* m = get(MarkerId::RightKnee)) { layLeg("RightUpLeg", "RightLeg", "RightFoot", m->pos); ++applied; }

    if (outMarkersApplied) *outMarkersApplied = applied;
    return placed;
}

namespace {

// Tightly read POSITION floats out of a VertexData (same idiom as
// SkinWeights::extractPositions). Appends to `out`.
bool appendPositions(Ogre::VertexData* vd, std::vector<float>& out)
{
    if (!vd) return false;
    const auto* posElem =
        vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (!posElem) return false;
    auto vbuf = vd->vertexBufferBinding->getBuffer(posElem->getSource());
    if (!vbuf || vd->vertexCount == 0) return false;
    const size_t base0 = out.size();
    out.resize(base0 + static_cast<size_t>(vd->vertexCount) * 3);
    const size_t stride = vbuf->getVertexSize();
    auto* base = static_cast<unsigned char*>(
        vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    if (!base) {
        // Lock can fail (write-only buffer with no shadow copy, etc.). Shrink
        // back to the pre-grow size so the unread slots don't inflate vcount.
        out.resize(base0);
        return false;
    }
    for (size_t i = 0; i < vd->vertexCount; ++i) {
        float* p = nullptr;
        posElem->baseVertexPointerToElement(base + i * stride, &p);
        out[base0 + 3 * i + 0] = p[0];
        out[base0 + 3 * i + 1] = p[1];
        out[base0 + 3 * i + 2] = p[2];
    }
    vbuf->unlock();
    return true;
}

} // namespace

AutoRig::Report AutoRig::rigEntity(Ogre::Entity* entity, const Options& opts)
{
    return rigEntityWithMarkers(entity, /*markers=*/{}, opts);
}

AutoRig::Report AutoRig::rigEntityWithMarkers(Ogre::Entity* entity,
                                              const std::vector<Marker>& markers,
                                              const Options& opts)
{
    Report report;
    report.templateName = templateToString(opts.tmpl);

    if (!entity || !entity->getMesh()) {
        report.error = QStringLiteral("no mesh to rig");
        return report;
    }
    Ogre::MeshPtr mesh = entity->getMesh();
    report.meshName = QString::fromStdString(mesh->getName());

    if (mesh->hasSkeleton()) {
        report.error = QStringLiteral(
            "mesh already has a skeleton — auto-rig only applies to unrigged "
            "(static) meshes");
        return report;
    }

    // Gather all vertex positions (shared + per-submesh).
    std::vector<float> verts;
    if (mesh->sharedVertexData) appendPositions(mesh->sharedVertexData, verts);
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (sub && !sub->useSharedVertices && sub->vertexData)
            appendPositions(sub->vertexData, verts);
    }
    const int vcount = static_cast<int>(verts.size() / 3);
    if (vcount == 0) {
        report.error = QStringLiteral("mesh has no readable vertex positions");
        return report;
    }
    report.verticesSampled = vcount;

    // Fit the template — marker-driven when markers are supplied, else the
    // plain proportional fit.
    int recentered = 0, markersApplied = 0;
    const std::vector<Joint> tmpl = templateJoints(opts.tmpl);
    const std::vector<Joint> placed = markers.empty()
        ? fitTemplate(tmpl, verts.data(), vcount, opts, &recentered)
        : fitTemplateWithMarkers(tmpl, verts.data(), vcount, markers, opts,
                                 &recentered, &markersApplied);
    report.jointsRecentered = recentered;
    report.markersApplied   = markersApplied;

    // Build the Ogre skeleton. Bone POSITIONS are parent-relative in Ogre,
    // so each child's setPosition is its world pos minus its parent's world
    // pos. createBone(name, handle) — handle == index.
    auto& skelMgr = Ogre::SkeletonManager::getSingleton();
    const std::string skelName = mesh->getName() + "_autorig";
    if (skelMgr.resourceExists(skelName))
        skelMgr.remove(skelName);
    Ogre::SkeletonPtr skel;
    try {
        skel = skelMgr.create(
            skelName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        std::vector<Ogre::Bone*> bones(placed.size(), nullptr);
        for (size_t i = 0; i < placed.size(); ++i)
            bones[i] = skel->createBone(placed[i].name.toStdString(),
                                        static_cast<unsigned short>(i));
        for (size_t i = 0; i < placed.size(); ++i) {
            const Joint& j = placed[i];
            Ogre::Vector3 local(
                static_cast<float>(j.pos[0]),
                static_cast<float>(j.pos[1]),
                static_cast<float>(j.pos[2]));
            if (j.parent >= 0 && static_cast<size_t>(j.parent) < placed.size()) {
                bones[j.parent]->addChild(bones[i]);
                const Joint& pj = placed[j.parent];
                local -= Ogre::Vector3(
                    static_cast<float>(pj.pos[0]),
                    static_cast<float>(pj.pos[1]),
                    static_cast<float>(pj.pos[2]));
            }
            bones[i]->setPosition(local);
            bones[i]->setOrientation(Ogre::Quaternion::IDENTITY);
        }
        skel->setBindingPose();

        // Bind the skeleton to the mesh, then force the entity to
        // re-initialise so it acquires a SkeletonInstance. Without the
        // _initialise(true), the already-created Ogre::Entity keeps
        // hasSkeleton()==false and BOTH exporters (FBXExporter and the
        // Assimp glTF/FBX path gate on entity->hasSkeleton()) would drop
        // the new rig — the skeleton would exist on the mesh but never
        // reach the wire. (Same refresh EditableMesh / EditModeController
        // do after mutating an entity's mesh.)
        mesh->_notifySkeleton(skel);
        entity->_initialise(true);
        report.skeletonName = QString::fromStdString(skelName);
        report.boneCount    = static_cast<int>(placed.size());
        report.applied      = true;
    } catch (const Ogre::Exception& e) {
        report.error = QStringLiteral("Ogre error building skeleton: %1")
            .arg(QString::fromStdString(e.getFullDescription()));
        // Detach the half-built skeleton from the mesh BEFORE removing the
        // resource. _notifySkeleton(skel) ran before entity->_initialise; if
        // the latter threw, the mesh still references the skeleton, so
        // mesh->hasSkeleton() would stay true — a later rigEntity() would bail
        // with "mesh already has a skeleton" and exporters could pick up the
        // half-built rig. Reset it to a clean static mesh.
        mesh->_notifySkeleton(Ogre::SkeletonPtr());
        if (skel && skelMgr.resourceExists(skelName)) skelMgr.remove(skelName);
        report.applied = false;
    }
    return report;
}

bool AutoRig::unrigEntity(Ogre::Entity* entity)
{
    if (!entity || !entity->getMesh()) return false;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh->hasSkeleton()) return false;

    // Remember the skeleton resource name so we can free it after detaching.
    const std::string skelName = mesh->getSkeletonName();

    // Drop every bone assignment (shared + per-submesh) so the mesh carries no
    // stale weights once it's static again.
    mesh->clearBoneAssignments();
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        if (Ogre::SubMesh* sub = mesh->getSubMesh(si))
            sub->clearBoneAssignments();
    }

    // Detach the skeleton and force the entity back to its static form (mirror
    // of the rig path's _notifySkeleton + _initialise(true)).
    mesh->_notifySkeleton(Ogre::SkeletonPtr());
    entity->_initialise(true);

    auto& skelMgr = Ogre::SkeletonManager::getSingleton();
    if (!skelName.empty() && skelMgr.resourceExists(skelName))
        skelMgr.remove(skelName);
    return true;
}

QString AutoRig::templateToString(Template t)
{
    switch (t) {
        case Template::Humanoid:  return QStringLiteral("humanoid");
        case Template::Biped:     return QStringLiteral("biped");
        case Template::Quadruped: return QStringLiteral("quadruped");
        case Template::Generic:   return QStringLiteral("generic");
    }
    return QStringLiteral("generic");
}

AutoRig::Template AutoRig::templateFromString(const QString& s)
{
    const QString l = s.trimmed().toLower();
    if (l == "humanoid")  return Template::Humanoid;
    if (l == "biped")     return Template::Biped;
    if (l == "quadruped" || l == "quad") return Template::Quadruped;
    if (l == "generic")   return Template::Generic;
    return Template::Humanoid;   // default
}

QJsonObject AutoRig::reportToJson(const Report& r)
{
    QJsonObject o;
    o["applied"]          = r.applied;
    o["meshName"]         = r.meshName;
    o["skeletonName"]     = r.skeletonName;
    o["template"]         = r.templateName;
    o["boneCount"]        = r.boneCount;
    o["verticesSampled"]  = r.verticesSampled;
    o["jointsRecentered"] = r.jointsRecentered;
    if (!r.error.isEmpty()) o["error"] = r.error;
    return o;
}

QString AutoRig::reportToText(const Report& r)
{
    if (!r.applied)
        return QStringLiteral("Auto-rig failed: %1\n")
            .arg(r.error.isEmpty() ? QStringLiteral("unknown error") : r.error);
    return QStringLiteral(
        "Auto-rigged %1 with the '%2' template.\n"
        "  bones: %3\n  vertices sampled: %4\n  joints recentered: %5\n")
        .arg(r.meshName, r.templateName)
        .arg(r.boneCount).arg(r.verticesSampled).arg(r.jointsRecentered);
}
