#include "AutoRig.h"
#include "UniRigPredictor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

// Find a placed joint's position by name; returns `fallback` if absent.
std::array<double,3> jointPosOr(const std::vector<AutoRig::Joint>& js,
                                const char* name,
                                const std::array<double,3>& fallback)
{
    for (const auto& j : js) if (j.name == QLatin1String(name)) return j.pos;
    return fallback;
}

std::array<double,3> add3(const std::array<double,3>& a, const std::array<double,3>& b)
{ return { a[0]+b[0], a[1]+b[1], a[2]+b[2] }; }
std::array<double,3> sub3(const std::array<double,3>& a, const std::array<double,3>& b)
{ return { a[0]-b[0], a[1]-b[1], a[2]-b[2] }; }

} // namespace

std::vector<AutoRig::Joint> AutoRig::fitTemplateWithMarkers(
        const std::vector<Joint>& tmpl,
        const float* verts, int vertexCount,
        const std::vector<Marker>& markers,
        const Options& opts,
        int* outRecentered, int* outMarkersApplied)
{
    // Proportional baseline — gives sensible default joint positions AND the
    // template relationships (segment vectors, lateral offsets) we use to
    // INFER unmarked joints from marked ones, so a partial marker set produces
    // a coherent skeleton (no shoulder-above-head etc.) instead of mixing
    // marked anchors with stranded template joints.
    std::vector<Joint> placed = fitTemplate(tmpl, verts, vertexCount, opts, outRecentered);
    if (outMarkersApplied) *outMarkersApplied = 0;

    auto get = [&](MarkerId id) -> const Marker* {
        for (const auto& m : markers)
            if (m.id == id && m.set) return &m;
        return nullptr;
    };

    // No markers placed → the proportional fit is the answer, untouched (keeps
    // the "empty marker set ≡ fitTemplate" contract; nothing to infer from).
    bool anySet = false;
    for (const auto& m : markers) if (m.set) { anySet = true; break; }
    if (!anySet) return placed;

    int applied = 0;

    // Mesh AABB (mesh-local space, same coords the fit works in) — used to
    // CLAMP inferred joints to the model's extent so an extrapolated limb
    // (e.g. up-leg set but knee skipped) can't shoot a foot below the mesh.
    std::array<double,3> mn = { 1e30,  1e30,  1e30};
    std::array<double,3> mx = {-1e30, -1e30, -1e30};
    for (int i = 0; i < vertexCount; ++i) {
        for (int a = 0; a < 3; ++a) {
            const double v = verts[3 * i + a];
            mn[a] = std::min(mn[a], v);
            mx[a] = std::max(mx[a], v);
        }
    }

    // ---- Template reference positions (the proportional fit) -------------
    const auto tHips  = jointPosOr(placed, "Hips",          {0,0,0});
    const auto tHead  = jointPosOr(placed, "Head",          tHips);
    const auto tLSh   = jointPosOr(placed, "LeftShoulder",  tHips);
    const auto tRSh   = jointPosOr(placed, "RightShoulder", tHips);
    const auto tLHand = jointPosOr(placed, "LeftHand",      tLSh);
    const auto tRHand = jointPosOr(placed, "RightHand",     tRSh);
    const auto tLUp   = jointPosOr(placed, "LeftUpLeg",     tHips);
    const auto tRUp   = jointPosOr(placed, "RightUpLeg",    tHips);
    const auto tLKnee = jointPosOr(placed, "LeftLeg",       tLUp);
    const auto tRKnee = jointPosOr(placed, "RightLeg",      tRUp);

    // Reflect a point across the body's sagittal plane (the plane through Hips
    // perpendicular to the side axis). Used to mirror a marked left limb onto
    // an unmarked right one (and vice-versa). The side axis is whichever of the
    // two non-up axes the template shoulders are most separated along.
    const int up = std::clamp(opts.upAxis, 0, 2);
    int sideAxis = (up == 0) ? 1 : 0;            // first non-up axis
    {
        const int a1 = (up == 0) ? 1 : 0;
        const int a2 = (up == 2) ? 1 : 2;
        if (std::abs(tLSh[a2] - tRSh[a2]) > std::abs(tLSh[a1] - tRSh[a1]))
            sideAxis = a2;
    }
    auto mirror = [&](std::array<double,3> p, const std::array<double,3>& center) {
        p[sideAxis] = center[sideAxis] - (p[sideAxis] - center[sideAxis]);
        return p;
    };

    // ---- Resolve anchor positions (marked → inferred → template) ---------
    // Each `resolve` records whether a USER marker drove it (for applied count).
    const Marker* mHead  = get(MarkerId::Chin);
    const Marker* mHips  = get(MarkerId::Hips);
    const Marker* mLSh   = get(MarkerId::LeftShoulder);
    const Marker* mRSh   = get(MarkerId::RightShoulder);
    const Marker* mLWr   = get(MarkerId::LeftWrist);
    const Marker* mRWr   = get(MarkerId::RightWrist);
    const Marker* mLUp   = get(MarkerId::LeftUpLeg);
    const Marker* mRUp   = get(MarkerId::RightUpLeg);
    const Marker* mLKn   = get(MarkerId::LeftKnee);
    const Marker* mRKn   = get(MarkerId::RightKnee);
    for (const Marker* m : {mHead,mHips,mLSh,mRSh,mLWr,mRWr,mLUp,mRUp,mLKn,mRKn})
        if (m) ++applied;

    // HIPS: marked → else from the up-legs (midpoint, lifted by the template
    // socket→pelvis rise) → else template.
    std::array<double,3> pHips = tHips;
    if (mHips) pHips = mHips->pos;
    else if (mLUp && mRUp) {
        pHips = { 0.5*(mLUp->pos[0]+mRUp->pos[0]),
                  0.5*(mLUp->pos[1]+mRUp->pos[1]),
                  0.5*(mLUp->pos[2]+mRUp->pos[2]) };
        const auto lift = sub3(tHips, { 0.5*(tLUp[0]+tRUp[0]),
                                        0.5*(tLUp[1]+tRUp[1]),
                                        0.5*(tLUp[2]+tRUp[2]) });
        pHips = add3(pHips, lift);
    }

    // HEAD (chin): marked → else template lifted to keep the marked-hips offset.
    std::array<double,3> pHead = mHead ? mHead->pos : add3(pHips, sub3(tHead, tHips));

    // UP-LEGS: marked → else mirror the other marked one across the pelvis →
    // else pelvis + template socket offset.
    std::array<double,3> pLUp, pRUp;
    pLUp = mLUp ? mLUp->pos : (mRUp ? mirror(mRUp->pos, pHips) : add3(pHips, sub3(tLUp, tHips)));
    pRUp = mRUp ? mRUp->pos : (mLUp ? mirror(mLUp->pos, pHips) : add3(pHips, sub3(tRUp, tHips)));

    // SHOULDERS: marked → else mirror the other → else from the spine: place at
    // the template's shoulder-height fraction along the live Hips→Head line,
    // plus the template lateral offset (so chin+hips imply the shoulders).
    auto shoulderFromSpine = [&](const std::array<double,3>& tSh) {
        const double denomUp = (tHead[up] - tHips[up]);
        const double f = std::abs(denomUp) > 1e-9
            ? (tSh[up] - tHips[up]) / denomUp : 0.78;
        std::array<double,3> p = lerp3(pHips, pHead, std::clamp(f, 0.0, 1.0));
        // lateral / depth offset of the template shoulder from the spine line
        const std::array<double,3> tSpineAtSh = lerp3(tHips, tHead, std::clamp(f,0.0,1.0));
        const auto off = sub3(tSh, tSpineAtSh);
        return add3(p, off);
    };
    std::array<double,3> pLSh, pRSh;
    pLSh = mLSh ? mLSh->pos : (mRSh ? mirror(mRSh->pos, pHead) : shoulderFromSpine(tLSh));
    pRSh = mRSh ? mRSh->pos : (mLSh ? mirror(mLSh->pos, pHead) : shoulderFromSpine(tRSh));

    // HANDS (wrist): marked → else shoulder + template arm vector (so a marked
    // shoulder with a skipped wrist still lays a full arm reaching out).
    std::array<double,3> pLHand, pRHand;
    pLHand = mLWr ? mLWr->pos : add3(pLSh, sub3(tLHand, tLSh));
    pRHand = mRWr ? mRWr->pos : add3(pRSh, sub3(tRHand, tRSh));

    // KNEES + FEET: resolve both, clamped to the mesh's lower extent so an
    // inferred leg never punches through the bottom of the model.
    //   * knee marked   → knee at the marker, foot extrapolated below
    //                     (knee + thigh→knee), then clamped to the floor.
    //   * knee unmarked  → drop the foot to the mesh FLOOR (mn[up]) straight
    //                     below the up-leg, and put the knee halfway between the
    //                     up-leg and that foot. (Template thigh-vector
    //                     extrapolation is what shot feet past the mesh limit;
    //                     anchoring the foot to the floor fixes that.)
    const double floorUp = mn[up];
    auto resolveLeg = [&](const std::array<double,3>& upPos,
                          const std::array<double,3>& tKnee,
                          const std::array<double,3>& tUp,
                          const Marker* kneeMk,
                          std::array<double,3>& knee,
                          std::array<double,3>& foot) {
        if (kneeMk) {
            knee = kneeMk->pos;
            foot = add3(knee, sub3(knee, upPos));   // continue below the knee
        } else {
            // Foot straight below the up-leg, sitting on the mesh floor.
            foot = upPos; foot[up] = floorUp;
            knee = { 0.5*(upPos[0]+foot[0]),
                     0.5*(upPos[1]+foot[1]),
                     0.5*(upPos[2]+foot[2]) };
            // Nudge the knee slightly forward (template thigh→knee in-plane
            // direction) so it isn't a perfectly straight, lockable line.
            const auto tIn = sub3(tKnee, tUp);
            for (int a = 0; a < 3; ++a) if (a != up) knee[a] += tIn[a] * 0.25;
        }
        // Never let the foot go below the mesh floor (clamp the up coord).
        if (foot[up] < floorUp) foot[up] = floorUp;
        // Keep the knee strictly between the up-leg and the foot in up-coord.
        const double lo = std::min(upPos[up], foot[up]);
        const double hi = std::max(upPos[up], foot[up]);
        knee[up] = std::clamp(knee[up], lo, hi);
    };
    std::array<double,3> pLKnee, pLFoot, pRKnee, pRFoot;
    resolveLeg(pLUp, tLKnee, tLUp, mLKn, pLKnee, pLFoot);
    resolveLeg(pRUp, tRKnee, tRUp, mRKn, pRKnee, pRFoot);

    // ---- Write the resolved anchors back, then lay the dependent chains --
    auto setJoint = [&](const char* name, const std::array<double,3>& p) {
        if (auto* j = findJoint(placed, name)) j->pos = p;
    };
    setJoint("Hips", pHips);
    setJoint("Head", pHead);
    setJoint("LeftShoulder", pLSh);
    setJoint("RightShoulder", pRSh);
    setJoint("LeftUpLeg", pLUp);
    setJoint("RightUpLeg", pRUp);

    // Spine: distribute Spine/Chest/Neck evenly between Hips and Head.
    {
        static const char* kSpine[] = { "Spine", "Chest", "Neck" };
        const int last = static_cast<int>(std::size(kSpine)) + 1;   // +Head
        for (int i = 0; i < static_cast<int>(std::size(kSpine)); ++i)
            setJoint(kSpine[i], lerp3(pHips, pHead,
                                      static_cast<double>(i + 1) / last));
    }
    // Arms: lay the full chain shoulder→arm→forearm→hand toward the resolved hand.
    layChain(placed, {"LeftShoulder",  "LeftArm",  "LeftForeArm",  "LeftHand"},  pLHand);
    layChain(placed, {"RightShoulder", "RightArm", "RightForeArm", "RightHand"}, pRHand);

    // Legs: write the resolved (and floor-clamped) knee + foot anchors.
    setJoint("LeftLeg",   pLKnee);  setJoint("LeftFoot",  pLFoot);
    setJoint("RightLeg",  pRKnee);  setJoint("RightFoot", pRFoot);

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

// Append a submesh's triangle indices (offset by `vertexBase`, the running
// shared/per-submesh vertex offset) so RigNet sees one combined index buffer
// over the same vertex order appendPositions produced. Only used for the
// RigNet path. Returns the number of vertices this submesh contributed (to
// advance vertexBase). Shared-vertex submeshes are still emitted but reference
// the shared block at offset 0.
int appendIndices(Ogre::SubMesh* sub, Ogre::Mesh* mesh,
                  uint32_t sharedBase, uint32_t ownBase,
                  std::vector<uint32_t>& out)
{
    if (!sub || !sub->indexData || !sub->indexData->indexBuffer) return 0;
    Ogre::IndexData* id = sub->indexData;
    const uint32_t base = sub->useSharedVertices ? sharedBase : ownBase;
    auto ibuf = id->indexBuffer;
    const bool is32 = ibuf->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;
    auto* p = static_cast<unsigned char*>(ibuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    if (!p) return 0;
    const auto* i32 = reinterpret_cast<const uint32_t*>(p);
    const auto* i16 = reinterpret_cast<const uint16_t*>(p);
    const size_t n = id->indexCount;
    out.reserve(out.size() + n);
    for (size_t k = 0; k + 2 < n; k += 3) {
        out.push_back(base + (is32 ? i32[k]   : i16[k]));
        out.push_back(base + (is32 ? i32[k+1] : i16[k+1]));
        out.push_back(base + (is32 ? i32[k+2] : i16[k+2]));
    }
    ibuf->unlock();
    return 0;
}

} // namespace

bool AutoRig::gatherGeometry(Ogre::Entity* entity,
                             std::vector<float>& outVerts,
                             std::vector<uint32_t>& outIndices)
{
    outVerts.clear();
    outIndices.clear();
    if (!entity || !entity->getMesh()) return false;
    Ogre::MeshPtr mesh = entity->getMesh();
    const uint32_t sharedBase = 0;
    uint32_t ownBase = 0;
    if (mesh->sharedVertexData) {
        appendPositions(mesh->sharedVertexData, outVerts);
        ownBase = static_cast<uint32_t>(mesh->sharedVertexData->vertexCount);
    }
    std::vector<uint32_t> ownOffset(mesh->getNumSubMeshes(), 0);
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (sub && !sub->useSharedVertices && sub->vertexData) {
            ownOffset[si] = ownBase;
            appendPositions(sub->vertexData, outVerts);
            ownBase += static_cast<uint32_t>(sub->vertexData->vertexCount);
        }
    }
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si)
        appendIndices(mesh->getSubMesh(si), mesh.get(), sharedBase, ownOffset[si], outIndices);
    return !outVerts.empty();
}

std::vector<AutoRig::Joint> AutoRig::predictUniRig(
        const std::vector<float>& verts,
        const std::vector<uint32_t>& indices,
        int upAxis,
        const std::function<bool(int,int)>& progress,
        QString* outError)
{
    std::vector<Joint> out;
    const int vcount = static_cast<int>(verts.size() / 3);
    if (vcount < 4) { if (outError) *outError = QStringLiteral("mesh has too few vertices"); return out; }
    if (!UniRigPredictor::isAvailable()) {
        if (outError) *outError = QStringLiteral("UniRig needs an ONNX-enabled build");
        return out;
    }
    const QString enc = UniRigPredictor::ensureModelBlocking();   // may download (worker thread)
    if (enc.isEmpty()) {
        if (outError) *outError = QStringLiteral("UniRig model unavailable (offline or not yet hosted)");
        return out;
    }
    UniRigPredictor::Options rnOpts;
    rnOpts.upAxis = upAxis;
    const auto rn = UniRigPredictor::predict(
        verts.data(), vcount,
        indices.empty() ? nullptr : indices.data(), static_cast<int>(indices.size()),
        UniRigPredictor::encoderModelPath(), UniRigPredictor::decoderModelPath(),
        UniRigPredictor::embedModelPath(), rnOpts, progress);
    if (!rn.ok || rn.joints.size() < 2) {
        if (outError) *outError = rn.error.isEmpty()
            ? QStringLiteral("UniRig prediction returned no usable skeleton") : rn.error;
        return out;
    }
    out.reserve(rn.joints.size());
    for (const auto& j : rn.joints) {
        Joint pj; pj.name = j.name; pj.parent = j.parent; pj.pos = j.pos; pj.recenter = false;
        out.push_back(std::move(pj));
    }
    return out;
}

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

    // Gather all vertex positions (shared + per-submesh). The vertex order is
    // shared-block first, then each non-shared submesh — appendIndices uses the
    // same offsets so the RigNet index buffer matches.
    std::vector<float> verts;
    const uint32_t sharedBase = 0;
    uint32_t ownBase = 0;
    if (mesh->sharedVertexData) {
        appendPositions(mesh->sharedVertexData, verts);
        ownBase = static_cast<uint32_t>(mesh->sharedVertexData->vertexCount);
    }
    // Per-submesh vertex offsets, captured before we know whether RigNet needs
    // them (cheap). offsets[si] = first vertex index of submesh si's own block.
    std::vector<uint32_t> ownOffset(mesh->getNumSubMeshes(), 0);
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (sub && !sub->useSharedVertices && sub->vertexData) {
            ownOffset[si] = ownBase;
            appendPositions(sub->vertexData, verts);
            ownBase += static_cast<uint32_t>(sub->vertexData->vertexCount);
        }
    }
    const int vcount = static_cast<int>(verts.size() / 3);
    if (vcount == 0) {
        report.error = QStringLiteral("mesh has no readable vertex positions");
        return report;
    }
    report.verticesSampled = vcount;

    const std::vector<Joint> tmpl = templateJoints(opts.tmpl);
    int recentered = 0, markersApplied = 0;
    std::vector<Joint> placed;
    report.algorithmUsed = Algorithm::Pinocchio;   // updated to UniRig on success

    // --- UniRig (#408): ML skeleton prediction, with graceful fallback ------
    // Markers are a Pinocchio/template concept, so UniRig only runs for the
    // plain (marker-less) rig; a marker-driven call always uses the template.
    bool mlUsed = false;
    if (opts.algorithm == Algorithm::UniRig && markers.empty()) {
        QString reason;
        if (!opts.prePredictedJoints.empty()) {
            // GUI worker path: inference already ran off-thread; just adopt the
            // joints (skip the slow ONNX predict here — we're on the main thread
            // building the Ogre skeleton).
            placed = opts.prePredictedJoints;
            for (auto& j : placed) j.recenter = false;
            report.algorithmUsed = Algorithm::UniRig;
            mlUsed = true;
        } else if (!UniRigPredictor::isAvailable()) {
            reason = QStringLiteral("UniRig needs an ONNX-enabled build");
        } else {
            // ensureModelBlocking() returns the encoder path only when BOTH the
            // encoder and decoder models are present (it downloads the missing
            // one on first use); empty == unavailable/offline/not-yet-hosted.
            const QString encModel = UniRigPredictor::ensureModelBlocking();
            if (encModel.isEmpty()) {
                reason = QStringLiteral("UniRig model unavailable (offline or not yet hosted)");
            } else {
                // Build the combined index buffer over the appendPositions order.
                std::vector<uint32_t> indices;
                for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si)
                    appendIndices(mesh->getSubMesh(si), mesh.get(),
                                  sharedBase, ownOffset[si], indices);
                UniRigPredictor::Options rnOpts;
                rnOpts.upAxis = opts.upAxis;
                const auto rn = UniRigPredictor::predict(
                    verts.data(), vcount,
                    indices.empty() ? nullptr : indices.data(),
                    static_cast<int>(indices.size()),
                    UniRigPredictor::encoderModelPath(),
                    UniRigPredictor::decoderModelPath(),
                    UniRigPredictor::embedModelPath(), rnOpts);
                if (rn.ok && rn.joints.size() >= 2) {
                    placed.reserve(rn.joints.size());
                    for (const auto& j : rn.joints) {
                        Joint pj; pj.name = j.name; pj.parent = j.parent;
                        pj.pos = j.pos; pj.recenter = false;
                        placed.push_back(std::move(pj));
                    }
                    report.algorithmUsed = Algorithm::UniRig;
                    mlUsed = true;
                } else {
                    reason = rn.error.isEmpty()
                        ? QStringLiteral("UniRig prediction returned no usable skeleton")
                        : rn.error;
                }
            }
        }
        if (!mlUsed)
            report.fallbackReason =
                QStringLiteral("%1 — used the native template rig instead.").arg(reason);
    }

    // --- Pinocchio / template fit (default, and the UniRig fallback) --------
    if (!mlUsed) {
        placed = markers.empty()
            ? fitTemplate(tmpl, verts.data(), vcount, opts, &recentered)
            : fitTemplateWithMarkers(tmpl, verts.data(), vcount, markers, opts,
                                     &recentered, &markersApplied);
    }
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

    // Strip the BLEND_INDICES/BLEND_WEIGHTS vertex elements that the skin step
    // (`_compileBoneAssignments`) added. CRUCIAL for undo when the rig was
    // committed with skinning: clearing the bone-assignment LIST is not enough
    // — Ogre's `_compileBoneAssignments` only *removes* the blend elements
    // inside `compileBoneAssignments`, which it skips entirely when the list is
    // empty (maxBones == 0). So a plain clear leaves the vertex declaration
    // advertising blend elements while the entity has no skeleton, and the next
    // `_initialise`/render dereferences a null SkeletonInstance → crash. We
    // mirror Ogre's own removal block (unset the buffer, drop both elements).
    auto stripBlend = [](Ogre::VertexData* vd) {
        if (!vd) return;
        Ogre::VertexDeclaration* decl = vd->vertexDeclaration;
        Ogre::VertexBufferBinding* bind = vd->vertexBufferBinding;
        const Ogre::VertexElement* e =
            decl->findElementBySemantic(Ogre::VES_BLEND_INDICES);
        if (!e) return;
        bind->unsetBinding(e->getSource());
        decl->removeElement(Ogre::VES_BLEND_INDICES);
        decl->removeElement(Ogre::VES_BLEND_WEIGHTS);
    };

    // Drop every bone assignment (shared + per-submesh) so the mesh carries no
    // stale weights once it's static again, then strip the blend elements.
    mesh->clearBoneAssignments();
    stripBlend(mesh->sharedVertexData);
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        if (Ogre::SubMesh* sub = mesh->getSubMesh(si)) {
            sub->clearBoneAssignments();
            if (!sub->useSharedVertices)
                stripBlend(sub->vertexData);
        }
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

QString AutoRig::algorithmToString(Algorithm a)
{
    switch (a) {
        case Algorithm::Pinocchio: return QStringLiteral("pinocchio");
        case Algorithm::UniRig:    return QStringLiteral("unirig");
    }
    return QStringLiteral("pinocchio");
}

AutoRig::Algorithm AutoRig::algorithmFromString(const QString& s)
{
    const QString l = s.trimmed().toLower();
    if (l == "unirig" || l == "rignet")  // "rignet" kept as a deprecated alias
        return Algorithm::UniRig;
    // Everything else ("pinocchio" / "native" / "template" / unknown) maps to
    // the offline-reliable native backend.
    return Algorithm::Pinocchio;
}

QJsonObject AutoRig::reportToJson(const Report& r)
{
    QJsonObject o;
    o["applied"]          = r.applied;
    o["meshName"]         = r.meshName;
    o["skeletonName"]     = r.skeletonName;
    o["template"]         = r.templateName;
    o["algorithm"]        = algorithmToString(r.algorithmUsed);
    o["boneCount"]        = r.boneCount;
    o["verticesSampled"]  = r.verticesSampled;
    o["jointsRecentered"] = r.jointsRecentered;
    if (!r.fallbackReason.isEmpty()) o["fallbackReason"] = r.fallbackReason;
    if (!r.error.isEmpty()) o["error"] = r.error;
    return o;
}

QString AutoRig::reportToText(const Report& r)
{
    if (!r.applied)
        return QStringLiteral("Auto-rig failed: %1\n")
            .arg(r.error.isEmpty() ? QStringLiteral("unknown error") : r.error);
    QString s = QStringLiteral(
        "Auto-rigged %1 with the '%2' template (%3 backend).\n"
        "  bones: %4\n  vertices sampled: %5\n  joints recentered: %6\n")
        .arg(r.meshName, r.templateName, algorithmToString(r.algorithmUsed))
        .arg(r.boneCount).arg(r.verticesSampled).arg(r.jointsRecentered);
    if (!r.fallbackReason.isEmpty())
        s += QStringLiteral("  note: %1\n").arg(r.fallbackReason);
    return s;
}
