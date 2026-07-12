/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "VertexAnimationManager.h"

#include "SelectionSet.h"
#include "SentryReporter.h"

#include <QCoreApplication>
#include <QThread>

#include <OgreAnimation.h>
#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreMesh.h>
#include <OgrePose.h>
#include <OgreSubMesh.h>
#include <OgreVertexIndexData.h>

#include <cmath>

namespace {

// Singletons run on the main thread (CLAUDE.md). Assert at lifecycle entry.
inline void assertMainThread()
{
    Q_ASSERT(QCoreApplication::instance());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
}

// The issue's cutoff: caches below this frame count store as blend-able poses.
constexpr int kPoseStorageMaxFrames = 32;

// Read submesh-0 (or shared) bind positions into a flat xyz array. Returns the
// vertex count (0 on failure). Mirrors gatherGeometry's position walk but for a
// single target-geometry the vertex-anim poses are built against.
int readBindPositions(Ogre::Mesh* mesh, std::vector<float>& out)
{
    out.clear();
    if (!mesh || mesh->getNumSubMeshes() == 0) return 0;
    Ogre::SubMesh* sub = mesh->getSubMesh(0);
    Ogre::VertexData* vd = sub->useSharedVertices ? mesh->sharedVertexData
                                                  : sub->vertexData;
    if (!vd) return 0;
    const Ogre::VertexElement* posElem =
        vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
    if (!posElem) return 0;
    auto vbuf = vd->vertexBufferBinding->getBuffer(posElem->getSource());
    if (!vbuf) return 0;

    const size_t count = vd->vertexCount;
    out.resize(count * 3);
    auto* base = static_cast<unsigned char*>(vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    for (size_t v = 0; v < count; ++v) {
        float* p = nullptr;
        posElem->baseVertexPointerToElement(base + v * vbuf->getVertexSize(), &p);
        out[v * 3 + 0] = p[0];
        out[v * 3 + 1] = p[1];
        out[v * 3 + 2] = p[2];
    }
    vbuf->unlock();
    return static_cast<int>(count);
}

} // namespace

VertexAnimationManager* VertexAnimationManager::s_instance = nullptr;

VertexAnimationManager* VertexAnimationManager::instance()
{
    assertMainThread();
    if (!s_instance) s_instance = new VertexAnimationManager();
    return s_instance;
}

VertexAnimationManager* VertexAnimationManager::qmlInstance(QQmlEngine*, QJSEngine*)
{
    assertMainThread();
    auto* inst = instance();
    // Process-wide singleton shared across every QQuickWidget's QQmlEngine.
    // Pin CppOwnership so no engine's GC can delete the shared instance and
    // leave a dangling pointer for the others — matching every sibling
    // singleton's qmlInstance (see MorphAnimationManager for the full note).
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void VertexAnimationManager::kill()
{
    assertMainThread();
    if (!s_instance) return;
    delete s_instance;
    s_instance = nullptr;
}

VertexAnimationManager::VertexAnimationManager(QObject* parent) : QObject(parent)
{
    if (auto* sel = SelectionSet::getSingleton()) {
        connect(sel, &SelectionSet::selectionChanged,
                this, &VertexAnimationManager::vertexAnimationsChanged);
    }
}

VertexAnimationManager::~VertexAnimationManager() = default;

VertexAnimationManager::Storage VertexAnimationManager::sampleHeuristic(int frameCount)
{
    return frameCount < kPoseStorageMaxFrames ? Storage::Poses : Storage::Stream;
}

bool VertexAnimationManager::buildClipFromFrames(Ogre::Mesh* mesh,
                                                 const QString& clipName,
                                                 const FrameSet& frames)
{
    if (!mesh || clipName.isEmpty() || !frames.ok())
        return false;

    std::vector<float> bind;
    const int vcount = readBindPositions(mesh, bind);
    if (vcount <= 0 || vcount != frames.vertexCount)
        return false;

    const std::string animName = clipName.toStdString();
    if (mesh->hasAnimation(animName))
        mesh->removeAnimation(animName);

    // Rebuilding the same clip must also drop the dense per-frame poses it
    // created last time ("<clip>/frameN"). Ogre poses are mesh-level and are
    // still walked by the dope-sheet / export paths, so without this a
    // re-import appends stale poses (and shifts every pose index). removePose
    // is index-based, so collect matching indices and erase from the back to
    // keep the remaining indices stable. (Same pattern as MorphCommands.)
    {
        const std::string prefix = animName + "/frame";
        const auto& poseList = mesh->getPoseList();
        std::vector<unsigned short> drop;
        for (unsigned short pi = 0; pi < poseList.size(); ++pi) {
            if (poseList[pi] && poseList[pi]->getName().compare(0, prefix.size(), prefix) == 0)
                drop.push_back(pi);
        }
        for (auto it = drop.rbegin(); it != drop.rend(); ++it)
            mesh->removePose(*it);
    }

    // VAT_POSE targets submesh handle 1 (submesh 0); 0 is shared geometry.
    Ogre::SubMesh* sub = mesh->getSubMesh(0);
    const unsigned short target = sub->useSharedVertices ? 0 : 1;

    const float length = frames.frames.back().time - frames.frames.front().time;
    Ogre::Animation* anim =
        mesh->createAnimation(animName, length > 0.0f ? length : 0.0f);
    if (!anim) return false;
    Ogre::VertexAnimationTrack* track =
        anim->createVertexTrack(target, Ogre::VAT_POSE);
    if (!track) { mesh->removeAnimation(animName); return false; }

    const float t0 = frames.frames.front().time;
    // One Pose per frame (delta vs bind) + one keyframe at that frame's time
    // referencing it at full weight. VAT_POSE interpolates the pose references
    // between consecutive keyframes, so scrubbing the timeline blends frames.
    for (size_t f = 0; f < frames.frames.size(); ++f) {
        const FrameData& fd = frames.frames[f];
        if (static_cast<int>(fd.positions.size()) != vcount * 3) {
            mesh->removeAnimation(animName);
            return false;
        }
        const unsigned short poseIndex =
            static_cast<unsigned short>(mesh->getPoseCount());
        Ogre::Pose* pose = mesh->createPose(
            target, clipName.toStdString() + "/frame" + std::to_string(f));
        for (int v = 0; v < vcount; ++v) {
            const Ogre::Vector3 delta(fd.positions[v * 3 + 0] - bind[v * 3 + 0],
                                      fd.positions[v * 3 + 1] - bind[v * 3 + 1],
                                      fd.positions[v * 3 + 2] - bind[v * 3 + 2]);
            // Store every vertex (dense by nature); a zero delta is harmless.
            pose->addVertex(static_cast<size_t>(v), delta);
        }
        auto* kf = track->createVertexPoseKeyFrame(fd.time - t0);
        kf->addPoseReference(poseIndex, 1.0f);
    }

    mesh->load();
    SentryReporter::addBreadcrumb(
        QStringLiteral("scene.anim.vertex_anim"),
        QStringLiteral("built VAT_POSE clip '%1' — %2 frames, %3 verts")
            .arg(clipName).arg(frames.frames.size()).arg(vcount));
    return true;
}

bool VertexAnimationManager::hasVertexAnimation(Ogre::Entity* entity) const
{
    return !vertexClipsFor(entity).isEmpty();
}

QStringList VertexAnimationManager::vertexClipsFor(Ogre::Entity* entity) const
{
    QStringList out;
    if (!entity || !entity->getMesh()) return out;
    Ogre::MeshPtr mesh = entity->getMesh();
    for (unsigned short i = 0; i < mesh->getNumAnimations(); ++i) {
        Ogre::Animation* a = mesh->getAnimation(i);
        if (!a) continue;
        // A vertex-anim clip is a mesh Animation carrying a vertex track. (This
        // also matches morph clips; the "Mesh" dope-sheet row treats them the
        // same — a single scrubbable vertex row.)
        bool hasVertexTrack = false;
        for (const auto& kv : a->_getVertexTrackList()) {
            if (kv.second) { hasVertexTrack = true; break; }
        }
        if (hasVertexTrack)
            out << QString::fromStdString(a->getName());
    }
    return out;
}

QStringList VertexAnimationManager::vertexClipsForSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return {};
    const auto entities = sel->getResolvedEntities();
    return entities.isEmpty() ? QStringList{} : vertexClipsFor(entities.first());
}

bool VertexAnimationManager::selectionHasVertexAnimation() const
{
    return !vertexClipsForSelection().isEmpty();
}
