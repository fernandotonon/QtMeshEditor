/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "AlembicExporter.h"

#include "Manager.h"
#include "SentryReporter.h"

#include <QFile>
#include <QFileInfo>

#ifdef ENABLE_ALEMBIC
#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/AbcGeom/All.h>

#include <OgreAnimationState.h>
#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMesh.h>
#include <OgreRoot.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubEntity.h>
#include <OgreSubMesh.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#endif

namespace AlembicExporter {

bool available()
{
#ifdef ENABLE_ALEMBIC
    return true;
#else
    return false;
#endif
}

#ifndef ENABLE_ALEMBIC

WriteResult write(const QString&, const Scene&)
{
    WriteResult r;
    r.error = QStringLiteral(
        "Alembic export needs a build with -DENABLE_ALEMBIC (Alembic + Imath "
        "are not compiled in).");
    return r;
}

Result exportEntities(const QList<Ogre::Entity*>&, const QString&, const Options&)
{
    Result r;
    r.error = QStringLiteral("Alembic export needs a build with -DENABLE_ALEMBIC.");
    return r;
}

#else  // ENABLE_ALEMBIC

using namespace Alembic::AbcGeom;

namespace {

// Does `v` differ from `ref` anywhere? Exact comparison on purpose: the
// collapse to a single sample is only for data that did not move at all.
template <typename T>
bool allEqualToFirst(const std::vector<T>& frames)
{
    for (size_t i = 1; i < frames.size(); ++i)
        if (frames[i] != frames.front())
            return false;
    return true;
}

QString validate(const Scene& scene, int* outFrames)
{
    if (!(scene.fps > 0.0))
        return QStringLiteral("fps must be > 0");
    if (scene.shapes.empty())
        return QStringLiteral("nothing to export (no meshes)");

    size_t frames = 1;
    for (const Shape& s : scene.shapes) {
        frames = std::max(frames, s.framePositions.size());
        frames = std::max(frames, s.frameXforms.size());
    }

    for (const Shape& s : scene.shapes) {
        const QString who = s.meshName;
        if (s.framePositions.empty() || s.framePositions.front().empty()
            || s.framePositions.front().size() % 3 != 0)
            return QStringLiteral("mesh '%1' has no vertex positions").arg(who);
        const size_t floats = s.framePositions.front().size();
        const size_t verts = floats / 3;
        for (size_t f = 1; f < s.framePositions.size(); ++f)
            if (s.framePositions[f].size() != floats)
                return QStringLiteral("mesh '%1' changes vertex count at frame %2 "
                                      "(%3 vs %4) — Alembic needs fixed topology")
                    .arg(who).arg(f).arg(s.framePositions[f].size() / 3).arg(verts);
        if (s.framePositions.size() != 1 && s.framePositions.size() != frames)
            return QStringLiteral("mesh '%1' has %2 position frames, expected 1 or %3")
                .arg(who).arg(s.framePositions.size()).arg(frames);
        if (!s.frameXforms.empty() && s.frameXforms.size() != 1
            && s.frameXforms.size() != frames)
            return QStringLiteral("mesh '%1' has %2 transform frames, expected 1 or %3")
                .arg(who).arg(s.frameXforms.size()).arg(frames);

        if (s.faceCounts.empty())
            return QStringLiteral("mesh '%1' has no faces").arg(who);
        size_t corners = 0;
        for (int32_t c : s.faceCounts) {
            if (c < 3)
                return QStringLiteral("mesh '%1' has a face with %2 corners").arg(who).arg(c);
            corners += static_cast<size_t>(c);
        }
        if (corners != s.faceIndices.size())
            return QStringLiteral("mesh '%1': face counts sum to %2 but %3 indices given")
                .arg(who).arg(corners).arg(s.faceIndices.size());
        for (int32_t i : s.faceIndices)
            if (i < 0 || static_cast<size_t>(i) >= verts)
                return QStringLiteral("mesh '%1': index %2 is outside %3 vertices")
                    .arg(who).arg(i).arg(verts);
        if (!s.uvs.empty() && s.uvs.size() != verts * 2)
            return QStringLiteral("mesh '%1': %2 UV floats for %3 vertices")
                .arg(who).arg(s.uvs.size()).arg(verts);
    }
    *outFrames = static_cast<int>(frames);
    return QString();
}

struct Writer {
    std::unique_ptr<OXform> xform;
    std::unique_ptr<OPolyMesh> mesh;
};

}  // namespace

WriteResult write(const QString& path, const Scene& scene)
{
    WriteResult r;
    int frames = 0;
    const QString bad = validate(scene, &frames);
    if (!bad.isEmpty()) {
        r.error = bad;
        return r;
    }

    const QString tmpPath = path + QStringLiteral(".qtmesh_tmp");
    QFile::remove(tmpPath);

    try {
        {
            Alembic::Abc::MetaData md;
            md.set(Alembic::Abc::kApplicationNameKey, "QtMeshEditor");
            OArchive archive(Alembic::AbcCoreOgawa::WriteArchive(),
                             tmpPath.toStdString(), md);
            const Alembic::Util::uint32_t tsIdx =
                archive.addTimeSampling(TimeSampling(1.0 / scene.fps, 0.0));

            std::vector<Writer> writers(scene.shapes.size());
            std::vector<std::vector<Imath::V2f>> uvStore(scene.shapes.size());
            std::vector<std::vector<Imath::V3f>> posStore(scene.shapes.size());
            // Alembic winds faces clockwise, Ogre counter-clockwise: reverse the
            // corner order of every face (AlembicImporter swaps them back).
            std::vector<std::vector<int32_t>> idxStore(scene.shapes.size());
            for (size_t i = 0; i < scene.shapes.size(); ++i) {
                const Shape& s = scene.shapes[i];
                writers[i].xform = std::make_unique<OXform>(
                    OObject(archive, kTop), s.xformName.toStdString(), tsIdx);
                writers[i].mesh = std::make_unique<OPolyMesh>(
                    *writers[i].xform, s.meshName.toStdString(), tsIdx);
                const size_t verts = s.framePositions.front().size() / 3;
                uvStore[i].resize(s.uvs.size() / 2);
                for (size_t v = 0; v < uvStore[i].size(); ++v)
                    uvStore[i][v] = Imath::V2f(s.uvs[v * 2], s.uvs[v * 2 + 1]);
                posStore[i].resize(verts);
                idxStore[i] = s.faceIndices;
                size_t cursor = 0;
                for (int32_t n : s.faceCounts) {
                    std::reverse(idxStore[i].begin() + cursor, idxStore[i].begin() + cursor + n);
                    cursor += static_cast<size_t>(n);
                }
            }

            // A shape whose data never moves keeps ONE sample.
            auto posSamples = [&](const Shape& s) {
                return allEqualToFirst(s.framePositions) ? size_t(1)
                                                         : s.framePositions.size();
            };
            auto xfSamples = [&](const Shape& s) {
                return allEqualToFirst(s.frameXforms) ? std::min<size_t>(s.frameXforms.size(), 1)
                                                      : s.frameXforms.size();
            };

            for (int f = 0; f < frames; ++f) {
                for (size_t i = 0; i < scene.shapes.size(); ++i) {
                    const Shape& s = scene.shapes[i];

                    const size_t nx = xfSamples(s);
                    const bool needIdentity = (f == 0 && nx == 0);
                    if (needIdentity || static_cast<size_t>(f) < nx) {
                        XformSample xs;
                        if (nx > 0) {
                            const auto& a = s.frameXforms[static_cast<size_t>(f)];
                            Imath::M44d m;
                            for (int rr = 0; rr < 4; ++rr)
                                for (int cc = 0; cc < 4; ++cc)
                                    m[rr][cc] = a[static_cast<size_t>(rr * 4 + cc)];
                            xs.setMatrix(m);
                        }
                        writers[i].xform->getSchema().set(xs);
                    }

                    if (static_cast<size_t>(f) >= posSamples(s))
                        continue;
                    const std::vector<float>& pf = s.framePositions[static_cast<size_t>(f)];
                    for (size_t v = 0; v < posStore[i].size(); ++v)
                        posStore[i][v] = Imath::V3f(pf[v * 3], pf[v * 3 + 1], pf[v * 3 + 2]);
                    const P3fArraySample pos(posStore[i].data(), posStore[i].size());

                    OPolyMeshSchema::Sample samp;
                    samp.setPositions(pos);
                    if (f == 0) {
                        samp.setFaceIndices(Int32ArraySample(
                            idxStore[i].data(), idxStore[i].size()));
                        samp.setFaceCounts(Int32ArraySample(
                            s.faceCounts.data(), s.faceCounts.size()));
                        if (!uvStore[i].empty()) {
                            OV2fGeomParam::Sample uvSamp(
                                V2fArraySample(uvStore[i].data(), uvStore[i].size()),
                                kVertexScope);
                            samp.setUVs(uvSamp);
                        }
                    }
                    writers[i].mesh->getSchema().set(samp);
                }
            }
            r.shapeCount = static_cast<int>(scene.shapes.size());
            r.frameCount = frames;
        }  // archive flushed + closed here
    } catch (const std::exception& e) {
        QFile::remove(tmpPath);
        r = WriteResult();
        r.error = QStringLiteral("Alembic write error: %1").arg(QString::fromUtf8(e.what()));
        return r;
    }

    QFile::remove(path);
    if (!QFile::rename(tmpPath, path)) {
        QFile::remove(tmpPath);
        r = WriteResult();
        r.error = QStringLiteral("could not write %1").arg(path);
        return r;
    }
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
// Ogre adapter
// ---------------------------------------------------------------------------

namespace {

struct Topology {
    std::vector<int32_t> faceCounts;
    std::vector<int32_t> faceIndices;
    std::vector<float>   uvs;
    size_t vertexCount = 0;
};

// Submesh walk shared by topology, UV and position readers: shared vertex
// data is visited once (at its first user), each non-shared submesh owns its
// range. Same order as VATBaker / NormalVisualizer.
template <typename Fn>
void walkVertexBlocks(Ogre::Mesh* mesh, Fn&& fn)
{
    bool sharedDone = false;
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub) continue;
        if (sub->useSharedVertices) {
            if (sharedDone) continue;
            sharedDone = true;
        }
        fn(si, sub);
    }
}

const Ogre::VertexData* deformedData(Ogre::Entity* e, Ogre::Mesh* mesh,
                                     Ogre::SubMesh* sub, unsigned short si)
{
    if (sub->useSharedVertices) {
        if (e->hasSkeleton()) return e->_getSkelAnimVertexData();
        if (mesh->getSharedVertexDataAnimationType() != Ogre::VAT_NONE)
            return e->_getSoftwareVertexAnimVertexData();
        return mesh->sharedVertexData;
    }
    Ogre::SubEntity* se = e->getSubEntity(si);
    if (!se) return nullptr;
    if (e->hasSkeleton()) return se->_getSkelAnimVertexData();
    if (sub->getVertexAnimationType() != Ogre::VAT_NONE)
        return se->_getSoftwareVertexAnimVertexData();
    return sub->vertexData;
}

// Append `semantic` (set 0) of every vertex as `comps` floats. Returns false
// when the element is missing or has an unexpected type.
bool appendElement(const Ogre::VertexData* vd, Ogre::VertexElementSemantic sem,
                   Ogre::VertexElementType type, int comps, std::vector<float>& out)
{
    if (!vd) return false;
    const auto* elem = vd->vertexDeclaration->findElementBySemantic(sem, 0);
    if (!elem || elem->getType() != type) return false;
    auto vbuf = vd->vertexBufferBinding->getBuffer(elem->getSource());
    if (!vbuf) return false;
    auto* bytes = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    const size_t stride = vbuf->getVertexSize();
    for (size_t j = 0; j < vd->vertexCount; ++j) {
        float* p = nullptr;
        elem->baseVertexPointerToElement(bytes + j * stride, &p);
        for (int c = 0; c < comps; ++c) out.push_back(p[c]);
    }
    vbuf->unlock();
    return true;
}

bool buildTopology(Ogre::Entity* entity, Topology& topo, QString* error)
{
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) { *error = QStringLiteral("entity has no mesh"); return false; }

    bool sharedPlaced = false;
    size_t sharedBase = 0;
    bool uvOk = true;
    int skippedSubmeshes = 0;

    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub) continue;

        const Ogre::VertexData* bind = sub->useSharedVertices ? mesh->sharedVertexData
                                                              : sub->vertexData;
        if (!bind) continue;

        size_t base;
        if (sub->useSharedVertices) {
            if (!sharedPlaced) {
                sharedPlaced = true;
                sharedBase = topo.vertexCount;
                topo.vertexCount += bind->vertexCount;
                uvOk = appendElement(bind, Ogre::VES_TEXTURE_COORDINATES,
                                     Ogre::VET_FLOAT2, 2, topo.uvs) && uvOk;
            }
            base = sharedBase;
        } else {
            base = topo.vertexCount;
            topo.vertexCount += bind->vertexCount;
            uvOk = appendElement(bind, Ogre::VES_TEXTURE_COORDINATES,
                                 Ogre::VET_FLOAT2, 2, topo.uvs) && uvOk;
        }

        Ogre::IndexData* id = sub->indexData;
        if (!id || !id->indexBuffer || id->indexCount == 0) continue;
        const auto op = sub->operationType;
        if (op != Ogre::RenderOperation::OT_TRIANGLE_LIST
            && op != Ogre::RenderOperation::OT_TRIANGLE_STRIP
            && op != Ogre::RenderOperation::OT_TRIANGLE_FAN) {
            ++skippedSubmeshes;
            continue;
        }

        const bool is32 = id->indexBuffer->getType() == Ogre::HardwareIndexBuffer::IT_32BIT;
        const size_t isz = is32 ? 4 : 2;
        std::vector<uint32_t> idx(id->indexCount);
        auto* raw = static_cast<unsigned char*>(id->indexBuffer->lock(
            id->indexStart * isz, id->indexCount * isz, Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (size_t i = 0; i < id->indexCount; ++i)
            idx[i] = is32 ? reinterpret_cast<const uint32_t*>(raw)[i]
                          : reinterpret_cast<const uint16_t*>(raw)[i];
        id->indexBuffer->unlock();

        auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
            if (a == b || b == c || a == c) return;   // degenerate (strip restarts)
            topo.faceCounts.push_back(3);
            topo.faceIndices.push_back(static_cast<int32_t>(base + a));
            topo.faceIndices.push_back(static_cast<int32_t>(base + b));
            topo.faceIndices.push_back(static_cast<int32_t>(base + c));
        };
        if (op == Ogre::RenderOperation::OT_TRIANGLE_LIST) {
            for (size_t i = 0; i + 2 < idx.size(); i += 3) tri(idx[i], idx[i + 1], idx[i + 2]);
        } else if (op == Ogre::RenderOperation::OT_TRIANGLE_STRIP) {
            for (size_t i = 0; i + 2 < idx.size(); ++i)
                (i & 1) ? tri(idx[i + 1], idx[i], idx[i + 2]) : tri(idx[i], idx[i + 1], idx[i + 2]);
        } else {
            for (size_t i = 1; i + 1 < idx.size(); ++i) tri(idx[0], idx[i], idx[i + 1]);
        }
    }

    if (topo.faceCounts.empty() || topo.vertexCount == 0) {
        *error = QStringLiteral("mesh has no triangles to export");
        return false;
    }
    if (skippedSubmeshes > 0)
        Ogre::LogManager::getSingleton().logWarning(
            "Alembic export: skipped " + std::to_string(skippedSubmeshes)
            + " non-triangle submesh(es) of " + entity->getName());

    // Ogre stores V top-down; Alembic (like glTF-to-DCC) expects V up.
    if (!uvOk || topo.uvs.size() != topo.vertexCount * 2) {
        topo.uvs.clear();
    } else {
        for (size_t v = 0; v < topo.vertexCount; ++v)
            topo.uvs[v * 2 + 1] = 1.0f - topo.uvs[v * 2 + 1];
    }
    return true;
}

bool readDeformedPositions(Ogre::Entity* entity, size_t expectedVerts, std::vector<float>& out)
{
    entity->_updateAnimation();
    Ogre::MeshPtr mesh = entity->getMesh();
    out.clear();
    out.reserve(expectedVerts * 3);
    bool ok = true;
    walkVertexBlocks(mesh.get(), [&](unsigned short si, Ogre::SubMesh* sub) {
        const Ogre::VertexData* vd = deformedData(entity, mesh.get(), sub, si);
        ok = appendElement(vd, Ogre::VES_POSITION, Ogre::VET_FLOAT3, 3, out) && ok;
    });
    return ok && out.size() == expectedVerts * 3;
}

struct StateSnap {
    Ogre::AnimationState* state;
    bool enabled;
    float time;
    bool loop;
    float weight;
};

StateSnap snap(Ogre::AnimationState* s)
{
    return { s, s->getEnabled(), s->getTimePosition(), s->getLoop(), s->getWeight() };
}

void restore(const StateSnap& n)
{
    n.state->setLoop(n.loop);
    n.state->setWeight(n.weight);
    n.state->setTimePosition(n.time);
    n.state->setEnabled(n.enabled);
}

struct NodeSnap {
    Ogre::SceneNode* node;
    Ogre::Vector3 pos;
    Ogre::Quaternion rot;
    Ogre::Vector3 scale;
};

void collectNodes(Ogre::SceneNode* n, std::vector<NodeSnap>& out)
{
    out.push_back({ n, n->getPosition(), n->getOrientation(), n->getScale() });
    for (auto* c : n->getChildren())
        if (auto* sn = dynamic_cast<Ogre::SceneNode*>(c))
            collectNodes(sn, out);
}

std::array<double, 16> imathLayout(const Ogre::Matrix4& m)
{
    std::array<double, 16> a{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            a[static_cast<size_t>(r * 4 + c)] = m[c][r];   // transpose: column- → row-vector
    return a;
}

}  // namespace

Result exportEntities(const QList<Ogre::Entity*>& entities, const QString& path,
                      const Options& opts)
{
    Result res;
    auto fail = [&](const QString& m) { res.error = m; return res; };

    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
        QStringLiteral("export format=abc %1").arg(QFileInfo(path).fileName()));

    if (entities.isEmpty()) return fail(QStringLiteral("no entities to export"));
    if (!(opts.fps > 0.0)) return fail(QStringLiteral("fps must be > 0"));
    if (path.isEmpty()) return fail(QStringLiteral("output path is empty"));
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr())
        return fail(QStringLiteral("no active scene"));
    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();

    // ---- topology (once per entity) --------------------------------------
    struct Item {
        Ogre::Entity* entity;
        Topology topo;
        Shape shape;
    };
    std::vector<Item> items;
    std::set<QString> usedNames;
    auto unique = [&](QString base) {
        QString n = base;
        for (int i = 2; usedNames.count(n); ++i) n = base + QString::number(i);
        usedNames.insert(n);
        return n;
    };
    for (Ogre::Entity* e : entities) {
        if (!e) continue;
        Item it;
        it.entity = e;
        QString err;
        if (!buildTopology(e, it.topo, &err))
            return fail(QStringLiteral("%1: %2").arg(QString::fromStdString(e->getName()), err));
        it.shape.faceCounts = it.topo.faceCounts;
        it.shape.faceIndices = it.topo.faceIndices;
        it.shape.uvs = it.topo.uvs;
        const QString nodeName = e->getParentSceneNode()
            ? QString::fromStdString(e->getParentSceneNode()->getName())
            : QString::fromStdString(e->getName());
        it.shape.xformName = unique(nodeName);
        it.shape.meshName = unique(QString::fromStdString(e->getName()) + QStringLiteral("Shape"));
        items.push_back(std::move(it));
    }
    if (items.empty()) return fail(QStringLiteral("no entities to export"));

    // ---- which clips drive the bake ---------------------------------------
    const std::string wanted = opts.animationName.toStdString();
    std::vector<StateSnap> allSnaps;     // everything we may touch (restored after)
    std::vector<Ogre::AnimationState*> driven;
    bool wantedFound = wanted.empty();

    for (auto& it : items) {
        auto* set = it.entity->getAllAnimationStates();
        if (!set) continue;
        for (const auto& kv : set->getAnimationStates()) {
            Ogre::AnimationState* s = kv.second;
            allSnaps.push_back(snap(s));
            if (!wanted.empty()) {
                if (kv.first == wanted) { driven.push_back(s); wantedFound = true; }
            } else if (s->getEnabled()) {
                driven.push_back(s);
            }
        }
    }
    // Node clips live on the SceneManager. Without an explicit name every
    // node clip rides along; with one, only a same-named node clip does.
    if (opts.nodeAnimations) {
        for (const auto& kv : sceneMgr->getAnimationStates()) {
            Ogre::AnimationState* s = kv.second;
            allSnaps.push_back(snap(s));
            if (wanted.empty() || kv.first == wanted) {
                driven.push_back(s);
                wantedFound = true;
            }
        }
    }
    if (!wantedFound)
        return fail(QStringLiteral("animation '%1' not found on the exported entities")
                        .arg(opts.animationName));
    // Nothing playing and no node clips: an animated asset exported as one
    // static frame would surprise, so bake its longest clip instead.
    if (driven.empty() && wanted.empty()) {
        Ogre::AnimationState* best = nullptr;
        for (const auto& sn : allSnaps)
            if (!best || sn.state->getLength() > best->getLength())
                best = sn.state;
        if (best && best->getLength() > 0.0f) driven.push_back(best);
    }
    std::sort(driven.begin(), driven.end());
    driven.erase(std::unique(driven.begin(), driven.end()), driven.end());

    float maxLen = 0.0f;
    for (auto* s : driven) maxLen = std::max(maxLen, s->getLength());
    const double t0 = (opts.startTime < 0.0) ? 0.0 : opts.startTime;
    const double t1 = (opts.endTime < 0.0) ? static_cast<double>(maxLen) : opts.endTime;
    const bool animated = !driven.empty() && t1 > t0;
    if (!driven.empty() && opts.endTime >= 0.0 && t1 <= t0)
        return fail(QStringLiteral("endTime (%1) must be > startTime (%2)").arg(t1).arg(t0));
    const double span = animated ? (t1 - t0) : 0.0;
    const int frames = animated ? static_cast<int>(std::floor(span * opts.fps + 1e-6)) + 1 : 1;

    // ---- sample ------------------------------------------------------------
    std::vector<NodeSnap> nodeSnaps;
    collectNodes(sceneMgr->getRootSceneNode(), nodeSnaps);

    for (auto& s : allSnaps) {
        if (std::find(driven.begin(), driven.end(), s.state) == driven.end())
            s.state->setEnabled(false);
    }
    for (auto* s : driven) {
        s->setEnabled(true);
        s->setWeight(1.0f);
        s->setLoop(false);   // clamp at the end instead of Ogre's fmod wrap
    }
    for (auto& it : items) it.entity->addSoftwareAnimationRequest(true);

    QString sampleError;
    for (int f = 0; f < frames && sampleError.isEmpty(); ++f) {
        const double t = t0 + static_cast<double>(f) / opts.fps;
        for (auto* s : driven)
            s->setTimePosition(static_cast<Ogre::Real>(std::min<double>(t, s->getLength())));
        for (auto& it : items)
            if (auto* set = it.entity->getAllAnimationStates()) set->_notifyDirty();
        sceneMgr->_applySceneAnimations();
        sceneMgr->getRootSceneNode()->_update(true, false);
        // Bone matrices are cached per Root frame number; headless bakes never
        // render, so bump it by hand (the VATBaker lesson).
        Ogre::FrameEvent ev{};
        Ogre::Root::getSingleton()._fireFrameRenderingQueued(ev);

        for (auto& it : items) {
            std::vector<float> pos;
            if (!readDeformedPositions(it.entity, it.topo.vertexCount, pos)) {
                sampleError = QStringLiteral("frame %1 of %2: could not read deformed "
                                             "positions").arg(f)
                                  .arg(QString::fromStdString(it.entity->getName()));
                break;
            }
            it.shape.framePositions.push_back(std::move(pos));
            Ogre::SceneNode* node = it.entity->getParentSceneNode();
            it.shape.frameXforms.push_back(imathLayout(
                node ? Ogre::Matrix4(node->_getFullTransform()) : Ogre::Matrix4::IDENTITY));
        }
    }

    // ---- restore -----------------------------------------------------------
    for (auto& it : items) it.entity->removeSoftwareAnimationRequest(true);
    for (const auto& s : allSnaps) restore(s);
    for (const auto& n : nodeSnaps) {
        n.node->setPosition(n.pos);
        n.node->setOrientation(n.rot);
        n.node->setScale(n.scale);
    }
    for (auto& it : items)
        if (auto* set = it.entity->getAllAnimationStates()) set->_notifyDirty();

    if (!sampleError.isEmpty()) return fail(sampleError);

    Scene scene;
    scene.fps = opts.fps;
    for (auto& it : items) {
        res.vertexCount += static_cast<int>(it.topo.vertexCount);
        scene.shapes.push_back(std::move(it.shape));
    }
    const WriteResult wr = write(path, scene);
    if (!wr.ok) return fail(wr.error);

    res.ok = true;
    res.frameCount = wr.frameCount;
    res.shapeCount = wr.shapeCount;
    res.animated = animated;
    for (auto* d : driven)
        res.animationsUsed << QString::fromStdString(d->getAnimationName());
    res.durationSec = animated ? static_cast<double>(frames - 1) / opts.fps : 0.0;
    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
        QStringLiteral("exported format=abc %1 shapes=%2 frames=%3")
            .arg(QFileInfo(path).fileName()).arg(res.shapeCount).arg(res.frameCount));
    return res;
}

#endif  // ENABLE_ALEMBIC

}  // namespace AlembicExporter
