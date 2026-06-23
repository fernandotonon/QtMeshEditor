#include "AutoRig.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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

    // Fit the template.
    int recentered = 0;
    const std::vector<Joint> tmpl = templateJoints(opts.tmpl);
    const std::vector<Joint> placed =
        fitTemplate(tmpl, verts.data(), vcount, opts, &recentered);
    report.jointsRecentered = recentered;

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
