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
#ifndef NDEBUG
    if (std::getenv("QTMESH_FACERIG_DEBUG"))
        std::fprintf(stderr, "[facerig] landmark anchors: tmpl.ok=%d user.ok=%d "
                     "anchors=%zu\n", tlm.ok, ulm.ok, anchors.size());
#endif
    return anchors;
}

}  // namespace FaceRig
