#include "FaceRigLandmarks.h"

#include "ArkitTemplate.h"
#include "FaceLandmarkDetector.h"

#include "../Manager.h"
#include "../MeshDepthRenderer.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMatrix4.h>
#include <OgreMeshManager.h>
#include <OgreNode.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>
#include <OgreVector3.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace FaceRig {

namespace {

constexpr int kRenderSize = 512;   // render + detect resolution

// Möller–Trumbore ray/triangle intersection. Returns t (ray param) > 0 on hit.
bool rayTri(const Ogre::Vector3& o, const Ogre::Vector3& d,
            const Ogre::Vector3& a, const Ogre::Vector3& b,
            const Ogre::Vector3& c, float& tOut)
{
    const Ogre::Vector3 e1 = b - a, e2 = c - a;
    const Ogre::Vector3 p = d.crossProduct(e2);
    const float det = e1.dotProduct(p);
    if (std::fabs(det) < 1e-9f) return false;
    const float inv = 1.0f / det;
    const Ogre::Vector3 tv = o - a;
    const float u = tv.dotProduct(p) * inv;
    if (u < -1e-4f || u > 1.0001f) return false;
    const Ogre::Vector3 q = tv.crossProduct(e1);
    const float v = d.dotProduct(q) * inv;
    if (v < -1e-4f || u + v > 1.0001f) return false;
    const float t = e2.dotProduct(q) * inv;
    if (t <= 1e-5f) return false;
    tOut = t;
    return true;
}

}  // namespace

MeshLandmarks detectMeshLandmarks(Ogre::Entity* entity,
                                  const std::vector<float>& localV,
                                  const std::vector<int>& localF)
{
    MeshLandmarks out;
    if (!entity || localV.size() < 9 || localF.size() < 3) return out;

    FaceLandmarkDetector det;
    if (!det.load()) return out;   // ONNX off / model missing → caller falls back

    // World transform for building the head focus box + later ray/tri tests.
    Ogre::Node* node = entity->getParentNode();
    const Ogre::Matrix4 world = node ? node->_getFullTransform()
                                     : Ogre::Matrix4::IDENTITY;

    // Head focus box (WORLD space) from the local head verts, so the render
    // frames tightly on the FACE — a full-body character's face would be a few
    // pixels if we framed the whole entity, and MediaPipe wouldn't detect it.
    Ogre::AxisAlignedBox focus;
    { const int fnv = int(localV.size() / 3);
      for (int i = 0; i < fnv; ++i)
          focus.merge(world * Ogre::Vector3(localV[size_t(i)*3],
                                            localV[size_t(i)*3+1],
                                            localV[size_t(i)*3+2])); }

    // 1) render the head front-on (materials intact, lit) for the detector.
    QString err;
    const MeshDepthRenderer::RenderResult rr =
        MeshDepthRenderer::renderShadedView(
            entity, kRenderSize, MeshDepthRenderer::front(), &err,
            focus.isNull() ? nullptr : &focus);
    if (rr.depth.isNull()) return out;
#ifndef NDEBUG
    if (const char* dp = std::getenv("QTMESH_FACERIG_DUMP_RENDER"))
        rr.depth.save(QString::fromUtf8(dp));
#endif

    // 2) detect landmarks (image-pixel space).
    const LandmarkResult lr = det.detect(rr.depth);
    if (!lr.ok || lr.points.empty()) return out;
    out.confidence = lr.confidence;
#ifndef NDEBUG
    if (const char* dp = std::getenv("QTMESH_FACERIG_DUMP_LANDMARKS")) {
        QImage vis = rr.depth.convertToFormat(QImage::Format_RGB888);
        for (const auto& p : lr.points) {
            const int px = int(p[0]), py = int(p[1]);
            for (int dy = -1; dy <= 1; ++dy)
              for (int dx = -1; dx <= 1; ++dx) {
                const int x = px+dx, y = py+dy;
                if (x >= 0 && y >= 0 && x < vis.width() && y < vis.height())
                    vis.setPixel(x, y, qRgb(0, 255, 0));
              }
        }
        vis.save(QString::fromUtf8(dp));
    }
#endif

    // 3) build world-space head triangles (local verts × node world transform).
    //    The render framed the WORLD bounding box, so rays are in world space;
    //    we intersect world triangles and transform the hit back to LOCAL (the
    //    frame the fit uses). `world` was computed above for the focus box.
    const Ogre::Matrix4 worldInv = world.inverse();
    const int nv = int(localV.size() / 3);
    std::vector<Ogre::Vector3> wv(size_t(nv), Ogre::Vector3::ZERO);
    for (int i = 0; i < nv; ++i)
        wv[size_t(i)] = world * Ogre::Vector3(localV[size_t(i)*3],
                                              localV[size_t(i)*3+1],
                                              localV[size_t(i)*3+2]);

    // Inverse of (proj * view) to unproject pixels into world rays.
    const Ogre::Matrix4 vp = rr.projMatrix * rr.viewMatrix;
    const Ogre::Matrix4 vpInv = vp.inverse();
    const float W = float(rr.depth.width()), H = float(rr.depth.height());

    const int nl = int(lr.points.size());
    out.points.assign(size_t(nl), {0, 0, 0});
    out.valid.assign(size_t(nl), 0);

    for (int i = 0; i < nl; ++i) {
        // pixel → NDC ([-1,1], y up). Landmarks are in image pixels (y down).
        const float ndcX = (lr.points[size_t(i)][0] / W) * 2.0f - 1.0f;
        const float ndcY = 1.0f - (lr.points[size_t(i)][1] / H) * 2.0f;
        // unproject near (z=-1) and far (z=1) → world ray.
        const Ogre::Vector3 nearW = vpInv * Ogre::Vector3(ndcX, ndcY, -1.0f);
        const Ogre::Vector3 farW  = vpInv * Ogre::Vector3(ndcX, ndcY,  1.0f);
        const Ogre::Vector3 o = nearW;
        Ogre::Vector3 dir = farW - nearW;
        if (dir.isZeroLength()) continue;
        dir.normalise();

        // nearest triangle hit along the ray.
        float bestT = std::numeric_limits<float>::max();
        Ogre::Vector3 hit;
        bool found = false;
        for (size_t f = 0; f + 2 < localF.size(); f += 3) {
            const int ia = localF[f], ib = localF[f+1], ic = localF[f+2];
            if (ia < 0 || ib < 0 || ic < 0 || ia >= nv || ib >= nv || ic >= nv)
                continue;
            float t;
            if (rayTri(o, dir, wv[size_t(ia)], wv[size_t(ib)], wv[size_t(ic)], t)
                && t < bestT) {
                bestT = t; hit = o + dir * t; found = true;
            }
        }
        if (!found) continue;
        const Ogre::Vector3 local = worldInv * hit;   // back to mesh-local
        out.points[size_t(i)] = {local.x, local.y, local.z};
        out.valid[size_t(i)] = 1;
    }

    // ok only if we anchored a useful number of landmarks.
    int good = 0;
    for (char v : out.valid) good += v ? 1 : 0;
    out.ok = good >= 20;   // need a meaningful anchor set
    return out;
}

namespace {

// Build a throwaway Ogre entity for the template neutral so we can render +
// detect its landmarks. Cached by vertex/face count so repeated rigs in one
// session reuse it (the template is fixed). Returns nullptr on failure.
Ogre::Entity* templateEntity(const std::vector<float>& v,
                             const std::vector<int>& f)
{
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    Ogre::SceneManager* sm = mgr->getSceneMgr();
    if (!sm) return nullptr;

    static Ogre::Entity* cached = nullptr;
    static size_t cachedKey = 0;
    const size_t key = v.size() * 1000003u + f.size();
    if (cached && cachedKey == key) return cached;

    const std::string meshName = "QtMeshFaceRigTemplateMesh";
    const std::string entName = "QtMeshFaceRigTemplateEnt";
    auto& mm = Ogre::MeshManager::getSingleton();
    if (sm->hasEntity(entName)) sm->destroyEntity(entName);
    if (mm.resourceExists(meshName)) mm.remove(meshName);

    Ogre::MeshPtr mesh = mm.createManual(
        meshName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* sub = mesh->createSubMesh();
    sub->useSharedVertices = false;
    const int vc = int(v.size() / 3);
    sub->vertexData = new Ogre::VertexData();
    sub->vertexData->vertexCount = size_t(vc);
    auto* decl = sub->vertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), vc, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    vbuf->writeData(0, v.size() * sizeof(float), v.data());
    sub->vertexData->vertexBufferBinding->setBinding(0, vbuf);

    const bool use32 = vc > 65535;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        use32 ? Ogre::HardwareIndexBuffer::IT_32BIT
              : Ogre::HardwareIndexBuffer::IT_16BIT,
        f.size(), Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    if (use32) {
        std::vector<uint32_t> i32(f.begin(), f.end());
        ibuf->writeData(0, i32.size() * sizeof(uint32_t), i32.data());
    } else {
        std::vector<uint16_t> i16(f.size());
        for (size_t i = 0; i < f.size(); ++i) i16[i] = uint16_t(f[i]);
        ibuf->writeData(0, i16.size() * sizeof(uint16_t), i16.data());
    }
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = f.size();

    Ogre::Vector3 mn(1e30f, 1e30f, 1e30f), mx(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < vc; ++i) {
        Ogre::Vector3 p(v[size_t(i)*3], v[size_t(i)*3+1], v[size_t(i)*3+2]);
        mn.makeFloor(p); mx.makeCeil(p);
    }
    mesh->_setBounds(Ogre::AxisAlignedBox(mn, mx));
    mesh->_setBoundingSphereRadius(0.5f * (mx - mn).length());
    mesh->load();

    Ogre::Entity* ent = sm->createEntity(entName, meshName);
    // Parent to a detached node so it's in the scene graph for world transform
    // but hidden (renderShadedView hides all other entities anyway; this one is
    // the render target when we pass it in).
    Ogre::SceneNode* node = sm->getRootSceneNode()->createChildSceneNode();
    node->attachObject(ent);
    node->setVisible(false);   // only shown during its own render pass

    cached = ent;
    cachedKey = key;
    return ent;
}

}  // namespace

namespace {
double constellationResidual(const std::vector<std::array<float,3>>& C,
                             const std::vector<std::array<float,3>>& U);
}  // namespace

std::vector<NricpLandmark> buildLandmarkAnchors(
    Ogre::Entity* userEntity,
    const std::vector<float>& userLocalV,
    const std::vector<int>& userLocalF,
    const ArkitTemplate& tmpl)
{
    std::vector<NricpLandmark> anchors;
    if (!userEntity || !tmpl.valid()) return anchors;
    if (!FaceLandmarkDetector::backendAvailable()) return anchors;
    // Cheap pre-check: no model → skip the renders entirely.
    { FaceLandmarkDetector probe; if (!probe.load()) return anchors; }

    // template side: build a temp entity, detect, map each landmark → nearest
    // template vertex.
    Ogre::Entity* tent = templateEntity(tmpl.neutral(), tmpl.faces());
    if (!tent) return anchors;
    // render pass needs it visible; renderShadedView hides OTHERS, so show it.
    if (auto* tn = tent->getParentSceneNode()) tn->setVisible(true);
    const MeshLandmarks tlm = detectMeshLandmarks(tent, tmpl.neutral(),
                                                  std::vector<int>(tmpl.faces()));
    if (auto* tn = tent->getParentSceneNode()) tn->setVisible(false);
    if (!tlm.ok) return anchors;

    // user side.
    const MeshLandmarks ulm = detectMeshLandmarks(userEntity, userLocalV, userLocalF);
    if (!ulm.ok) return anchors;

    const int nl = int(std::min(tlm.points.size(), ulm.points.size()));
    const int tvc = tmpl.vertexCount();
    const auto& tn = tmpl.neutral();
    for (int i = 0; i < nl; ++i) {
        if (!tlm.valid[size_t(i)] || !ulm.valid[size_t(i)]) continue;
        // nearest template vertex to the template landmark point.
        const auto& tp = tlm.points[size_t(i)];
        int best = -1; float bestD = std::numeric_limits<float>::max();
        for (int vtx = 0; vtx < tvc; ++vtx) {
            const float dx = tn[size_t(vtx)*3] - tp[0];
            const float dy = tn[size_t(vtx)*3+1] - tp[1];
            const float dz = tn[size_t(vtx)*3+2] - tp[2];
            const float d = dx*dx + dy*dy + dz*dz;
            if (d < bestD) { bestD = d; best = vtx; }
        }
        if (best < 0) continue;
        anchors.push_back({best, ulm.points[size_t(i)]});
    }

    // Gate on constellation consistency: MediaPipe returns a scattered garbage
    // blob on cartoon/stylized faces, and anchoring the fit to garbage is worse
    // than fitting unanchored. Compare the template-vs-user landmark layouts
    // under a similarity — a real detection agrees, garbage doesn't.
    {
        std::vector<std::array<float,3>> C, U;
        C.reserve(anchors.size()); U.reserve(anchors.size());
        for (const auto& a : anchors) {
            C.push_back({tn[size_t(a.tmplVertex)*3],
                         tn[size_t(a.tmplVertex)*3+1],
                         tn[size_t(a.tmplVertex)*3+2]});
            U.push_back(a.target);
        }
        const double resid = constellationResidual(C, U);
#ifndef NDEBUG
        if (std::getenv("QTMESH_FACERIG_DEBUG"))
            std::fprintf(stderr, "[facerig] landmark anchors: tmpl.ok=%d "
                         "user.ok=%d anchors=%zu residual=%.3f\n",
                         tlm.ok, ulm.ok, anchors.size(), resid);
#endif
        if (resid >= 0.15) anchors.clear();   // garbage → fit unanchored
    }
    return anchors;
}

// Canonical MediaPipe FaceMesh indices for the few anatomical anchors the fit
// needs. IMPORTANT side semantics: MediaPipe names sides in IMAGE space, which
// is MIRRORED for a camera-facing subject — MP "left" indices (33/133/61/105)
// are the CHARACTER'S RIGHT (measured on the template: they sit at +X =
// character-right). Labels here are in CHARACTER space (what a user placing
// markers on a model naturally means by left/right).
const std::vector<std::pair<QString, int>>& faceMarkerCatalog()
{
    static const std::vector<std::pair<QString, int>> kCatalog = {
        {QStringLiteral("Nose tip"),           1},
        {QStringLiteral("Chin"),               152},
        {QStringLiteral("Right eye outer"),    33},
        {QStringLiteral("Left eye outer"),     263},
        {QStringLiteral("Right eye inner"),    133},
        {QStringLiteral("Left eye inner"),     362},
        {QStringLiteral("Right mouth corner"), 61},
        {QStringLiteral("Left mouth corner"),  291},
        {QStringLiteral("Upper lip"),          13},
        {QStringLiteral("Lower lip"),          14},
        {QStringLiteral("Right brow"),         105},
        {QStringLiteral("Left brow"),          334},
        {QStringLiteral("Forehead"),           10},
    };
    return kCatalog;
}

namespace {
// Side-pair / midline structure of the catalog (MediaPipe indices). Used to
// SYMMETRIZE the template-side anchors (the template is x-symmetric with the
// midline at x=0, but MediaPipe drifts on the untextured template render —
// measured: "nose tip" detected 4 units off the midline) and to auto-correct
// a user who placed markers with the opposite left/right convention.
constexpr int kMidlineIdx[] = {1, 152, 13, 14, 10};
constexpr int kPairIdx[][2] = {{33, 263}, {133, 362}, {61, 291}, {105, 334}};

bool isMidline(int mpIdx)
{
    for (int m : kMidlineIdx) if (m == mpIdx) return true;
    return false;
}
int pairOf(int mpIdx)
{
    for (const auto& p : kPairIdx) {
        if (p[0] == mpIdx) return p[1];
        if (p[1] == mpIdx) return p[0];
    }
    return -1;
}
}  // namespace

namespace {
int nearestTemplateVertex(const std::vector<float>& tn, int tvc,
                          const std::array<float, 3>& p)
{
    int best = -1; float bestD = std::numeric_limits<float>::max();
    for (int vtx = 0; vtx < tvc; ++vtx) {
        const float dx = tn[size_t(vtx)*3] - p[0];
        const float dy = tn[size_t(vtx)*3+1] - p[1];
        const float dz = tn[size_t(vtx)*3+2] - p[2];
        const float d = dx*dx + dy*dy + dz*dz;
        if (d < bestD) { bestD = d; best = vtx; }
    }
    return best;
}

// Does the user point constellation actually LOOK like the template's? Align
// C onto U with a similarity (centroid + RMS scale, no rotation — both faces
// upright/front by contract) and measure the mean residual normalised by U's
// spread. A real face layout agrees (≲0.2); a garbage detection (MediaPipe on
// a cartoon face returns a scattered blob) does not. This is the gate that
// keeps garbage landmarks from silently poisoning the warp/fit.
double constellationResidual(const std::vector<std::array<float,3>>& C,
                             const std::vector<std::array<float,3>>& U)
{
    const int n = int(std::min(C.size(), U.size()));
    if (n < 4) return 1e9;
    std::array<double,3> cC{0,0,0}, cU{0,0,0};
    for (int i = 0; i < n; ++i)
        for (int d = 0; d < 3; ++d) {
            cC[size_t(d)] += C[size_t(i)][size_t(d)] / n;
            cU[size_t(d)] += U[size_t(i)][size_t(d)] / n;
        }
    double sC = 0, sU = 0;
    for (int i = 0; i < n; ++i) {
        double dc = 0, du = 0;
        for (int d = 0; d < 3; ++d) {
            const double a = C[size_t(i)][size_t(d)] - cC[size_t(d)];
            const double b = U[size_t(i)][size_t(d)] - cU[size_t(d)];
            dc += a*a; du += b*b;
        }
        sC += std::sqrt(dc); sU += std::sqrt(du);
    }
    if (sC < 1e-12 || sU < 1e-12) return 1e9;
    const double scale = sU / sC;
    double resid = 0;
    for (int i = 0; i < n; ++i) {
        double d2 = 0;
        for (int d = 0; d < 3; ++d) {
            const double m = (C[size_t(i)][size_t(d)] - cC[size_t(d)]) * scale
                             + cU[size_t(d)];
            const double e = U[size_t(i)][size_t(d)] - m;
            d2 += e*e;
        }
        resid += std::sqrt(d2);
    }
    resid /= n;
    return resid / (sU / n);   // normalise by mean spread of U
}
}  // namespace

std::vector<FaceMarker> seedFaceMarkers(
    Ogre::Entity* userEntity,
    const std::vector<float>& userLocalV,
    const std::vector<int>& userLocalF,
    const ArkitTemplate& tmpl,
    bool* outConfident)
{
    std::vector<FaceMarker> markers;
    if (outConfident) *outConfident = false;
    if (!userEntity || !tmpl.valid()) return markers;

    const auto& cat = faceMarkerCatalog();
    markers.reserve(cat.size());
    for (const auto& [label, idx] : cat) {
        FaceMarker m; m.label = label; m.mediapipeIndex = idx;
        markers.push_back(std::move(m));
    }

    if (!FaceLandmarkDetector::backendAvailable()) return markers;
    { FaceLandmarkDetector probe; if (!probe.load()) return markers; }

    // Template detection resolves each marker's TEMPLATE vertex (reliable — the
    // ICT template is a real human face).
    Ogre::Entity* tent = templateEntity(tmpl.neutral(), tmpl.faces());
    if (!tent) return markers;
    if (auto* tnode = tent->getParentSceneNode()) tnode->setVisible(true);
    const MeshLandmarks tlm = detectMeshLandmarks(tent, tmpl.neutral(),
                                                  std::vector<int>(tmpl.faces()));
    if (auto* tnode = tent->getParentSceneNode()) tnode->setVisible(false);

    const int tvc = tmpl.vertexCount();
    const auto& tn = tmpl.neutral();
    if (tlm.ok) {
        // SYMMETRIZE the detected template landmarks before resolving vertices:
        // the ICT template is x-symmetric (midline at x=0), but MediaPipe
        // drifts on the untextured template render (measured: nose tip 4 units
        // off-midline), which mis-anchors EVERYTHING downstream. Midline
        // features snap to x=0; side pairs get mirrored positions with the
        // pair-mean height/depth and mean |x|. Detection still supplies the
        // vertical/depth placement it gets roughly right.
        auto detected = [&](int mpIdx, std::array<float,3>& out) -> bool {
            if (mpIdx < 0 || mpIdx >= int(tlm.points.size())
                || !tlm.valid[size_t(mpIdx)]) return false;
            out = tlm.points[size_t(mpIdx)];
            return true;
        };
        for (auto& m : markers) {
            const int i = m.mediapipeIndex;
            std::array<float,3> p;
            if (!detected(i, p)) continue;
            if (isMidline(i)) {
                p[0] = 0.0f;
            } else if (const int j = pairOf(i); j >= 0) {
                std::array<float,3> q;
                if (detected(j, q)) {
                    const float xm = 0.5f * (std::fabs(p[0]) + std::fabs(q[0]));
                    const float ym = 0.5f * (p[1] + q[1]);
                    const float zm = 0.5f * (p[2] + q[2]);
                    // keep this marker on the side detection put it (ties →
                    // MP-left-named index goes to +X = character-right,
                    // the measured convention on this template).
                    float sign = p[0] > q[0] ? 1.0f : (p[0] < q[0] ? -1.0f
                        : ((i == 33 || i == 133 || i == 61 || i == 105) ? 1.0f
                                                                        : -1.0f));
                    p = { sign * xm, ym, zm };
                } // single-sided detection: use as-is
            }
            m.tmplVertex = nearestTemplateVertex(tn, tvc, p);
        }
    }

    // User detection seeds the editable positions — but ONLY when the detected
    // constellation actually looks like a face layout. MediaPipe returns a
    // scattered garbage blob on cartoon/stylized faces, and garbage seeds
    // silently poison the warp/fit (measured: jawOpen deltas 50x too small).
    const MeshLandmarks ulm = detectMeshLandmarks(userEntity, userLocalV, userLocalF);

    int seeded = 0;
    std::vector<std::array<float,3>> detC, detU;   // template/user pairs
    if (ulm.ok) {
        for (auto& m : markers) {
            const int i = m.mediapipeIndex;
            if (i >= 0 && i < int(ulm.points.size()) && ulm.valid[size_t(i)]
                && m.tmplVertex >= 0) {
                ++seeded;
                detC.push_back({tn[size_t(m.tmplVertex)*3],
                                tn[size_t(m.tmplVertex)*3+1],
                                tn[size_t(m.tmplVertex)*3+2]});
                detU.push_back(ulm.points[size_t(i)]);
            }
        }
    }
    // STRICT gate (0.12): the GUI render differs from headless (skybox,
    // lighting), so MediaPipe's garbage varies run-to-run and looser gates let
    // some of it through (field-reproduced: a "trusted" garbage constellation
    // crushed every shape to 0.06% amplitude). Detection must look UNAMBIGUOUSLY
    // like a face layout to be trusted; otherwise the proportional defaults are
    // measurably good and the user refines from there.
    const double resid = constellationResidual(detC, detU);
    const bool confident = ulm.ok && seeded >= int(markers.size()) * 3 / 4
                           && resid < 0.12;
#ifndef NDEBUG
    if (std::getenv("QTMESH_FACERIG_DEBUG"))
        std::fprintf(stderr, "[facerig] seed: %d/%zu detected, constellation "
                     "residual %.3f -> %s\n", seeded, markers.size(), resid,
                     confident ? "trusted" : "using proportional defaults");
#endif

    if (confident) {
        for (auto& m : markers) {
            const int i = m.mediapipeIndex;
            if (i >= 0 && i < int(ulm.points.size()) && ulm.valid[size_t(i)]) {
                m.userPos = ulm.points[size_t(i)];
                m.placed = true;
            }
        }
    } else {
        // Garbage / weak detection: seed EVERY marker at the head-box-projected
        // template position instead. Those proportional defaults measurably
        // produce a good rig on their own (the cartoon-face path), and the user
        // refines from there. placed=true so they act as anchors even if the
        // user rigs without touching them.
        std::array<float,3> lo{1e30f,1e30f,1e30f}, hi{-1e30f,-1e30f,-1e30f};
        const int unv = int(userLocalV.size()/3);
        for (int i = 0; i < unv; ++i)
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], userLocalV[size_t(i)*3+a]);
                hi[a] = std::max(hi[a], userLocalV[size_t(i)*3+a]);
            }
        std::array<float,3> tlo{1e30f,1e30f,1e30f}, thi{-1e30f,-1e30f,-1e30f};
        for (int i = 0; i < tvc; ++i)
            for (int a = 0; a < 3; ++a) {
                tlo[a] = std::min(tlo[a], tn[size_t(i)*3+a]);
                thi[a] = std::max(thi[a], tn[size_t(i)*3+a]);
            }
        for (auto& m : markers) {
            if (m.tmplVertex < 0) continue;
            for (int a = 0; a < 3; ++a) {
                const float tv = tn[size_t(m.tmplVertex)*3+a];
                const float f = (thi[a]-tlo[a]) > 1e-6f
                    ? (tv - tlo[a]) / (thi[a]-tlo[a]) : 0.5f;
                m.userPos[size_t(a)] = lo[a] + f * (hi[a]-lo[a]);
            }
            m.placed = true;
        }
    }

    if (outConfident) *outConfident = confident;
    return markers;
}

std::vector<NricpLandmark> anchorsFromMarkers(const std::vector<FaceMarker>& markers,
                                              const ArkitTemplate& tmpl)
{
    // Two candidate pairings: as placed, and with the left/right PAIR targets
    // swapped — a user may reasonably use either the character's or the
    // screen's left/right. Score both against the template constellation and
    // keep the one that agrees; a mirrored anchor set would ask the warp to
    // fold the template through itself and crush every shape.
    auto build = [&](bool swapped) {
        std::vector<NricpLandmark> anchors;
        for (const auto& m : markers) {
            if (!m.placed || m.tmplVertex < 0) continue;
            std::array<float,3> target = m.userPos;
            if (swapped) {
                const int j = pairOf(m.mediapipeIndex);
                if (j >= 0) {
                    // use the PAIRED marker's position instead
                    for (const auto& o : markers)
                        if (o.mediapipeIndex == j && o.placed) {
                            target = o.userPos;
                            break;
                        }
                }
            }
            anchors.push_back({m.tmplVertex, target});
        }
        return anchors;
    };
    auto residualOf = [&](const std::vector<NricpLandmark>& anchors) {
        std::vector<std::array<float,3>> C, U;
        const auto& tn = tmpl.neutral();
        const int tvc = tmpl.vertexCount();
        for (const auto& a : anchors) {
            if (a.tmplVertex < 0 || a.tmplVertex >= tvc) continue;
            C.push_back({tn[size_t(a.tmplVertex)*3],
                         tn[size_t(a.tmplVertex)*3+1],
                         tn[size_t(a.tmplVertex)*3+2]});
            U.push_back(a.target);
        }
        return constellationResidual(C, U);
    };

    std::vector<NricpLandmark> normal = build(false);
    if (!tmpl.valid()) return normal;
    std::vector<NricpLandmark> swapped = build(true);
    const double rn = residualOf(normal);
    const double rs = residualOf(swapped);
    if (rs < rn) {
        std::fprintf(stderr, "[facerig] markers look MIRRORED (residual %.3f "
                     "vs %.3f) — auto-swapping left/right pairs\n", rn, rs);
        return swapped;
    }
    return normal;
}

}  // namespace FaceRig
