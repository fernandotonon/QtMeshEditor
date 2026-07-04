/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "AlembicImporter.h"

#include "Manager.h"
#include "SentryReporter.h"

#include <QFileInfo>

#ifdef ENABLE_ALEMBIC
#include <Alembic/Abc/All.h>
#include <Alembic/AbcCoreFactory/All.h>
#include <Alembic/AbcGeom/All.h>

#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMesh.h>
#include <OgreMeshManager.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSubMesh.h>

#include <algorithm>
#include <limits>
#endif

namespace AlembicImporter {

bool available()
{
#ifdef ENABLE_ALEMBIC
    return true;
#else
    return false;
#endif
}

#ifndef ENABLE_ALEMBIC

ReadResult readFrameSet(const QString&, int)
{
    ReadResult r;
    r.error = QStringLiteral(
        "Alembic import needs a build with -DENABLE_ALEMBIC (Alembic + Imath "
        "are not compiled in).");
    return r;
}

Ogre::SceneNode* importToScene(const QString&, QString* error)
{
    if (error)
        *error = QStringLiteral(
            "Alembic import needs a build with -DENABLE_ALEMBIC.");
    return nullptr;
}

InfoResult readInfo(const QString&)
{
    InfoResult r;
    r.error = QStringLiteral("Alembic info needs a build with -DENABLE_ALEMBIC.");
    return r;
}

#else  // ENABLE_ALEMBIC

using namespace Alembic::AbcGeom;

namespace {

// Depth-first search for the first IPolyMesh in the archive tree.
IPolyMesh findFirstPolyMesh(IObject obj)
{
    for (size_t i = 0; i < obj.getNumChildren(); ++i) {
        IObject child = obj.getChild(i);
        if (IPolyMesh::matches(child.getHeader()))
            return IPolyMesh(child, kWrapExisting);
        IPolyMesh found = findFirstPolyMesh(child);
        if (found.valid())
            return found;
    }
    return IPolyMesh();
}

}  // namespace

ReadResult readFrameSet(const QString& path, int maxFrames)
{
    ReadResult r;
    QFileInfo fi(path);
    if (!fi.exists()) {
        r.error = QStringLiteral("Alembic file not found: %1").arg(path);
        return r;
    }

    try {
        Alembic::AbcCoreFactory::IFactory factory;
        IArchive archive = factory.getArchive(path.toStdString());
        if (!archive.valid()) {
            r.error = QStringLiteral("Not a readable Alembic archive: %1").arg(path);
            return r;
        }

        IPolyMesh mesh = findFirstPolyMesh(archive.getTop());
        if (!mesh.valid()) {
            r.error = QStringLiteral("No polygon mesh found in %1").arg(fi.fileName());
            return r;
        }
        r.meshName = QString::fromStdString(mesh.getName());

        IPolyMeshSchema& schema = mesh.getSchema();
        const size_t numSamples = schema.getNumSamples();
        if (numSamples == 0) {
            r.error = QStringLiteral("Alembic mesh '%1' has no samples").arg(r.meshName);
            return r;
        }

        Alembic::AbcCoreAbstract::TimeSamplingPtr ts = schema.getTimeSampling();

        // Topology (vertex count) comes from the first sample; VAT_POSE needs a
        // fixed base, so reject a cache whose vertex count changes over time.
        IPolyMeshSchema::Sample first;
        schema.get(first, ISampleSelector(static_cast<index_t>(0)));
        const size_t baseVerts = first.getPositions()->size();
        if (baseVerts == 0) {
            r.error = QStringLiteral("Alembic mesh '%1' has zero vertices").arg(r.meshName);
            return r;
        }

        size_t decodeCount = numSamples;
        if (maxFrames > 0 && static_cast<size_t>(maxFrames) < decodeCount)
            decodeCount = static_cast<size_t>(maxFrames);

        VertexAnimationManager::FrameSet& fs = r.frames;
        fs.vertexCount = static_cast<int>(baseVerts);
        // fps from the sample spacing (uniform sampling → constant dt).
        const double dt = (numSamples > 1)
            ? (ts->getSampleTime(1) - ts->getSampleTime(0))
            : (1.0 / 30.0);
        fs.fps = (dt > 1e-9) ? static_cast<int>(std::lround(1.0 / dt)) : 30;
        if (fs.fps <= 0) fs.fps = 30;

        float mn[3] = { std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max() };
        float mx[3] = { -std::numeric_limits<float>::max(),
                        -std::numeric_limits<float>::max(),
                        -std::numeric_limits<float>::max() };

        for (size_t s = 0; s < decodeCount; ++s) {
            IPolyMeshSchema::Sample samp;
            schema.get(samp, ISampleSelector(static_cast<index_t>(s)));
            Abc::P3fArraySamplePtr P = samp.getPositions();
            if (!P || P->size() != baseVerts) {
                r.error = QStringLiteral(
                    "Alembic mesh '%1' changes vertex count between frames "
                    "(frame %2: %3 vs %4) — not a fixed-topology vertex cache")
                    .arg(r.meshName).arg(s)
                    .arg(P ? P->size() : 0).arg(baseVerts);
                r.frames = {};
                return r;
            }
            VertexAnimationManager::FrameData fd;
            fd.time = static_cast<float>(ts->getSampleTime(s) - ts->getSampleTime(0));
            fd.positions.resize(baseVerts * 3);
            for (size_t v = 0; v < baseVerts; ++v) {
                const Imath::V3f& p = (*P)[v];
                fd.positions[v * 3 + 0] = p.x;
                fd.positions[v * 3 + 1] = p.y;
                fd.positions[v * 3 + 2] = p.z;
                mn[0] = std::min(mn[0], p.x); mx[0] = std::max(mx[0], p.x);
                mn[1] = std::min(mn[1], p.y); mx[1] = std::max(mx[1], p.y);
                mn[2] = std::min(mn[2], p.z); mx[2] = std::max(mx[2], p.z);
            }
            fs.frames.push_back(std::move(fd));
        }
        fs.aabb = { mn[0], mn[1], mn[2], mx[0], mx[1], mx[2] };

        r.ok = fs.ok();
        if (!r.ok)
            r.error = QStringLiteral(
                "Alembic mesh '%1' produced fewer than 2 frames — nothing to animate")
                .arg(r.meshName);
        return r;
    } catch (const std::exception& e) {
        r.error = QStringLiteral("Alembic read error: %1").arg(QString::fromUtf8(e.what()));
        r.frames = {};
        return r;
    }
}

InfoResult readInfo(const QString& path)
{
    InfoResult r;
    QFileInfo fi(path);
    if (!fi.exists()) {
        r.error = QStringLiteral("Alembic file not found: %1").arg(path);
        return r;
    }
    try {
        Alembic::AbcCoreFactory::IFactory factory;
        IArchive archive = factory.getArchive(path.toStdString());
        if (!archive.valid()) {
            r.error = QStringLiteral("Not a readable Alembic archive: %1").arg(path);
            return r;
        }
        IPolyMesh mesh = findFirstPolyMesh(archive.getTop());
        if (!mesh.valid()) {
            r.error = QStringLiteral("No polygon mesh found in %1").arg(fi.fileName());
            return r;
        }
        IPolyMeshSchema& schema = mesh.getSchema();
        const size_t numSamples = schema.getNumSamples();
        Alembic::AbcCoreAbstract::TimeSamplingPtr ts = schema.getTimeSampling();
        IPolyMeshSchema::Sample first;
        schema.get(first, ISampleSelector(static_cast<index_t>(0)));

        r.meshName    = QString::fromStdString(mesh.getName());
        r.frameCount  = static_cast<int>(numSamples);
        r.vertexCount = first.getPositions() ? static_cast<int>(first.getPositions()->size()) : 0;
        // Sum triangles across n-gon faces (fan triangulation = n-2 per face).
        int tris = 0;
        if (auto fc = first.getFaceCounts())
            for (size_t f = 0; f < fc->size(); ++f)
                tris += std::max(0, (*fc)[f] - 2);
        r.faceCount = tris;
        const double dt = (numSamples > 1)
            ? (ts->getSampleTime(1) - ts->getSampleTime(0)) : (1.0 / 30.0);
        r.fps = (dt > 1e-9) ? static_cast<int>(std::lround(1.0 / dt)) : 30;
        if (r.fps <= 0) r.fps = 30;
        r.durationSec = (numSamples > 1)
            ? static_cast<float>(ts->getSampleTime(numSamples - 1) - ts->getSampleTime(0))
            : 0.0f;
        r.storage = (VertexAnimationManager::sampleHeuristic(r.frameCount)
                     == VertexAnimationManager::Storage::Poses) ? "poses" : "stream";
        r.ok = (r.vertexCount > 0 && r.frameCount > 0);
        if (!r.ok)
            r.error = QStringLiteral("Alembic mesh '%1' has no usable samples").arg(r.meshName);
        return r;
    } catch (const std::exception& e) {
        r.error = QStringLiteral("Alembic info error: %1").arg(QString::fromUtf8(e.what()));
        return r;
    }
}

// Build a static base Ogre::Mesh (frame-0 topology + positions) with POSITION +
// NORMAL, non-shared submesh 0, so buildClipFromFrames' VAT_POSE poses target
// submesh handle 1 — matching the morph/vertex-anim convention.
static Ogre::MeshPtr buildBaseMesh(const QString& name,
                                   const QString& abcPath,
                                   const VertexAnimationManager::FrameSet& fs)
{
    // Re-read topology (face indices) from the archive's first sample.
    std::vector<uint32_t> indices;
    try {
        Alembic::AbcCoreFactory::IFactory factory;
        IArchive archive = factory.getArchive(abcPath.toStdString());
        IPolyMesh mesh = findFirstPolyMesh(archive.getTop());
        IPolyMeshSchema::Sample first;
        mesh.getSchema().get(first, ISampleSelector(static_cast<index_t>(0)));
        Abc::Int32ArraySamplePtr faceIdx = first.getFaceIndices();
        Abc::Int32ArraySamplePtr faceCnt = first.getFaceCounts();
        // Fan-triangulate each n-gon face (Alembic faces are arbitrary polygons).
        size_t cursor = 0;
        for (size_t f = 0; f < faceCnt->size(); ++f) {
            const int n = (*faceCnt)[f];
            for (int t = 1; t + 1 < n; ++t) {
                indices.push_back(static_cast<uint32_t>((*faceIdx)[cursor]));
                indices.push_back(static_cast<uint32_t>((*faceIdx)[cursor + t]));
                indices.push_back(static_cast<uint32_t>((*faceIdx)[cursor + t + 1]));
            }
            cursor += static_cast<size_t>(n);
        }
    } catch (const std::exception&) {
        return Ogre::MeshPtr();
    }
    if (indices.empty()) return Ogre::MeshPtr();

    const int vcount = fs.vertexCount;
    const std::vector<float>& pos0 = fs.frames.front().positions;

    auto& mm = Ogre::MeshManager::getSingleton();
    if (mm.resourceExists(name.toStdString()))
        mm.remove(name.toStdString());
    Ogre::MeshPtr om = mm.createManual(
        name.toStdString(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* sub = om->createSubMesh();
    sub->useSharedVertices = false;
    sub->vertexData = new Ogre::VertexData();
    sub->vertexData->vertexCount = static_cast<size_t>(vcount);
    auto* decl = sub->vertexData->vertexDeclaration;
    size_t off = 0;
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    off += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, off, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), vcount, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    std::vector<float> vdata(static_cast<size_t>(vcount) * 6, 0.0f);
    for (int v = 0; v < vcount; ++v) {
        vdata[v * 6 + 0] = pos0[v * 3 + 0];
        vdata[v * 6 + 1] = pos0[v * 3 + 1];
        vdata[v * 6 + 2] = pos0[v * 3 + 2];
        vdata[v * 6 + 5] = 1.0f;  // placeholder +Z normal (recomputed by shading)
    }
    vbuf->writeData(0, vdata.size() * sizeof(float), vdata.data());
    sub->vertexData->vertexBufferBinding->setBinding(0, vbuf);

    const bool use32 = vcount > 65535;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        use32 ? Ogre::HardwareIndexBuffer::IT_32BIT : Ogre::HardwareIndexBuffer::IT_16BIT,
        indices.size(), Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    if (use32) {
        ibuf->writeData(0, indices.size() * sizeof(uint32_t), indices.data());
    } else {
        std::vector<uint16_t> i16(indices.size());
        for (size_t i = 0; i < indices.size(); ++i)
            i16[i] = static_cast<uint16_t>(indices[i]);
        ibuf->writeData(0, i16.size() * sizeof(uint16_t), i16.data());
    }
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = indices.size();

    Ogre::AxisAlignedBox aabb(fs.aabb[0], fs.aabb[1], fs.aabb[2],
                              fs.aabb[3], fs.aabb[4], fs.aabb[5]);
    om->_setBounds(aabb);
    om->_setBoundingSphereRadius(0.5f * aabb.getSize().length());
    om->load();
    return om;
}

Ogre::SceneNode* importToScene(const QString& path, QString* error)
{
    auto fail = [&](const QString& m) -> Ogre::SceneNode* {
        if (error) *error = m;
        return nullptr;
    };

    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.vertex_anim"),
                                  QStringLiteral("import Alembic %1")
                                      .arg(QFileInfo(path).fileName()));

    // Decode heuristic-capped: pose storage tops out at 32 frames, but we still
    // decode all here (streaming is B3). Cap conservatively so a pathological
    // multi-thousand-frame cache doesn't hang the import before B3 lands.
    ReadResult rr = readFrameSet(path, /*maxFrames=*/512);
    if (!rr.ok)
        return fail(rr.error);

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getSceneMgr())
        return fail(QStringLiteral("No active scene to import into."));

    const QString base = QFileInfo(path).completeBaseName();
    const QString meshName = base + QStringLiteral("_abc");
    Ogre::MeshPtr om = buildBaseMesh(meshName, path, rr.frames);
    if (!om)
        return fail(QStringLiteral("Failed to build base mesh from %1").arg(base));

    const QString clip = base + QStringLiteral("_cache");
    if (!VertexAnimationManager::buildClipFromFrames(om.get(), clip, rr.frames))
        return fail(QStringLiteral("Failed to build vertex-animation clip."));

    Ogre::SceneNode* node = mgr->addSceneNode(base + QStringLiteral("_abc_node"));
    if (!node)
        return fail(QStringLiteral("Failed to create scene node."));
    mgr->createEntity(node, om);

    SentryReporter::addBreadcrumb(
        QStringLiteral("scene.anim.vertex_anim"),
        QStringLiteral("imported Alembic '%1' — %2 frames, %3 verts")
            .arg(rr.meshName).arg(rr.frames.frames.size()).arg(rr.frames.vertexCount));
    return node;
}

#endif  // ENABLE_ALEMBIC

}  // namespace AlembicImporter
