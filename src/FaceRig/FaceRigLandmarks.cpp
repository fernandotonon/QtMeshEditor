#include "FaceRigLandmarks.h"

#include "ArkitTemplate.h"
#include "FaceLandmarkDetector.h"
#include "FaceRigAttach.h"

#include "../Manager.h"
#include "../MeshDepthRenderer.h"
#include "../TransformOperator.h"
#include "../OgreWidget.h"

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

    // 1+2) render the head + detect landmarks — MULTI-VIEW. Nothing guarantees
    // the mesh faces the renderer's "front" (glTF assets commonly face +Z
    // while MeshDepthRenderer::front() places the camera on -Z; the LH-flip
    // asymmetry between import and glTF export also flips facing on
    // round-trips). Render the four horizontal views and keep the one
    // MediaPipe is most confident about; a true face scores high (~0.9+), the
    // back of a head scores low or fails outright. Early-out on a confident
    // hit so the common facing stays one render.
    const MeshDepthRenderer::View views[] = {
        MeshDepthRenderer::front(), MeshDepthRenderer::back(),
        MeshDepthRenderer::left(),  MeshDepthRenderer::right(),
    };
    MeshDepthRenderer::RenderResult rr;
    LandmarkResult lr;
    float bestLogit = -1e9f;
    // FACING may only be claimed by SHADED-mode detections: the fog depth
    // statue has no texture and MediaPipe false-positives on smooth domes
    // (measured: Rumba's occiput depth render scored logit +8 while its true
    // stylized face scored -3.5 — the facing flipped to the back of the
    // head). Depth-mode detections still feed LANDMARKS (the constellation
    // gate protects those), just never the facing decision.
    float bestShadedLogit = -1e9f;
    Ogre::Vector3 bestShadedCamDir = Ogre::Vector3::ZERO;
    // Geometric facing fallback: mean |Laplacian| of the fog depth render
    // over subject pixels. The face side of a head (nose, brows, lips, chin)
    // carries far more depth detail than the smooth occiput — used to decide
    // facing when NO view shows positive face evidence (stylized faces
    // MediaPipe can't read return only noise logits).
    double bestDetail = -1.0;
    Ogre::Vector3 bestDetailCamDir = Ogre::Vector3::ZERO;
    auto depthDetail = [](const QImage& img) -> double {
        const QImage g = img.convertToFormat(QImage::Format_Grayscale8);
        double acc = 0; long n = 0;
        for (int y = 1; y + 1 < g.height(); ++y) {
            const uchar* lm = g.constScanLine(y - 1);
            const uchar* lc = g.constScanLine(y);
            const uchar* lp = g.constScanLine(y + 1);
            for (int x = 1; x + 1 < g.width(); ++x) {
                if (lc[x] <= 12) continue;   // background
                acc += std::fabs(4.0 * lc[x] - lc[x-1] - lc[x+1] - lm[x] - lp[x]);
                ++n;
            }
        }
        return n > 0 ? acc / double(n) : -1.0;
    };
    // Two render styles per view: the shaded render (materials intact —
    // carries texture contrast MediaPipe likes) and the fog depth-map render
    // (pure geometry statue — immune to broken normals / inconsistent winding
    // / missing textures, which turn the shaded render into unusable noise).
    //
    // EVERY view in a mode is evaluated — no first-hit early-out. The winner
    // is the highest RAW presence logit: a true face scores ~+20 while a
    // false positive (the smooth back of a head) scores far lower, but both
    // saturate the sigmoid, so an early-out on `confidence` locked onto the
    // back of backwards-facing imports (glb round-trips flip facing). Ranking
    // all four views by logit IS the orientation detection.
    constexpr float kStrongFaceLogit = 6.0f;   // sigmoid ≈ 0.998
    for (int depthMode = 0;
         depthMode <= 1 && bestLogit < kStrongFaceLogit; ++depthMode) {
        for (const auto& view : views) {
            QString err;
            MeshDepthRenderer::RenderResult vrr = depthMode
                ? MeshDepthRenderer::renderDepthMapView(
                      entity, kRenderSize, view, &err,
                      focus.isNull() ? nullptr : &focus)
                : MeshDepthRenderer::renderShadedView(
                      entity, kRenderSize, view, &err,
                      focus.isNull() ? nullptr : &focus);
            if (vrr.depth.isNull()) continue;
            if (depthMode) {
                const double det = depthDetail(vrr.depth);
                if (std::getenv("QTMESH_FACERIG_DEBUG"))
                    std::fprintf(stderr, "[facerig] detail view=%s score=%.2f\n",
                                 view.name, det);
                if (det > bestDetail) {
                    bestDetail = det;
                    bestDetailCamDir = vrr.camDirection;
                }
            }
            // Flatness sanity: a blown-out / silhouette render (near-zero
            // intensity variance inside the subject) carries no facial
            // features — MediaPipe false-positives on such blobs with high
            // presence, and the garbage landmarks CORRELATE between the
            // template and user renders, slipping through the constellation
            // gate. A genuinely shaded face has stddev well above this.
            {
                const QImage g = vrr.depth.convertToFormat(QImage::Format_Grayscale8);
                double sum = 0, sum2 = 0; long n = 0;
                for (int y = 0; y < g.height(); ++y) {
                    const uchar* ln = g.constScanLine(y);
                    for (int x = 0; x < g.width(); ++x) {
                        if (ln[x] > 12) { sum += ln[x]; sum2 += double(ln[x]) * ln[x]; ++n; }
                    }
                }
                const double var = n > 0 ? (sum2 / n - (sum / n) * (sum / n)) : 0.0;
                if (n < 64 || var < 36.0) {   // stddev < 6 → featureless
                    if (std::getenv("QTMESH_FACERIG_DEBUG"))
                        std::fprintf(stderr, "[facerig] detect view=%s mode=%s "
                                     "SKIPPED (flat render, var=%.1f)\n",
                                     view.name, depthMode ? "depth" : "shaded", var);
                    continue;
                }
            }
            LandmarkResult vlr = det.detect(vrr.depth);
            if (std::getenv("QTMESH_FACERIG_DEBUG"))
                std::fprintf(stderr,
                             "[facerig] detect view=%s mode=%s ok=%d conf=%.2f "
                             "logit=%.1f\n",
                             view.name, depthMode ? "depth" : "shaded",
                             vlr.ok, vlr.confidence, vlr.presenceLogit);
            if (const char* dp = std::getenv("QTMESH_FACERIG_DUMP_RENDER"))
                vrr.depth.save(QString::fromUtf8(dp) + "."
                               + QString::fromStdString(entity->getName()) + "."
                               + (depthMode ? "depth." : "shaded.")
                               + view.name + ".png");
            if (!vlr.ok || vlr.points.empty()) continue;
            if (!depthMode && vlr.presenceLogit > bestShadedLogit) {
                bestShadedLogit = vlr.presenceLogit;
                bestShadedCamDir = vrr.camDirection;
            }
            if (vlr.presenceLogit > bestLogit) {
                bestLogit = vlr.presenceLogit;
                rr = std::move(vrr);
                lr = std::move(vlr);
            }
        }
    }
    // Facing signal: the face points TOWARD the winning view's camera
    // (= against its look direction). When NO view produced positive face
    // evidence (all logits negative — MediaPipe can't read stylized faces),
    // the logit "winner" is noise; fall back to the depth-DETAIL winner
    // instead (the face side out-details the smooth back of the head).
    // Exposed in MESH-LOCAL space even when the landmarks themselves are too
    // weak to use — the proportional-default marker placement needs only the
    // facing.
    {
        // Facing ladder: (1) a strong SHADED-mode detection; (2) the active
        // viewport camera; (3) feet direction; (4) depth-detail winner; (5)
        // the overall logit winner as a last resort.
        Ogre::Vector3 camDir = Ogre::Vector3::ZERO;
        const bool strongShaded = bestShadedLogit >= kStrongFaceLogit
                                  && !bestShadedCamDir.isZeroLength();
        if (strongShaded) {
            camDir = bestShadedCamDir;
            if (std::getenv("QTMESH_FACERIG_DEBUG"))
                std::fprintf(stderr, "[facerig] facing from SHADED detection "
                             "(logit=%.1f)\n", bestShadedLogit);
        }
        // The ACTIVE VIEWPORT camera is the strongest user-intent hint: the
        // user orbits to LOOK AT the face before rigging, so the face points
        // toward that camera. Trust it over every geometric fallback whenever
        // detection itself isn't conclusive.
        bool vpResolved = false;
        if (!strongShaded) {
            if (auto* to = TransformOperator::getSingletonPtr()) {
                if (auto* w = to->getActiveWidget()) {
                    if (w->getViewport() && w->getViewport()->getCamera()) {
                        const Ogre::Vector3 d =
                            w->getViewport()->getCamera()->getDerivedDirection();
                        if (!d.isZeroLength()) {
                            camDir = d;   // camera looks toward the face
                            vpResolved = true;
                            if (std::getenv("QTMESH_FACERIG_DEBUG"))
                                std::fprintf(stderr,
                                    "[facerig] facing from VIEWPORT camera "
                                    "(bestLogit=%.1f)\n", bestLogit);
                        }
                    }
                }
            }
        }
        if (!strongShaded && !vpResolved) {
            // No positive face evidence anywhere (stylized / covered faces) —
            // the logit "winner" is noise. For a FULL-BODY character the feet
            // are the strongest facing cue: toes extend forward of the ankle.
            // Compare the horizontal centroid of the feet slab (lowest 8% of
            // the body) against the ankle slab above it.
            bool feetResolved = false;
            const FaceRigGeometry full = extractGeometry(entity);
            if (full.valid()) {
                const int n = int(full.userV.size() / 3);
                float bLoY = 1e30f, bHiY = -1e30f;
                for (int i = 0; i < n; ++i) {
                    bLoY = std::min(bLoY, full.userV[size_t(i)*3+1]);
                    bHiY = std::max(bHiY, full.userV[size_t(i)*3+1]);
                }
                // head height from the head verts we render (localV)
                float hLoY = 1e30f, hHiY = -1e30f;
                for (int i = 0; i < int(localV.size()/3); ++i) {
                    hLoY = std::min(hLoY, localV[size_t(i)*3+1]);
                    hHiY = std::max(hHiY, localV[size_t(i)*3+1]);
                }
                const float bodyH = bHiY - bLoY, headH = hHiY - hLoY;
                if (std::getenv("QTMESH_FACERIG_DEBUG"))
                    std::fprintf(stderr, "[facerig] feet gate: bodyH=%.2f "
                                 "headH=%.2f ratio=%.2f\n",
                                 bodyH, headH, headH > 0 ? bodyH/headH : 0.f);
                if (bodyH > 2.5f * headH && bodyH > 1e-6f) {
                    const float feetTop  = bLoY + 0.08f * bodyH;
                    const float ankleTop = bLoY + 0.16f * bodyH;
                    double fx = 0, fz = 0, ax = 0, az = 0;
                    long nf = 0, na = 0;
                    for (int i = 0; i < n; ++i) {
                        const float y = full.userV[size_t(i)*3+1];
                        if (y < feetTop) {
                            fx += full.userV[size_t(i)*3]; fz += full.userV[size_t(i)*3+2]; ++nf;
                        } else if (y < ankleTop) {
                            ax += full.userV[size_t(i)*3]; az += full.userV[size_t(i)*3+2]; ++na;
                        }
                    }
                    if (std::getenv("QTMESH_FACERIG_DEBUG"))
                        std::fprintf(stderr, "[facerig] feet slabs: nf=%ld "
                                     "na=%ld\n", nf, na);
                    if (nf > 8 && na > 8) {
                        const double dx = fx/nf - ax/na, dz = fz/nf - az/na;
                        const double len = std::sqrt(dx*dx + dz*dz);
                        if (std::getenv("QTMESH_FACERIG_DEBUG"))
                            std::fprintf(stderr, "[facerig] feet dir: "
                                         "d=(%.3f,%.3f) len=%.3f min=%.3f\n",
                                         dx, dz, len, 0.005 * bodyH);
                        // 0.5% of body height: the toe-forward offset is small
                        // on dance-pose rigs (Rumba: 1.9cm on a 2m body) but
                        // its DIRECTION is reliable; only reject a truly
                        // degenerate (near-zero) offset.
                        if (len > 0.005 * bodyH) {
                            // LOCAL-space facing; convert to a WORLD camDir
                            // (the block below converts back) by mapping
                            // through the entity transform.
                            const Ogre::Vector3 o = world * Ogre::Vector3::ZERO;
                            Ogre::Vector3 faceW =
                                (world * Ogre::Vector3(float(dx), 0, float(dz))) - o;
                            if (!faceW.isZeroLength()) {
                                camDir = -faceW;   // camera looks against facing
                                feetResolved = true;
                                if (std::getenv("QTMESH_FACERIG_DEBUG"))
                                    std::fprintf(stderr,
                                        "[facerig] facing from FEET dir "
                                        "local=(%.2f,0,%.2f)\n", dx/len, dz/len);
                            }
                        }
                    }
                }
            }
            if (!feetResolved && bestDetail >= 0.0
                && !bestDetailCamDir.isZeroLength()) {
                camDir = bestDetailCamDir;
                if (std::getenv("QTMESH_FACERIG_DEBUG"))
                    std::fprintf(stderr, "[facerig] facing from depth detail "
                                 "(bestLogit=%.1f, detail=%.2f)\n",
                                 bestLogit, bestDetail);
            }
        }
        if (!camDir.isZeroLength()) {
            const Ogre::Matrix4 wInv = world.inverse();
            const Ogre::Vector3 faceW = -camDir;
            const Ogre::Vector3 o = wInv * Ogre::Vector3::ZERO;
            Ogre::Vector3 faceL = (wInv * faceW) - o;
            if (!faceL.isZeroLength()) {
                faceL.normalise();
                out.faceDirLocal = {faceL.x, faceL.y, faceL.z};
                out.faceDirValid = true;
            }
        }
    }
    if (rr.depth.isNull() || !lr.ok || lr.points.empty()) return out;
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
    decl->addElement(0, sizeof(float) * 3, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);

    // Smooth vertex normals (area-weighted face-normal accumulation). Without
    // a NORMAL attribute the render pipeline samples undefined per-vertex data
    // and the shaded render degrades to per-triangle noise MediaPipe can't
    // detect a face in.
    std::vector<float> normals(v.size(), 0.0f);
    for (size_t i = 0; i + 2 < f.size(); i += 3) {
        const int a = f[i], b = f[i+1], c = f[i+2];
        if (a < 0 || b < 0 || c < 0 || a >= vc || b >= vc || c >= vc) continue;
        const Ogre::Vector3 pa(v[size_t(a)*3], v[size_t(a)*3+1], v[size_t(a)*3+2]);
        const Ogre::Vector3 pb(v[size_t(b)*3], v[size_t(b)*3+1], v[size_t(b)*3+2]);
        const Ogre::Vector3 pc(v[size_t(c)*3], v[size_t(c)*3+1], v[size_t(c)*3+2]);
        const Ogre::Vector3 n = (pb - pa).crossProduct(pc - pa);  // area-weighted
        for (int k : {a, b, c}) {
            normals[size_t(k)*3+0] += n.x;
            normals[size_t(k)*3+1] += n.y;
            normals[size_t(k)*3+2] += n.z;
        }
    }
    for (int i = 0; i < vc; ++i) {
        Ogre::Vector3 n(normals[size_t(i)*3], normals[size_t(i)*3+1],
                        normals[size_t(i)*3+2]);
        if (n.isZeroLength()) n = Ogre::Vector3::UNIT_Z;
        n.normalise();
        normals[size_t(i)*3+0] = n.x;
        normals[size_t(i)*3+1] = n.y;
        normals[size_t(i)*3+2] = n.z;
    }

    std::vector<float> interleaved(size_t(vc) * 6);
    for (int i = 0; i < vc; ++i) {
        interleaved[size_t(i)*6+0] = v[size_t(i)*3+0];
        interleaved[size_t(i)*6+1] = v[size_t(i)*3+1];
        interleaved[size_t(i)*6+2] = v[size_t(i)*3+2];
        interleaved[size_t(i)*6+3] = normals[size_t(i)*3+0];
        interleaved[size_t(i)*6+4] = normals[size_t(i)*3+1];
        interleaved[size_t(i)*6+5] = normals[size_t(i)*3+2];
    }
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), vc, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    vbuf->writeData(0, interleaved.size() * sizeof(float), interleaved.data());
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
        if (std::getenv("QTMESH_FACERIG_DEBUG"))
            std::fprintf(stderr, "[facerig] landmark anchors: tmpl.ok=%d "
                         "user.ok=%d anchors=%zu residual=%.3f\n",
                         tlm.ok, ulm.ok, anchors.size(), resid);
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
int canonicalTemplateVertex(int mpIndex)
{
    // ICT-FaceKit topology (26,719 verts). Derived offline from the packed
    // template's own blendshape deltas + midline geometry; side assignment
    // matches the catalog's measured detector convention (anchorsFromMarkers'
    // mirror check absorbs a global L/R flip regardless).
    switch (mpIndex) {
        case 1:   return 4841;   // nose tip (front-most midline)
        case 152: return 961;    // chin / gnathion (lowest front midline)
        case 13:  return 5829;   // upper lip (mouthUpperUp peak, midline)
        case 14:  return 5945;   // lower lip (mouthLowerDown peak, midline)
        case 10:  return 2138;   // forehead (midline above brows)
        case 33:  return 2798;   // eye outer corner (+X lid extreme)
        case 133: return 3585;   // eye inner corner (+X)
        case 61:  return 6156;   // mouth corner (+X, mouthSmile peak)
        case 105: return 2590;   // brow (+X, browOuterUp peak)
        case 263: return 557;    // eye outer corner (-X)
        case 362: return 1370;   // eye inner corner (-X)
        case 291: return 5651;   // mouth corner (-X)
        case 334: return 349;    // brow (-X)
        default:  return -1;
    }
}

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

    const int tvc = tmpl.vertexCount();
    const auto& tn = tmpl.neutral();

    // Template-side marker vertices: CANONICAL constants for the ICT topology,
    // derived offline from the template's own blendshape deltas (the
    // mouthSmile peak IS the mouth corner, the eyeBlink-moved lid's lateral
    // extremes ARE the eye corners, gnathion = lowest front midline vertex).
    // Detection on the template's untextured render carries a systematic
    // detector bias (lower-face landmarks drift UP one anatomical step), and
    // that bias used to define the "ground truth" every user marker was
    // matched against. Falls back to template detection for a non-ICT
    // template (vertex count mismatch).
    const bool canonicalOk = (tvc == 26719);
    if (canonicalOk) {
        for (auto& m : markers)
            m.tmplVertex = canonicalTemplateVertex(m.mediapipeIndex);
    }
    // Template detection still runs even with canonical vertices: the
    // template is rendered + detected EXACTLY like the user mesh, so the
    // difference between where the detector puts a marker on the template and
    // its canonical vertex measures the detector's systematic bias on this
    // render style (lower-face landmarks drift up one anatomical step on
    // untextured statues) — which we then subtract from the user detections.
    MeshLandmarks tlm;
    {
        Ogre::Entity* tent = templateEntity(tmpl.neutral(), tmpl.faces());
        if (!tent) return markers;
        if (auto* tnode = tent->getParentSceneNode()) tnode->setVisible(true);
        tlm = detectMeshLandmarks(tent, tmpl.neutral(),
                                  std::vector<int>(tmpl.faces()));
        if (auto* tnode = tent->getParentSceneNode()) tnode->setVisible(false);
    }
    std::vector<std::array<float,3>> tlmSym(markers.size(), {0,0,0});
    std::vector<char> tlmSymOk(markers.size(), 0);
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
        for (size_t k = 0; k < markers.size(); ++k) {
            auto& m = markers[k];
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
            tlmSym[k] = p;
            tlmSymOk[k] = 1;
            if (!canonicalOk)
                m.tmplVertex = nearestTemplateVertex(tn, tvc, p);
        }
    }

    // User detection seeds the editable positions — but ONLY when the detected
    // constellation actually looks like a face layout. MediaPipe returns a
    // scattered garbage blob on cartoon/stylized faces, and garbage seeds
    // silently poison the warp/fit (measured: jawOpen deltas 50x too small).
    const MeshLandmarks ulm = detectMeshLandmarks(userEntity, userLocalV, userLocalF);

    // Detector-bias correction: transfer each marker's measured template-side
    // bias (symmetrized detection − canonical vertex) into the user's frame
    // (scaled by the template→user constellation size ratio) and subtract it
    // from the user detection. Template and user go through the same render +
    // detector, so the systematic statue bias cancels; what remains is the
    // user's actual anatomy.
    std::vector<std::array<float,3>> uCorr(markers.size(), {0,0,0});
    std::vector<char> uOk(markers.size(), 0);
    if (ulm.ok) {
        // constellation size ratio from raw detected pairs (bias-consistent
        // on both sides, so the ratio is unaffected by the bias itself)
        double sT = 0, sUsr = 0;
        {
            std::array<double,3> cT{0,0,0}, cUsr{0,0,0};
            int n = 0;
            for (size_t k = 0; k < markers.size(); ++k) {
                const int i = markers[k].mediapipeIndex;
                if (!tlmSymOk[k] || i < 0 || i >= int(ulm.points.size())
                    || !ulm.valid[size_t(i)]) continue;
                for (int d = 0; d < 3; ++d) {
                    cT[size_t(d)] += tlmSym[k][size_t(d)];
                    cUsr[size_t(d)] += ulm.points[size_t(i)][size_t(d)];
                }
                ++n;
            }
            if (n >= 4) {
                for (int d = 0; d < 3; ++d) { cT[size_t(d)] /= n; cUsr[size_t(d)] /= n; }
                for (size_t k = 0; k < markers.size(); ++k) {
                    const int i = markers[k].mediapipeIndex;
                    if (!tlmSymOk[k] || i < 0 || i >= int(ulm.points.size())
                        || !ulm.valid[size_t(i)]) continue;
                    double dt = 0, du = 0;
                    for (int d = 0; d < 3; ++d) {
                        const double a = tlmSym[k][size_t(d)] - cT[size_t(d)];
                        const double b = ulm.points[size_t(i)][size_t(d)] - cUsr[size_t(d)];
                        dt += a*a; du += b*b;
                    }
                    sT += std::sqrt(dt); sUsr += std::sqrt(du);
                }
            }
        }
        const double ratio = (canonicalOk && tlm.ok && sT > 1e-9)
                             ? sUsr / sT : 0.0;
        for (size_t k = 0; k < markers.size(); ++k) {
            const int i = markers[k].mediapipeIndex;
            if (i < 0 || i >= int(ulm.points.size()) || !ulm.valid[size_t(i)])
                continue;
            uCorr[k] = ulm.points[size_t(i)];
            uOk[k] = 1;
            const int cv = markers[k].tmplVertex;
            if (ratio > 0.0 && tlmSymOk[k] && cv >= 0) {
                for (int d = 0; d < 3; ++d) {
                    const float bias = tlmSym[k][size_t(d)]
                                       - tn[size_t(cv)*3 + size_t(d)];
                    uCorr[k][size_t(d)] -= float(ratio * bias);
                }
            }
        }
    }

    int seeded = 0;
    std::vector<std::array<float,3>> detC, detU;   // template/user pairs
    for (size_t k = 0; k < markers.size(); ++k) {
        if (!uOk[k] || markers[k].tmplVertex < 0) continue;
        ++seeded;
        const int cv = markers[k].tmplVertex;
        detC.push_back({tn[size_t(cv)*3], tn[size_t(cv)*3+1],
                        tn[size_t(cv)*3+2]});
        detU.push_back(uCorr[k]);
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
    if (std::getenv("QTMESH_FACERIG_DEBUG"))
        std::fprintf(stderr, "[facerig] seed: ulm.ok=%d conf=%.2f %d/%zu "
                     "detected, constellation residual %.3f -> %s\n",
                     ulm.ok, ulm.confidence, seeded, markers.size(), resid,
                     confident ? "trusted" : "using proportional defaults");

    if (confident) {
        // Consensus outlier correction: fit the similarity model the
        // constellation gate already uses (centroid + scale, no rotation)
        // and predict each marker from the TEMPLATE layout. Individual
        // detections on low-contrast renders drift most at the eye/mouth
        // corners (measured up to 0.59 units on the reference vs ~0.07
        // for the nose); a detection that deviates from the consensus
        // prediction by more than 2x the median is detector noise — replace
        // it with the prediction, snapped to the head surface.
        std::array<double,3> cC{0,0,0}, cU{0,0,0};
        const int np = int(detC.size());
        for (int i = 0; i < np; ++i)
            for (int d = 0; d < 3; ++d) {
                cC[size_t(d)] += detC[size_t(i)][size_t(d)] / np;
                cU[size_t(d)] += detU[size_t(i)][size_t(d)] / np;
            }
        double sC = 0, sU = 0;
        for (int i = 0; i < np; ++i) {
            double dc = 0, du = 0;
            for (int d = 0; d < 3; ++d) {
                const double a = detC[size_t(i)][size_t(d)] - cC[size_t(d)];
                const double b = detU[size_t(i)][size_t(d)] - cU[size_t(d)];
                dc += a*a; du += b*b;
            }
            sC += std::sqrt(dc); sU += std::sqrt(du);
        }
        const double scale = (sC > 1e-12) ? sU / sC : 1.0;

        auto predictOf = [&](int tmplVertex) -> std::array<float,3> {
            std::array<float,3> p;
            for (int d = 0; d < 3; ++d)
                p[size_t(d)] = float((double(tn[size_t(tmplVertex)*3 + d])
                                      - cC[size_t(d)]) * scale + cU[size_t(d)]);
            return p;
        };
        std::vector<double> devs;
        for (size_t k = 0; k < markers.size(); ++k) {
            const auto& m = markers[k];
            if (!uOk[k] || m.tmplVertex < 0) continue;
            const auto pred = predictOf(m.tmplVertex);
            const auto& det = uCorr[k];
            const double dx = det[0]-pred[0], dy = det[1]-pred[1],
                         dz = det[2]-pred[2];
            devs.push_back(std::sqrt(dx*dx + dy*dy + dz*dz));
        }
        std::vector<double> sorted = devs;
        std::sort(sorted.begin(), sorted.end());
        const double median = sorted.empty() ? 0.0
                              : sorted[sorted.size() / 2];
        const double outlierAt = std::max(2.0 * median, 1e-9);

        size_t di = 0;
        for (size_t k = 0; k < markers.size(); ++k) {
            auto& m = markers[k];
            if (!uOk[k]) continue;
            if (m.tmplVertex >= 0 && di < devs.size()
                && devs[di] > outlierAt) {
                // consensus prediction, snapped onto the head surface
                std::array<float,3> p = predictOf(m.tmplVertex);
                float best = 1e30f;
                std::array<float,3> snap = p;
                for (size_t v = 0; v + 2 < userLocalV.size(); v += 3) {
                    const float dx = userLocalV[v]   - p[0];
                    const float dy = userLocalV[v+1] - p[1];
                    const float dz = userLocalV[v+2] - p[2];
                    const float d2 = dx*dx + dy*dy + dz*dz;
                    if (d2 < best) {
                        best = d2;
                        snap = {userLocalV[v], userLocalV[v+1], userLocalV[v+2]};
                    }
                }
                m.userPos = snap;
                if (std::getenv("QTMESH_FACERIG_DEBUG"))
                    std::fprintf(stderr, "[facerig] seed outlier '%s' "
                                 "dev=%.3f (median %.3f) -> consensus\n",
                                 m.label.toUtf8().constData(), devs[di], median);
            } else {
                m.userPos = uCorr[k];
            }
            if (m.tmplVertex >= 0) ++di;
            m.placed = true;
        }
    } else {
        // Garbage / weak detection: seed EVERY marker at the head-box-projected
        // template position instead. Those proportional defaults measurably
        // produce a good rig on their own (the cartoon-face path), and the user
        // refines from there. placed=true so they act as anchors even if the
        // user rigs without touching them.
        //
        // FACING-AWARE: the straight box mapping assumes the user's face
        // points the template's way (+Z); a backwards-facing import (glb
        // round-trips flip facing) landed every default on the BACK of the
        // head. Even a too-weak-to-trust detection still tells us which
        // render view scored best — yaw the template coordinates to that
        // cardinal facing before the box mapping.
        int yaw = 0;   // 0:+Z (template) 1:-Z 2:+X 3:-X
        if (ulm.faceDirValid) {
            const float fx = ulm.faceDirLocal[0], fz = ulm.faceDirLocal[2];
            yaw = (std::fabs(fz) >= std::fabs(fx)) ? (fz >= 0.f ? 0 : 1)
                                                   : (fx >= 0.f ? 2 : 3);
        }
        auto yawRot = [yaw](std::array<float,3> p) -> std::array<float,3> {
            switch (yaw) {
                case 1: return {-p[0], p[1], -p[2]};   // 180°
                case 2: return { p[2], p[1], -p[0]};   // +Z → +X
                case 3: return {-p[2], p[1],  p[0]};   // +Z → -X
                default: return p;
            }
        };
        if (std::getenv("QTMESH_FACERIG_DEBUG"))
            std::fprintf(stderr, "[facerig] defaults: faceDirValid=%d "
                         "dir=(%.2f,%.2f,%.2f) yaw=%d\n",
                         ulm.faceDirValid ? 1 : 0, ulm.faceDirLocal[0],
                         ulm.faceDirLocal[1], ulm.faceDirLocal[2], yaw);
        std::array<float,3> lo{1e30f,1e30f,1e30f}, hi{-1e30f,-1e30f,-1e30f};
        const int unv = int(userLocalV.size()/3);
        for (int i = 0; i < unv; ++i)
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], userLocalV[size_t(i)*3+a]);
                hi[a] = std::max(hi[a], userLocalV[size_t(i)*3+a]);
            }
        std::array<float,3> tlo{1e30f,1e30f,1e30f}, thi{-1e30f,-1e30f,-1e30f};
        for (int i = 0; i < tvc; ++i) {
            const std::array<float,3> tv = yawRot({tn[size_t(i)*3],
                                                   tn[size_t(i)*3+1],
                                                   tn[size_t(i)*3+2]});
            for (int a = 0; a < 3; ++a) {
                tlo[a] = std::min(tlo[a], tv[size_t(a)]);
                thi[a] = std::max(thi[a], tv[size_t(a)]);
            }
        }
        // Depth axis + ray setup for surface snapping: the box mapping puts
        // markers at the box's proportional DEPTH, but protrusions (a cigar,
        // a long nose, hair) inflate the head box along the facing axis and
        // every default then floats off the face. Ray-cast each marker from
        // outside the box along the facing direction and take the first
        // surface hit as its depth instead.
        const int depthAxis = (yaw <= 1) ? 2 : 0;
        const float depthSign = (yaw == 0 || yaw == 2) ? 1.0f : -1.0f;
        const float margin = 0.25f * (hi[size_t(depthAxis)] - lo[size_t(depthAxis)]);
        Ogre::Vector3 rayDir = Ogre::Vector3::ZERO;
        rayDir[depthAxis] = -depthSign;   // from the face side into the head
        const int unvTot = int(userLocalV.size() / 3);

        for (auto& m : markers) {
            if (m.tmplVertex < 0) continue;
            const std::array<float,3> tv = yawRot({tn[size_t(m.tmplVertex)*3],
                                                   tn[size_t(m.tmplVertex)*3+1],
                                                   tn[size_t(m.tmplVertex)*3+2]});
            for (int a = 0; a < 3; ++a) {
                const float f = (thi[a]-tlo[a]) > 1e-6f
                    ? (tv[size_t(a)] - tlo[a]) / (thi[a]-tlo[a]) : 0.5f;
                m.userPos[size_t(a)] = lo[a] + f * (hi[a]-lo[a]);
            }
            // Snap to the head surface along the facing axis.
            Ogre::Vector3 o(m.userPos[0], m.userPos[1], m.userPos[2]);
            o[depthAxis] = depthSign > 0
                ? hi[size_t(depthAxis)] + margin
                : lo[size_t(depthAxis)] - margin;
            float bestT = std::numeric_limits<float>::max();
            bool hitAny = false;
            Ogre::Vector3 hit;
            for (size_t fI = 0; fI + 2 < userLocalF.size(); fI += 3) {
                const int ia = userLocalF[fI], ib = userLocalF[fI+1],
                          ic = userLocalF[fI+2];
                if (ia < 0 || ib < 0 || ic < 0
                    || ia >= unvTot || ib >= unvTot || ic >= unvTot) continue;
                auto vAt = [&](int k) {
                    return Ogre::Vector3(userLocalV[size_t(k)*3],
                                         userLocalV[size_t(k)*3+1],
                                         userLocalV[size_t(k)*3+2]);
                };
                float t;
                if (rayTri(o, rayDir, vAt(ia), vAt(ib), vAt(ic), t) && t < bestT) {
                    bestT = t; hit = o + rayDir * t; hitAny = true;
                }
            }
            if (hitAny)
                m.userPos = {hit.x, hit.y, hit.z};
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
