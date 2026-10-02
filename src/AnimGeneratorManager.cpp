/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "AnimGeneratorManager.h"

#include "AnimationControlController.h"
#include "LightManager.h"
#include "Manager.h"
#include "MorphAnimationManager.h"
#include "NodeAnimationManager.h"
#include "OgreWidget.h"
#include "PoseLibrary.h"
#include "SentryReporter.h"
#include "SpaceCamera.h"
#include "UndoManager.h"
#include "commands/AnimGeneratorCommands.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QQmlEngine>
#include <QSaveFile>
#include <QSet>

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreAnimationTrack.h>
#include <OgreBone.h>
#include <OgreCamera.h>
#include <OgreEntity.h>
#include <OgreKeyFrame.h>
#include <OgreLight.h>
#include <OgreManualObject.h>
#include <OgreMaterialManager.h>
#include <OgreMesh.h>
#include <OgrePass.h>
#include <OgrePose.h>
#include <OgreRay.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSkeletonInstance.h>
#include <OgreTechnique.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace AnimGen;

namespace {

constexpr double kEps = 1e-4;
constexpr float kPickRadiusPx = 12.0f;
constexpr const char* kDefaultNodeClip = "Generators";
constexpr const char* kPathLineMat = "AnimGen/PathLine";
constexpr const char* kPathPointMat = "AnimGen/PathPoint";
constexpr const char* kOverlayNodeName = "Unnamed_AnimGenPathOverlay";
constexpr const char* kOverlayObjName = "AnimGenPathOverlayObj";

Ogre::SceneManager* sceneMgr()
{
    Manager* m = Manager::getSingletonPtr();
    return m ? m->getSceneMgr() : nullptr;
}

Ogre::Entity* entityByName(const QString& n)
{
    Ogre::SceneManager* s = sceneMgr();
    if (!s || n.isEmpty()) return nullptr;
    const std::string sn = n.toStdString();
    return s->hasEntity(sn) ? s->getEntity(sn) : nullptr;
}

Ogre::SceneNode* nodeByName(const QString& n)
{
    Ogre::SceneManager* s = sceneMgr();
    if (!s || n.isEmpty()) return nullptr;
    const std::string sn = n.toStdString();
    return s->hasSceneNode(sn) ? s->getSceneNode(sn) : nullptr;
}

Ogre::Animation* skeletalAnim(Ogre::Entity* e, const QString& clip)
{
    if (!e || !e->hasSkeleton() || clip.isEmpty()) return nullptr;
    Ogre::SkeletonInstance* sk = e->getSkeleton();
    const std::string c = clip.toStdString();
    return sk->hasAnimation(c) ? sk->getAnimation(c) : nullptr;
}

Ogre::Animation* sceneAnim(const QString& clip)
{
    Ogre::SceneManager* s = sceneMgr();
    if (!s || clip.isEmpty()) return nullptr;
    const std::string c = clip.toStdString();
    return s->hasAnimation(c) ? s->getAnimation(c) : nullptr;
}

Ogre::NodeAnimationTrack* nodeTrackFor(Ogre::Animation* a, Ogre::Node* n)
{
    if (!a || !n) return nullptr;
    for (const auto& kv : a->_getNodeTrackList())
        if (kv.second && kv.second->getAssociatedNode() == n) return kv.second;
    return nullptr;
}

Ogre::Light* lightByName(const QString& n)
{
    LightManager* lm = LightManager::getSingletonPtr();
    if (!lm) return nullptr;
    LightHandle* h = lm->findLight(n);
    return (h && h->isValid()) ? h->light : nullptr;
}

Ogre::MaterialPtr materialByName(const QString& n)
{
    if (n.isEmpty() || !Ogre::MaterialManager::getSingletonPtr()) return {};
    return Ogre::MaterialManager::getSingleton().getByName(n.toStdString());
}

int axisOf(const QString& channel)
{
    if (channel.endsWith(QLatin1String(".x")) || channel.endsWith(QLatin1String(".r"))) return 0;
    if (channel.endsWith(QLatin1String(".y")) || channel.endsWith(QLatin1String(".g"))) return 1;
    if (channel.endsWith(QLatin1String(".z")) || channel.endsWith(QLatin1String(".b"))) return 2;
    if (channel.endsWith(QLatin1String(".a"))) return 3;
    return -1;
}

Ogre::Vector3 axisVec(int a)
{
    return a == 0 ? Ogre::Vector3::UNIT_X : a == 1 ? Ogre::Vector3::UNIT_Y : Ogre::Vector3::UNIT_Z;
}

struct TRS {
    Ogre::Vector3 p = Ogre::Vector3::ZERO;
    Ogre::Quaternion r = Ogre::Quaternion::IDENTITY;
    Ogre::Vector3 s = Ogre::Vector3::UNIT_SCALE;
};
using TrsKeys = std::vector<std::pair<double, TRS>>;

QJsonObject trsKeyJson(double t, const TRS& k)
{
    return QJsonObject{
        {QStringLiteral("t"), t},
        {QStringLiteral("p"), QJsonArray{k.p.x, k.p.y, k.p.z}},
        {QStringLiteral("r"), QJsonArray{k.r.w, k.r.x, k.r.y, k.r.z}},
        {QStringLiteral("s"), QJsonArray{k.s.x, k.s.y, k.s.z}},
    };
}

TrsKeys trsKeysFromJson(const QJsonArray& arr)
{
    TrsKeys out;
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        const QJsonArray p = o.value(QStringLiteral("p")).toArray();
        const QJsonArray r = o.value(QStringLiteral("r")).toArray();
        const QJsonArray s = o.value(QStringLiteral("s")).toArray();
        if (p.size() != 3 || r.size() != 4 || s.size() != 3) continue;
        TRS k;
        k.p = Ogre::Vector3(float(p[0].toDouble()), float(p[1].toDouble()), float(p[2].toDouble()));
        k.r = Ogre::Quaternion(float(r[0].toDouble()), float(r[1].toDouble()), float(r[2].toDouble()),
                               float(r[3].toDouble()));
        k.s = Ogre::Vector3(float(s[0].toDouble()), float(s[1].toDouble()), float(s[2].toDouble()));
        out.emplace_back(o.value(QStringLiteral("t")).toDouble(), k);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return out;
}

QJsonArray trsKeysJson(const TrsKeys& keys)
{
    QJsonArray a;
    for (const auto& k : keys) a.append(trsKeyJson(k.first, k.second));
    return a;
}

TrsKeys readTrsKeys(Ogre::NodeAnimationTrack* track)
{
    TrsKeys out;
    if (!track) return out;
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = track->getNodeKeyFrame(i);
        TRS k;
        k.p = kf->getTranslate();
        k.r = kf->getRotation();
        k.s = kf->getScale();
        out.emplace_back(double(kf->getTime()), k);
    }
    return out;
}

void writeTrsKeys(Ogre::NodeAnimationTrack* track, const TrsKeys& keys)
{
    if (!track) return;
    track->removeAllKeyFrames();
    for (const auto& k : keys) {
        auto* kf = track->createNodeKeyFrame(float(k.first));
        kf->setTranslate(k.second.p);
        kf->setRotation(k.second.r);
        kf->setScale(k.second.s);
    }
    track->_keyFrameDataChanged();
}

// Morph (VAT_POSE) keys: refs by pose NAME — indices are an artefact of the
// mesh's pose-list order and need not survive a re-import.
struct PoseKey {
    double t = 0.0;
    std::vector<std::pair<QString, double>> refs;
};

double poseInfluence(const PoseKey& k, const QString& name)
{
    for (const auto& r : k.refs) if (r.first == name) return r.second;
    return 0.0; // Ogre interpolates a pose missing from a keyframe toward 0
}

int poseIndex(const Ogre::MeshPtr& mesh, unsigned short handle, const QString& name)
{
    const auto& poses = mesh->getPoseList();
    const std::string n = name.toStdString();
    for (size_t i = 0; i < poses.size(); ++i)
        if (poses[i] && poses[i]->getTarget() == handle && poses[i]->getName() == n) return int(i);
    return -1;
}

std::vector<PoseKey> readPoseKeys(const Ogre::MeshPtr& mesh, Ogre::VertexAnimationTrack* track)
{
    std::vector<PoseKey> out;
    if (!track) return out;
    const auto& poses = mesh->getPoseList();
    for (unsigned short i = 0; i < track->getNumKeyFrames(); ++i) {
        auto* kf = track->getVertexPoseKeyFrame(i);
        PoseKey k;
        k.t = kf->getTime();
        for (const auto& ref : kf->getPoseReferences()) {
            if (ref.poseIndex < poses.size() && poses[ref.poseIndex])
                k.refs.emplace_back(QString::fromStdString(poses[ref.poseIndex]->getName()), ref.influence);
        }
        out.push_back(std::move(k));
    }
    return out;
}

void writePoseKeys(const Ogre::MeshPtr& mesh, unsigned short handle, Ogre::VertexAnimationTrack* track,
                   const std::vector<PoseKey>& keys)
{
    if (!track) return;
    track->removeAllKeyFrames();
    for (const auto& k : keys) {
        auto* kf = track->createVertexPoseKeyFrame(float(k.t));
        for (const auto& r : k.refs) {
            const int idx = poseIndex(mesh, handle, r.first);
            if (idx >= 0) kf->addPoseReference(ushort(idx), float(r.second));
        }
    }
    track->_keyFrameDataChanged();
}

QJsonArray poseKeysJson(const std::vector<PoseKey>& keys)
{
    QJsonArray arr;
    for (const auto& k : keys) {
        QJsonArray refs;
        for (const auto& r : k.refs) refs.append(QJsonArray{r.first, r.second});
        arr.append(QJsonObject{{QStringLiteral("t"), k.t}, {QStringLiteral("refs"), refs}});
    }
    return arr;
}

std::vector<PoseKey> poseKeysFromJson(const QJsonArray& arr)
{
    std::vector<PoseKey> out;
    for (const QJsonValue& v : arr) {
        const QJsonObject o = v.toObject();
        PoseKey k;
        k.t = o.value(QStringLiteral("t")).toDouble();
        for (const QJsonValue& r : o.value(QStringLiteral("refs")).toArray()) {
            const QJsonArray p = r.toArray();
            if (p.size() == 2) k.refs.emplace_back(p[0].toString(), p[1].toDouble());
        }
        out.push_back(std::move(k));
    }
    std::sort(out.begin(), out.end(), [](const PoseKey& a, const PoseKey& b) { return a.t < b.t; });
    return out;
}

/// Ogre's VAT_POSE interpolation: linear per pose, a pose absent from one of
/// the two bracketing keys counts as 0 there.
double evalPose(const std::vector<PoseKey>& keys, const QString& name, double t)
{
    if (keys.empty()) return 0.0;
    if (t <= keys.front().t) return poseInfluence(keys.front(), name);
    if (t >= keys.back().t) return poseInfluence(keys.back(), name);
    for (size_t i = 0; i + 1 < keys.size(); ++i) {
        if (t >= keys[i].t && t <= keys[i + 1].t) {
            const double span = keys[i + 1].t - keys[i].t;
            const double f = span > 1e-9 ? (t - keys[i].t) / span : 0.0;
            const double a = poseInfluence(keys[i], name), b = poseInfluence(keys[i + 1], name);
            return a + (b - a) * f;
        }
    }
    return poseInfluence(keys.back(), name);
}

/// Every generator evaluates through the same helper so a path sampler is
/// built once per materialisation, not once per sample.
struct GenEval {
    const Generator* g = nullptr;
    PathSampler path;
};

std::vector<GenEval> prepare(const std::vector<const Generator*>& gens)
{
    std::vector<GenEval> out;
    out.reserve(gens.size());
    for (const Generator* g : gens) {
        GenEval e;
        e.g = g;
        if (g->type == Type::FollowPath) e.path = PathSampler(g->pathPoints, g->pathClosed);
        out.push_back(std::move(e));
    }
    return out;
}

/// Compose base TRS with the generators at time t. `bone` (non-null for a
/// bone target) converts a path position into the bone's bind-relative key.
TRS composeTrs(const TRS& base, const std::vector<GenEval>& evals, double t, double clipLen, const Ogre::Bone* bone)
{
    TRS out = base;
    // Replacements first (follow-path), then the additive layers on top.
    for (const auto& e : evals) {
        if (e.g->type != Type::FollowPath || e.path.empty()) continue;
        const double s = pathParameter(*e.g, t, clipLen);
        const Ogre::Vector3 p = e.path.position(s, e.g->constantSpeed);
        out.p = bone ? p - bone->getInitialPosition() : p;
        if (e.g->orientToPath) {
            const Ogre::Quaternion q = orientationAlong(e.path.tangent(s, e.g->constantSpeed));
            out.r = bone ? bone->getInitialOrientation().Inverse() * q : q;
        }
    }
    for (const auto& e : evals) {
        if (e.g->type == Type::FollowPath) continue;
        const double v = evaluateScalar(*e.g, t, clipLen);
        const QString& ch = e.g->target.channel;
        const int a = axisOf(ch);
        if (a < 0 || a > 2) continue;
        if (ch.startsWith(QLatin1String("position"))) {
            out.p += axisVec(a) * float(v);
        } else if (ch.startsWith(QLatin1String("rotation"))) {
            out.r = out.r * Ogre::Quaternion(Ogre::Degree(float(v)), axisVec(a));
        } else if (ch.startsWith(QLatin1String("scale"))) {
            out.s[a] += float(v);
        }
    }
    out.r.normalise();
    return out;
}

/// The union of the generators' windows clipped to [0, clipLen] (clipLen <= 0
/// = unbounded), plus the densest sampling rate among them.
bool window(const std::vector<const Generator*>& gens, double clipLen, double* start, double* end, int* fps)
{
    if (gens.empty()) return false;
    double s = std::numeric_limits<double>::max(), e = -std::numeric_limits<double>::max();
    int f = 1;
    for (const Generator* g : gens) {
        s = std::min(s, g->startTime);
        e = std::max(e, windowEnd(*g, clipLen));
        f = std::max(f, g->sampleFps);
    }
    s = std::max(0.0, s);
    if (clipLen > 0.0) e = std::min(e, clipLen);
    if (e <= s) return false;
    *start = s;
    *end = e;
    *fps = f;
    return true;
}

void refreshSkeletalPlayback(Ogre::Entity* e)
{
    if (!e) return;
    if (Ogre::AnimationStateSet* states = e->getAllAnimationStates()) {
        states->_notifyDirty();
        for (auto& kv : states->getAnimationStates())
            if (kv.second->getEnabled()) kv.second->setTimePosition(kv.second->getTimePosition());
    }
    if (auto* acc = AnimationControlController::instance()) acc->notifyExternalAnimationEdit();
}

Ogre::MaterialPtr ensurePathMaterial(const char* name, float pointSize)
{
    auto& mm = Ogre::MaterialManager::getSingleton();
    Ogre::MaterialPtr mat = mm.getByName(name);
    if (mat) return mat;
    mat = mm.create(name, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    pass->setLightingEnabled(false);
    pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
    pass->setCullingMode(Ogre::CULL_NONE);
    pass->setDepthCheckEnabled(false);
    pass->setDepthWriteEnabled(false);
    pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
    if (pointSize > 0.0f) {
        pass->setPointSize(pointSize);
        pass->setPointSpritesEnabled(false);
    }
    return mat;
}

bool screenRay(OgreWidget* widget, const QPoint& p, Ogre::Ray* ray, Ogre::Camera** camOut)
{
    if (!widget) return false;
    SpaceCamera* sc = widget->getSpaceCamera();
    Ogre::Camera* cam = sc ? sc->getCamera() : nullptr;
    if (!cam) return false;
    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    if (vw <= 0 || vh <= 0) return false;
    *ray = cam->getCameraToViewportRay(float(p.x()) / vw, float(p.y()) / vh);
    if (camOut) *camOut = cam;
    return true;
}

bool projectToScreen(const Ogre::Camera* cam, OgreWidget* widget, const Ogre::Vector3& world, float* sx, float* sy)
{
    int vw = 0, vh = 0;
    widget->pixelSizeForCameraPicking(vw, vh);
    const Ogre::Vector4 clip = cam->getProjectionMatrix() * (cam->getViewMatrix() * Ogre::Vector4(world, 1.0f));
    if (clip.w <= 1e-6f) return false;
    *sx = (clip.x / clip.w * 0.5f + 0.5f) * vw;
    *sy = (1.0f - (clip.y / clip.w * 0.5f + 0.5f)) * vh;
    return true;
}

/// Runtime base keys as (t, v) pairs; malformed entries are skipped.
std::vector<std::pair<double, double>> runtimeKeys(const QJsonObject& base)
{
    std::vector<std::pair<double, double>> out;
    for (const QJsonValue& v : base.value(QStringLiteral("keys")).toArray()) {
        const QJsonArray p = v.toArray();
        if (p.size() == 2 && p.at(0).isDouble() && p.at(1).isDouble())
            out.emplace_back(p.at(0).toDouble(), p.at(1).toDouble());
    }
    return out;
}
void crumb(const char* op, const QString& msg)
{
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.generator.%1").arg(QLatin1String(op)), msg);
}

} // namespace

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

AnimGeneratorManager* AnimGeneratorManager::m_pSingleton = nullptr;

AnimGeneratorManager* AnimGeneratorManager::instance()
{
    if (!m_pSingleton) m_pSingleton = new AnimGeneratorManager();
    return m_pSingleton;
}

AnimGeneratorManager* AnimGeneratorManager::qmlInstance(QQmlEngine*, QJSEngine*)
{
    AnimGeneratorManager* m = instance();
    QQmlEngine::setObjectOwnership(m, QQmlEngine::CppOwnership);
    return m;
}

void AnimGeneratorManager::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

AnimGeneratorManager::AnimGeneratorManager()
{
    ensureSceneHook();
}

void AnimGeneratorManager::ensureSceneHook()
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr || m_hookedManager == mgr) return;
    m_hookedManager = mgr;
    connect(mgr, &Manager::sceneClearing, this, &AnimGeneratorManager::discardForSceneClear);
}

void AnimGeneratorManager::discardForSceneClear()
{
    // The scene is about to destroy every target; there is nothing left to
    // restore, only state to drop. The overlay goes with the scene graph.
    m_overlayObj = nullptr;
    m_overlayNode = nullptr;
    m_dragActive = false;
    const bool hadPath = !m_pathEditId.isEmpty();
    m_pathEditId.clear();
    m_selectedPoint = -1;
    const bool hadGens = !m_gens.empty() || !m_bases.isEmpty();
    m_gens.clear();
    m_bases.clear();
    if (hadPath) emit pathEditChanged();
    if (hadGens) {
        emit generatorsChanged();
        crumb("clear", QStringLiteral("scene replaced"));
    }
}

AnimGeneratorManager::~AnimGeneratorManager()
{
    destroyPathOverlay();
}

void AnimGeneratorManager::setStatus(const QString& text, bool ok)
{
    m_status = text;
    m_lastOk = ok;
    emit statusChanged();
}

const Generator* AnimGeneratorManager::find(const QString& id) const
{
    for (const auto& g : m_gens) if (g.id == id) return &g;
    return nullptr;
}

bool AnimGeneratorManager::isBound(const QString& id) const
{
    const Generator* g = find(id);
    return g && m_bases.contains(trackKey(g->target));
}

AnimGen::Generator* AnimGeneratorManager::findMutable(const QString& id)
{
    for (auto& g : m_gens) if (g.id == id) return &g;
    return nullptr;
}

QString AnimGeneratorManager::trackKey(const Target& t)
{
    switch (t.kind) {
    case TargetKind::Bone:
        return QStringLiteral("bone:%1/%2@%3").arg(t.object, t.sub, t.clip);
    case TargetKind::Node:
        return QStringLiteral("node:%1@%2").arg(t.object, t.clip);
    case TargetKind::Morph:
        return QStringLiteral("morph:%1@%2").arg(t.object, t.clip);
    default:
        return formatTarget(t);
    }
}

std::vector<const Generator*> AnimGeneratorManager::activeFor(const QString& key) const
{
    std::vector<const Generator*> out;
    for (const auto& g : m_gens)
        if (g.enabled && !g.baked && trackKey(g.target) == key) out.push_back(&g);
    return out;
}

QStringList AnimGeneratorManager::referencedKeys(const std::vector<Generator>& gens) const
{
    QStringList keys;
    for (const auto& g : gens) {
        const QString k = trackKey(g.target);
        if (!keys.contains(k)) keys << k;
    }
    return keys;
}

// ---------------------------------------------------------------------------
// Documents
// ---------------------------------------------------------------------------

QJsonObject AnimGeneratorManager::makeDocument(const Doc& d)
{
    QJsonObject doc = toDocument(d.gens);
    QJsonArray bases;
    QStringList keys = d.bases.keys();
    keys.sort(); // byte-stable sidecars
    for (const QString& k : keys) {
        QJsonObject b = d.bases.value(k);
        b[QStringLiteral("key")] = k;
        bases.append(b);
    }
    doc[QStringLiteral("bases")] = bases;
    return doc;
}

bool AnimGeneratorManager::parseDocument(const QJsonObject& doc, Doc* out, QString* error)
{
    Doc d;
    if (!fromDocument(doc, &d.gens, error)) return false;
    for (const QJsonValue& v : doc.value(QStringLiteral("bases")).toArray()) {
        QJsonObject b = v.toObject();
        const QString k = b.take(QStringLiteral("key")).toString();
        if (!k.isEmpty()) d.bases.insert(k, b);
    }
    *out = std::move(d);
    return true;
}

QJsonObject AnimGeneratorManager::document() const
{
    return makeDocument(current());
}

bool AnimGeneratorManager::applyDocument(const QJsonObject& docJson, QString* error)
{
    Doc next;
    if (!parseDocument(docJson, &next, error)) return false;

    const QStringList newKeys = referencedKeys(next.gens);
    // Tracks that leave the document go back to their base first.
    const QStringList oldKeys = m_bases.keys();
    for (const QString& k : oldKeys)
        if (!newKeys.contains(k)) restoreBase(k, m_bases.value(k));

    m_gens = std::move(next.gens);
    QHash<QString, QJsonObject> bases;
    for (const QString& k : newKeys)
        if (next.bases.contains(k)) bases.insert(k, next.bases.value(k));
    m_bases = std::move(bases);

    QStringList failures;
    for (const QString& k : newKeys) {
        const Generator* first = nullptr;
        for (const auto& g : m_gens) if (trackKey(g.target) == k) { first = &g; break; }
        if (!first) continue;
        if (!m_bases.contains(k)) {
            QJsonObject base;
            QString why;
            if (!captureBase(k, first->target, &base, &why)) { failures << why; continue; }
            m_bases.insert(k, base);
        }
        QJsonObject base = m_bases.value(k);
        const auto active = activeFor(k);
        if (kindIsTrackBacked(first->target.kind)) {
            if (active.empty()) restoreBase(k, base);
            else materialise(k, &base, active);
            m_bases.insert(k, base);
        } else if (active.empty()) {
            // Muted / baked runtime property: put the base back once (a
            // multi-key baked curve keeps being driven by tick()).
            restoreBase(k, base);
        }
    }
    if (pathEditActive() && !find(m_pathEditId)) endPathEdit();
    refreshPathOverlay();
    emit generatorsChanged();
    if (!failures.isEmpty() && error) *error = failures.join(QStringLiteral("; "));
    return failures.isEmpty();
}

void AnimGeneratorManager::clear()
{
    endPathEdit();
    for (auto it = m_bases.cbegin(); it != m_bases.cend(); ++it) restoreBase(it.key(), it.value());
    m_gens.clear();
    m_bases.clear();
    emit generatorsChanged();
}

AnimGeneratorManager::Result AnimGeneratorManager::commit(const Doc& next, const QString& label,
                                                         const QString& id, bool undoable)
{
    Result r;
    r.id = id;
    const QJsonObject before = document();
    QString err;
    const bool ok = applyDocument(makeDocument(next), &err);
    const QJsonObject after = document();
    if (undoable) {
        if (UndoManager* um = UndoManager::getSingleton())
            um->push(new AnimGeneratorDocCommand(label, before, after));
    }
    r.ok = ok;
    r.error = err;
    setStatus(ok ? label : err, ok);
    return r;
}

// ---------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------

bool AnimGeneratorManager::resolveTarget(Target* t, QString* error) const
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    switch (t->kind) {
    case TargetKind::Bone: {
        Ogre::Entity* e = entityByName(t->object);
        if (!e) return fail(QStringLiteral("no entity named '%1'").arg(t->object));
        if (!e->hasSkeleton()) return fail(QStringLiteral("'%1' has no skeleton").arg(t->object));
        Ogre::SkeletonInstance* sk = e->getSkeleton();
        if (!sk->hasBone(t->sub.toStdString()))
            return fail(QStringLiteral("'%1' has no bone '%2'").arg(t->object, t->sub));
        if (t->clip.isEmpty()) {
            auto* acc = AnimationControlController::instance();
            if (acc && acc->selectedEntityName() == t->object && skeletalAnim(e, acc->selectedAnimation()))
                t->clip = acc->selectedAnimation();
            else if (sk->getNumAnimations() > 0)
                t->clip = QString::fromStdString(sk->getAnimation(0)->getName());
        }
        if (!skeletalAnim(e, t->clip))
            return fail(t->clip.isEmpty()
                ? QStringLiteral("'%1' has no skeletal animation to add a bone generator to").arg(t->object)
                : QStringLiteral("'%1' has no skeletal clip '%2'").arg(t->object, t->clip));
        return true;
    }
    case TargetKind::Node: {
        Ogre::SceneNode* n = nodeByName(t->object);
        if (!n) return fail(QStringLiteral("no scene node named '%1'").arg(t->object));
        if (t->clip.isEmpty()) t->clip = QString::fromLatin1(kDefaultNodeClip);
        return true;
    }
    case TargetKind::Morph: {
        Ogre::Entity* e = entityByName(t->object);
        if (!e) return fail(QStringLiteral("no entity named '%1'").arg(t->object));
        if (!MorphAnimationManager::instance()->morphTargetsFor(e).contains(t->sub))
            return fail(QStringLiteral("'%1' has no morph target '%2'").arg(t->object, t->sub));
        if (t->clip.isEmpty()) t->clip = QString::fromLatin1(MorphAnimationManager::kWeightClipName);
        return true;
    }
    case TargetKind::Pose: {
        Ogre::Entity* e = entityByName(t->object);
        if (!e) return fail(QStringLiteral("no entity named '%1'").arg(t->object));
        if (!PoseLibrary::instance()->hasPose(e, t->sub))
            return fail(QStringLiteral("'%1' has no saved pose '%2'").arg(t->object, t->sub));
        return true;
    }
    case TargetKind::Light:
        if (!lightByName(t->object)) return fail(QStringLiteral("no light named '%1'").arg(t->object));
        return true;
    case TargetKind::Material:
        if (!materialByName(t->object)) return fail(QStringLiteral("no material named '%1'").arg(t->object));
        return true;
    }
    return fail(QStringLiteral("unknown target"));
}

double AnimGeneratorManager::clipLengthFor(const Target& t) const
{
    switch (t.kind) {
    case TargetKind::Bone:
        if (Ogre::Animation* a = skeletalAnim(entityByName(t.object), t.clip)) return a->getLength();
        return 0.0;
    case TargetKind::Node: {
        const QJsonObject b = m_bases.value(trackKey(t));
        if (b.contains(QStringLiteral("length"))) return b.value(QStringLiteral("length")).toDouble();
        if (Ogre::Animation* a = sceneAnim(t.clip)) return a->getLength();
        return 0.0;
    }
    case TargetKind::Morph: {
        const QJsonObject b = m_bases.value(trackKey(t));
        if (b.contains(QStringLiteral("length"))) return b.value(QStringLiteral("length")).toDouble();
        return 0.0;
    }
    default:
        return 0.0;
    }
}

// ---------------------------------------------------------------------------
// Track-backed binding
// ---------------------------------------------------------------------------

bool AnimGeneratorManager::captureBase(const QString& key, const Target& t, QJsonObject* base, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    QJsonObject b;
    b[QStringLiteral("target")] = formatTarget(t);
    switch (t.kind) {
    case TargetKind::Bone: {
        Ogre::Entity* e = entityByName(t.object);
        Ogre::Animation* anim = skeletalAnim(e, t.clip);
        if (!anim) return fail(QStringLiteral("%1: skeletal clip not found").arg(key));
        Ogre::Bone* bone = e->getSkeleton()->getBone(t.sub.toStdString());
        const bool had = anim->hasNodeTrack(bone->getHandle());
        b[QStringLiteral("kind")] = QStringLiteral("bone");
        b[QStringLiteral("hadTrack")] = had;
        b[QStringLiteral("keys")] = trsKeysJson(had ? readTrsKeys(anim->getNodeTrack(bone->getHandle())) : TrsKeys{});
        break;
    }
    case TargetKind::Node: {
        Ogre::SceneNode* node = nodeByName(t.object);
        if (!node) return fail(QStringLiteral("%1: scene node not found").arg(key));
        Ogre::Animation* anim = sceneAnim(t.clip);
        const bool existed = anim != nullptr;
        if (!existed) {
            if (!NodeAnimationManager::instance()->createClip(t.clip, 4.0))
                return fail(QStringLiteral("could not create node clip '%1'").arg(t.clip));
            NodeAnimationManager::instance()->setClipEnabled(t.clip, true);
            anim = sceneAnim(t.clip);
        }
        Ogre::NodeAnimationTrack* track = nodeTrackFor(anim, node);
        TrsKeys keys;
        if (track) keys = readTrsKeys(track);
        else {
            TRS k;
            k.p = node->getPosition();
            k.r = node->getOrientation();
            k.s = node->getScale();
            keys.emplace_back(0.0, k);
        }
        b[QStringLiteral("kind")] = QStringLiteral("node");
        b[QStringLiteral("clipExisted")] = existed;
        b[QStringLiteral("length")] = existed ? double(anim->getLength()) : 4.0;
        b[QStringLiteral("hadTrack")] = track != nullptr;
        b[QStringLiteral("keys")] = trsKeysJson(keys);
        break;
    }
    case TargetKind::Morph: {
        Ogre::Entity* e = entityByName(t.object);
        if (!e) return fail(QStringLiteral("%1: entity not found").arg(key));
        Ogre::MeshPtr mesh = e->getMesh();
        const std::string clip = t.clip.toStdString();
        const bool existed = mesh->hasAnimation(clip);
        bool stateEnabled = false;
        if (existed && e->getAllAnimationStates() && e->getAllAnimationStates()->hasAnimationState(clip))
            stateEnabled = e->getAnimationState(clip)->getEnabled();
        QJsonArray tracks;
        double length = 0.0;
        if (existed) {
            Ogre::Animation* anim = mesh->getAnimation(clip);
            length = anim->getLength();
            for (const auto& kv : anim->_getVertexTrackList()) {
                tracks.append(QJsonObject{{QStringLiteral("handle"), int(kv.first)},
                                          {QStringLiteral("hadTrack"), true},
                                          {QStringLiteral("keys"), poseKeysJson(readPoseKeys(mesh, kv.second))}});
            }
        }
        b[QStringLiteral("kind")] = QStringLiteral("morph");
        b[QStringLiteral("clipExisted")] = existed;
        b[QStringLiteral("stateEnabled")] = stateEnabled;
        b[QStringLiteral("length")] = length;
        b[QStringLiteral("tracks")] = tracks;
        break;
    }
    case TargetKind::Pose:
    case TargetKind::Light:
    case TargetKind::Material: {
        double v = 0.0;
        if (!readRuntime(t, &v)) return fail(QStringLiteral("%1: target not found").arg(key));
        b[QStringLiteral("kind")] = QStringLiteral("runtime");
        // NB not `QJsonArray{QJsonArray{0.0, v}}`: a braced single QJsonArray
        // is the COPY constructor and would store the flat pair [0, v].
        QJsonArray keys;
        keys.append(QJsonArray{0.0, v});
        b[QStringLiteral("keys")] = keys;
        break;
    }
    }
    *base = b;
    crumb("capture", key);
    return true;
}

bool AnimGeneratorManager::restoreBase(const QString& key, const QJsonObject& base)
{
    Target t;
    if (!parseTarget(base.value(QStringLiteral("target")).toString(), &t)) return false;
    const QString kind = base.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("bone")) {
        Ogre::Entity* e = entityByName(t.object);
        Ogre::Animation* anim = skeletalAnim(e, t.clip);
        if (!anim || !e->getSkeleton()->hasBone(t.sub.toStdString())) return false;
        Ogre::Bone* bone = e->getSkeleton()->getBone(t.sub.toStdString());
        const unsigned short h = bone->getHandle();
        if (!base.value(QStringLiteral("hadTrack")).toBool()) {
            if (anim->hasNodeTrack(h)) anim->destroyNodeTrack(h);
        } else {
            auto* track = anim->hasNodeTrack(h) ? anim->getNodeTrack(h) : anim->createNodeTrack(h, bone);
            writeTrsKeys(track, trsKeysFromJson(base.value(QStringLiteral("keys")).toArray()));
        }
        refreshSkeletalPlayback(e);
        return true;
    }
    if (kind == QLatin1String("node")) {
        Ogre::SceneNode* node = nodeByName(t.object);
        const TrsKeys keys = trsKeysFromJson(base.value(QStringLiteral("keys")).toArray());
        if (!base.value(QStringLiteral("clipExisted")).toBool()) {
            if (sceneAnim(t.clip)) NodeAnimationManager::instance()->deleteClip(t.clip);
        } else if (Ogre::Animation* anim = sceneAnim(t.clip)) {
            anim->setLength(float(base.value(QStringLiteral("length")).toDouble(anim->getLength())));
            Ogre::NodeAnimationTrack* track = nodeTrackFor(anim, node);
            if (!base.value(QStringLiteral("hadTrack")).toBool()) {
                if (track) anim->destroyNodeTrack(track->getHandle());
            } else if (track) {
                writeTrsKeys(track, keys);
            }
            emit NodeAnimationManager::instance()->keyframesChanged(t.clip);
        }
        // A node that had no track keeps the transform it had before.
        if (node && !base.value(QStringLiteral("hadTrack")).toBool() && !keys.empty()) {
            node->setPosition(keys.front().second.p);
            node->setOrientation(keys.front().second.r);
            node->setScale(keys.front().second.s);
        }
        return true;
    }
    if (kind == QLatin1String("morph")) {
        Ogre::Entity* e = entityByName(t.object);
        if (!e) return false;
        Ogre::MeshPtr mesh = e->getMesh();
        const std::string clip = t.clip.toStdString();
        Ogre::AnimationStateSet* states = e->getAllAnimationStates();
        if (!base.value(QStringLiteral("clipExisted")).toBool()) {
            if (states && states->hasAnimationState(clip)) states->removeAnimationState(clip);
            if (mesh->hasAnimation(clip)) mesh->removeAnimation(clip);
        } else if (mesh->hasAnimation(clip)) {
            Ogre::Animation* anim = mesh->getAnimation(clip);
            for (const QJsonValue& v : base.value(QStringLiteral("tracks")).toArray()) {
                const QJsonObject tr = v.toObject();
                const auto h = ushort(tr.value(QStringLiteral("handle")).toInt());
                if (!tr.value(QStringLiteral("hadTrack")).toBool()) {
                    if (anim->hasVertexTrack(h)) anim->destroyVertexTrack(h);
                    continue;
                }
                auto* track = anim->hasVertexTrack(h) ? anim->getVertexTrack(h)
                                                      : anim->createVertexTrack(h, Ogre::VAT_POSE);
                writePoseKeys(mesh, h, track, poseKeysFromJson(tr.value(QStringLiteral("keys")).toArray()));
            }
            anim->setLength(float(base.value(QStringLiteral("length")).toDouble()));
            e->refreshAvailableAnimationState();
            if (states && states->hasAnimationState(clip))
                e->getAnimationState(clip)->setEnabled(base.value(QStringLiteral("stateEnabled")).toBool());
        }
        if (states) states->_notifyDirty();
        if (auto* acc = AnimationControlController::instance()) acc->notifyExternalAnimationEdit();
        return true;
    }
    if (kind == QLatin1String("runtime")) {
        if (t.kind == TargetKind::Pose) {
            if (Ogre::Entity* e = entityByName(t.object)) PoseLibrary::instance()->releasePosedBones(e);
            return true;
        }
        writeRuntime(t, evalRuntimeBase(base, m_clock));
        return true;
    }
    Q_UNUSED(key);
    return false;
}

bool AnimGeneratorManager::materialise(const QString& key, QJsonObject* base,
                                       const std::vector<const Generator*>& gens)
{
    Target t;
    if (!parseTarget(base->value(QStringLiteral("target")).toString(), &t)) return false;
    const QString kind = base->value(QStringLiteral("kind")).toString();
    const std::vector<GenEval> evals = prepare(gens);

    if (kind == QLatin1String("bone")) {
        Ogre::Entity* e = entityByName(t.object);
        Ogre::Animation* anim = skeletalAnim(e, t.clip);
        if (!anim || !e->getSkeleton()->hasBone(t.sub.toStdString())) return false;
        Ogre::Bone* bone = e->getSkeleton()->getBone(t.sub.toStdString());
        const unsigned short h = bone->getHandle();
        auto* track = anim->hasNodeTrack(h) ? anim->getNodeTrack(h) : anim->createNodeTrack(h, bone);
        TrsKeys baseKeys = trsKeysFromJson(base->value(QStringLiteral("keys")).toArray());
        if (baseKeys.empty()) baseKeys.emplace_back(0.0, TRS{}); // bind pose
        writeTrsKeys(track, baseKeys);

        const double clipLen = anim->getLength();
        double ws = 0, we = 0;
        int fps = 30;
        if (window(gens, clipLen, &ws, &we, &fps)) {
            TrsKeys out;
            for (const auto& k : baseKeys)
                if (k.first < ws - kEps || k.first > we + kEps) out.push_back(k);
            for (double tm : sampleTimes(ws, we, fps)) {
                Ogre::TransformKeyFrame kf(nullptr, float(tm));
                track->getInterpolatedKeyFrame(anim->_getTimeIndex(float(tm)), &kf);
                TRS b{kf.getTranslate(), kf.getRotation(), kf.getScale()};
                out.emplace_back(tm, composeTrs(b, evals, tm, clipLen, bone));
            }
            std::sort(out.begin(), out.end(), [](const auto& a, const auto& b2) { return a.first < b2.first; });
            writeTrsKeys(track, out);
        }
        refreshSkeletalPlayback(e);
        return true;
    }

    if (kind == QLatin1String("node")) {
        Ogre::SceneNode* node = nodeByName(t.object);
        if (!node) return false;
        Ogre::Animation* anim = sceneAnim(t.clip);
        if (!anim) {
            if (!NodeAnimationManager::instance()->createClip(t.clip, 4.0)) return false;
            NodeAnimationManager::instance()->setClipEnabled(t.clip, true);
            anim = sceneAnim(t.clip);
            if (!anim) return false;
        }
        const double baseLen = base->value(QStringLiteral("length")).toDouble(anim->getLength());
        double required = baseLen;
        for (const Generator* g : gens) required = std::max(required, windowEnd(*g, baseLen));
        if (required > anim->getLength()) anim->setLength(float(required));
        const double clipLen = anim->getLength();

        TrsKeys baseKeys = trsKeysFromJson(base->value(QStringLiteral("keys")).toArray());
        if (baseKeys.empty()) {
            TRS k{node->getPosition(), node->getOrientation(), node->getScale()};
            baseKeys.emplace_back(0.0, k);
        }
        Ogre::NodeAnimationTrack* track = nodeTrackFor(anim, node);
        if (!track) {
            const TRS& k0 = baseKeys.front().second;
            NodeAnimationManager::instance()->addKeyframe(t.clip, t.object, 0.0, k0.p, k0.r, k0.s);
            track = nodeTrackFor(anim, node);
            if (!track) return false;
        }
        writeTrsKeys(track, baseKeys);
        double ws = 0, we = 0;
        int fps = 30;
        if (window(gens, clipLen, &ws, &we, &fps)) {
            TrsKeys out;
            for (const auto& k : baseKeys)
                if (k.first < ws - kEps || k.first > we + kEps) out.push_back(k);
            for (double tm : sampleTimes(ws, we, fps)) {
                Ogre::TransformKeyFrame kf(nullptr, float(tm));
                track->getInterpolatedKeyFrame(anim->_getTimeIndex(float(tm)), &kf);
                TRS b{kf.getTranslate(), kf.getRotation(), kf.getScale()};
                out.emplace_back(tm, composeTrs(b, evals, tm, clipLen, nullptr));
            }
            std::sort(out.begin(), out.end(), [](const auto& a, const auto& b2) { return a.first < b2.first; });
            writeTrsKeys(track, out);
        }
        emit NodeAnimationManager::instance()->keyframesChanged(t.clip);
        return true;
    }

    if (kind == QLatin1String("morph")) {
        Ogre::Entity* e = entityByName(t.object);
        if (!e) return false;
        Ogre::MeshPtr mesh = e->getMesh();
        const std::string clip = t.clip.toStdString();
        Ogre::Animation* anim = mesh->hasAnimation(clip) ? mesh->getAnimation(clip)
                                                         : mesh->createAnimation(clip, 0.0f);
        // Driven pose names and the submesh tracks they live on.
        QSet<QString> driven;
        for (const Generator* g : gens) driven.insert(g->target.sub);
        QSet<int> drivenHandles;
        for (const auto* p : mesh->getPoseList())
            if (p && driven.contains(QString::fromStdString(p->getName()))) drivenHandles.insert(p->getTarget());

        // A generator on a target whose submesh track the base never saw
        // (a later generator on another submesh): add it to the base now.
        QJsonArray tracks = base->value(QStringLiteral("tracks")).toArray();
        QSet<int> known;
        for (const QJsonValue& v : tracks) known.insert(v.toObject().value(QStringLiteral("handle")).toInt());
        for (int h : drivenHandles) {
            if (known.contains(h)) continue;
            const bool had = anim->hasVertexTrack(ushort(h));
            tracks.append(QJsonObject{{QStringLiteral("handle"), h},
                                      {QStringLiteral("hadTrack"), had},
                                      {QStringLiteral("keys"), poseKeysJson(had ? readPoseKeys(mesh, anim->getVertexTrack(ushort(h)))
                                                                                : std::vector<PoseKey>{})}});
        }
        (*base)[QStringLiteral("tracks")] = tracks;

        const double baseLen = base->value(QStringLiteral("length")).toDouble();
        double required = baseLen;
        for (const Generator* g : gens) required = std::max(required, windowEnd(*g, baseLen));
        if (required > anim->getLength()) anim->setLength(float(required));
        const double clipLen = anim->getLength();
        double ws = 0, we = 0;
        int fps = 30;
        const bool haveWindow = window(gens, clipLen, &ws, &we, &fps);

        for (const QJsonValue& v : tracks) {
            const QJsonObject tr = v.toObject();
            const auto h = ushort(tr.value(QStringLiteral("handle")).toInt());
            const std::vector<PoseKey> baseKeys = poseKeysFromJson(tr.value(QStringLiteral("keys")).toArray());
            auto* track = anim->hasVertexTrack(h) ? anim->getVertexTrack(h) : anim->createVertexTrack(h, Ogre::VAT_POSE);
            if (!drivenHandles.contains(h) || !haveWindow) {
                writePoseKeys(mesh, h, track, baseKeys);
                continue;
            }
            // Every pose of this submesh that the base or a generator mentions:
            // a keyframe must carry ALL of them, since Ogre interpolates a
            // pose missing from a keyframe toward 0 (the #1019 lipsync lesson).
            QStringList names;
            for (const auto& k : baseKeys) for (const auto& r : k.refs) if (!names.contains(r.first)) names << r.first;
            for (const auto* p : mesh->getPoseList()) {
                if (!p || p->getTarget() != h) continue;
                const QString n = QString::fromStdString(p->getName());
                if (driven.contains(n) && !names.contains(n)) names << n;
            }
            std::vector<PoseKey> out;
            for (const auto& k : baseKeys)
                if (k.t < ws - kEps || k.t > we + kEps) out.push_back(k);
            for (double tm : sampleTimes(ws, we, fps)) {
                PoseKey k;
                k.t = tm;
                for (const QString& n : names) {
                    double w = evalPose(baseKeys, n, tm);
                    for (const auto& e2 : evals)
                        if (e2.g->target.sub == n) w += evaluateScalar(*e2.g, tm, clipLen);
                    k.refs.emplace_back(n, std::clamp(w, 0.0, 1.0));
                }
                out.push_back(std::move(k));
            }
            std::sort(out.begin(), out.end(), [](const PoseKey& a, const PoseKey& b2) { return a.t < b2.t; });
            writePoseKeys(mesh, h, track, out);
        }
        e->refreshAvailableAnimationState();
        if (Ogre::AnimationStateSet* states = e->getAllAnimationStates()) {
            if (states->hasAnimationState(clip)) {
                Ogre::AnimationState* st = e->getAnimationState(clip);
                st->setEnabled(true);
                st->setLoop(true);
                st->setTimePosition(st->getTimePosition());
            }
            states->_notifyDirty();
        }
        if (auto* acc = AnimationControlController::instance()) acc->notifyExternalAnimationEdit();
        return true;
    }
    Q_UNUSED(key);
    return false;
}

QJsonObject AnimGeneratorManager::bakedBase(const QString& key, const QJsonObject& baseIn, const Generator& g)
{
    QJsonObject base = baseIn;
    const QString kind = base.value(QStringLiteral("kind")).toString();
    Target t;
    parseTarget(base.value(QStringLiteral("target")).toString(), &t);

    if (kind == QLatin1String("runtime")) {
        // Keys outside the window survive; inside, dense samples of base + g.
        const auto keys = runtimeKeys(base);
        const double we = windowEnd(g, 0.0), ws = std::max(0.0, g.startTime);
        std::vector<std::pair<double, double>> out;
        for (const auto& k : keys) if (k.first < ws - kEps || k.first > we + kEps) out.push_back(k);
        for (double tm : sampleTimes(ws, we, g.sampleFps))
            out.emplace_back(tm, evalRuntimeBase(base, tm) + evaluateScalar(g, tm, 0.0));
        std::sort(out.begin(), out.end());
        QJsonArray arr;
        for (const auto& k : out) arr.append(QJsonArray{k.first, k.second});
        base[QStringLiteral("keys")] = arr;
        return base;
    }

    // Track-backed: materialise base + g alone, then read the track back. The
    // result is permanent keyframes, so mark it as if it had always been
    // there — removing the baked generator later must not delete it.
    materialise(key, &base, {&g});
    if (kind == QLatin1String("bone")) {
        Ogre::Entity* e = entityByName(t.object);
        if (Ogre::Animation* anim = skeletalAnim(e, t.clip)) {
            const unsigned short h = e->getSkeleton()->getBone(t.sub.toStdString())->getHandle();
            base[QStringLiteral("hadTrack")] = true;
            base[QStringLiteral("keys")] = trsKeysJson(readTrsKeys(anim->getNodeTrack(h)));
        }
    } else if (kind == QLatin1String("node")) {
        if (Ogre::Animation* anim = sceneAnim(t.clip)) {
            base[QStringLiteral("clipExisted")] = true;
            base[QStringLiteral("hadTrack")] = true;
            base[QStringLiteral("length")] = double(anim->getLength());
            base[QStringLiteral("keys")] = trsKeysJson(readTrsKeys(nodeTrackFor(anim, nodeByName(t.object))));
        }
    } else if (kind == QLatin1String("morph")) {
        Ogre::Entity* e = entityByName(t.object);
        if (e && e->getMesh()->hasAnimation(t.clip.toStdString())) {
            Ogre::MeshPtr mesh = e->getMesh();
            Ogre::Animation* anim = mesh->getAnimation(t.clip.toStdString());
            QJsonArray tracks;
            for (const auto& kv : anim->_getVertexTrackList())
                tracks.append(QJsonObject{{QStringLiteral("handle"), int(kv.first)},
                                          {QStringLiteral("hadTrack"), true},
                                          {QStringLiteral("keys"), poseKeysJson(readPoseKeys(mesh, kv.second))}});
            base[QStringLiteral("clipExisted")] = true;
            base[QStringLiteral("stateEnabled")] = true;
            base[QStringLiteral("length")] = double(anim->getLength());
            base[QStringLiteral("tracks")] = tracks;
        }
    }
    return base;
}

// ---------------------------------------------------------------------------
// Runtime binding
// ---------------------------------------------------------------------------

bool AnimGeneratorManager::readRuntime(const Target& t, double* v) const
{
    const int a = axisOf(t.channel);
    switch (t.kind) {
    case TargetKind::Pose: {
        Ogre::Entity* e = entityByName(t.object);
        if (!e || !PoseLibrary::instance()->hasPose(e, t.sub)) return false;
        *v = 0.0;
        return true;
    }
    case TargetKind::Light: {
        Ogre::Light* l = lightByName(t.object);
        if (!l) return false;
        if (t.channel == QLatin1String("intensity")) { *v = l->getPowerScale(); return true; }
        const Ogre::ColourValue c = t.channel.startsWith(QLatin1String("diffuse")) ? l->getDiffuseColour()
                                                                                    : l->getSpecularColour();
        *v = a >= 0 && a < 4 ? c[size_t(a)] : 0.0;
        return true;
    }
    case TargetKind::Material: {
        Ogre::MaterialPtr m = materialByName(t.object);
        if (!m || !m->getNumTechniques() || !m->getTechnique(0)->getNumPasses()) return false;
        Ogre::Pass* p = m->getTechnique(0)->getPass(0);
        if (t.channel == QLatin1String("shininess")) { *v = p->getShininess(); return true; }
        Ogre::ColourValue c;
        if (t.channel.startsWith(QLatin1String("diffuse"))) c = p->getDiffuse();
        else if (t.channel.startsWith(QLatin1String("ambient"))) c = p->getAmbient();
        else if (t.channel.startsWith(QLatin1String("specular"))) c = p->getSpecular();
        else c = p->getSelfIllumination();
        *v = a >= 0 && a < 4 ? c[size_t(a)] : 0.0;
        return true;
    }
    default:
        return false;
    }
}

void AnimGeneratorManager::writeRuntime(const Target& t, double value)
{
    const int a = axisOf(t.channel);
    const float v = float(value);
    switch (t.kind) {
    case TargetKind::Pose:
        if (Ogre::Entity* e = entityByName(t.object))
            PoseLibrary::instance()->applyPoseWeighted(e, t.sub, std::clamp(v, 0.0f, 1.0f));
        return;
    case TargetKind::Light: {
        Ogre::Light* l = lightByName(t.object);
        if (!l) return;
        if (t.channel == QLatin1String("intensity")) { l->setPowerScale(std::max(0.0f, v)); return; }
        if (a < 0 || a > 2) return;
        if (t.channel.startsWith(QLatin1String("diffuse"))) {
            Ogre::ColourValue c = l->getDiffuseColour(); c[size_t(a)] = v; l->setDiffuseColour(c);
        } else {
            Ogre::ColourValue c = l->getSpecularColour(); c[size_t(a)] = v; l->setSpecularColour(c);
        }
        return;
    }
    case TargetKind::Material: {
        Ogre::MaterialPtr m = materialByName(t.object);
        if (!m) return;
        // Every technique's first pass: the RTSS-generated technique is a
        // copy of the source pass, so writing only technique 0 would leave
        // the shaded one unchanged.
        for (unsigned short ti = 0; ti < m->getNumTechniques(); ++ti) {
            Ogre::Technique* tech = m->getTechnique(ti);
            if (!tech->getNumPasses()) continue;
            Ogre::Pass* p = tech->getPass(0);
            if (t.channel == QLatin1String("shininess")) { p->setShininess(std::max(0.0f, v)); continue; }
            if (a < 0 || a > 3) continue;
            auto setComp = [&](Ogre::ColourValue c) { c[size_t(a)] = v; return c; };
            if (t.channel.startsWith(QLatin1String("diffuse"))) p->setDiffuse(setComp(p->getDiffuse()));
            else if (t.channel.startsWith(QLatin1String("ambient"))) p->setAmbient(setComp(p->getAmbient()));
            else if (t.channel.startsWith(QLatin1String("specular"))) p->setSpecular(setComp(p->getSpecular()));
            else p->setSelfIllumination(setComp(p->getSelfIllumination()));
        }
        return;
    }
    default:
        return;
    }
}

double AnimGeneratorManager::evalRuntimeBase(const QJsonObject& base, double t)
{
    const auto keys = runtimeKeys(base);
    if (keys.empty()) return 0.0;
    auto kt = [&](int i) { return keys[size_t(i)].first; };
    auto kv = [&](int i) { return keys[size_t(i)].second; };
    if (t <= kt(0)) return kv(0);
    const int n = int(keys.size());
    if (t >= kt(n - 1)) return kv(n - 1);
    for (int i = 0; i + 1 < n; ++i) {
        if (t >= kt(i) && t <= kt(i + 1)) {
            const double span = kt(i + 1) - kt(i);
            const double f = span > 1e-9 ? (t - kt(i)) / span : 0.0;
            return kv(i) + (kv(i + 1) - kv(i)) * f;
        }
    }
    return kv(n - 1);
}

double AnimGeneratorManager::runtimeSpan(const QString& key) const
{
    double span = 0.0;
    for (const auto& g : m_gens)
        if (trackKey(g.target) == key) span = std::max(span, windowEnd(g, 0.0));
    const auto keys = runtimeKeys(m_bases.value(key));
    if (!keys.empty()) span = std::max(span, keys.back().first);
    return span;
}

double AnimGeneratorManager::runtimeValue(const Target& target, double t) const
{
    const QString key = trackKey(target);
    double v = 0.0;
    if (m_bases.contains(key)) v = evalRuntimeBase(m_bases.value(key), t);
    else readRuntime(target, &v);
    for (const Generator* g : activeFor(key)) v += evaluateScalar(*g, t, 0.0);
    return v;
}

void AnimGeneratorManager::tick(double dt, bool playing)
{
    if (playing) {
        m_clock += dt;
    } else if (auto* acc = AnimationControlController::instance()) {
        const int ms = acc->sliderValue();
        if (ms != m_lastSliderMs) {
            m_lastSliderMs = ms;
            m_clock = ms / 1000.0;
        }
    }
    if (m_bases.isEmpty()) return;
    for (auto it = m_bases.cbegin(); it != m_bases.cend(); ++it) {
        if (it.value().value(QStringLiteral("kind")).toString() != QLatin1String("runtime")) continue;
        const bool curve = runtimeKeys(it.value()).size() > 1;
        const auto active = activeFor(it.key());
        if (active.empty() && !curve) continue; // muted: leave the property to the user
        Target t;
        if (!parseTarget(it.value().value(QStringLiteral("target")).toString(), &t)) continue;
        const double span = runtimeSpan(it.key());
        const double tm = span > 1e-6 ? std::fmod(m_clock, span) : m_clock;
        writeRuntime(t, runtimeValue(t, tm));
    }
    if (pathEditActive()) refreshPathOverlay();
}

// ---------------------------------------------------------------------------
// Public mutations
// ---------------------------------------------------------------------------

AnimGeneratorManager::Result AnimGeneratorManager::add(Generator g, bool undoable)
{
    ensureSceneHook();
    Result r;
    QString err;
    if (!resolveTarget(&g.target, &err) || !validate(g.type, g.target, &err)) {
        r.error = err;
        setStatus(err, false);
        crumb("error", err);
        return r;
    }
    if (g.id.isEmpty() || find(g.id)) g.id = uniqueId(m_gens);
    if (g.type == Type::FollowPath && g.pathPoints.empty()) {
        // Seed a two-point path from where the object is now so the first
        // add moves it visibly; the user edits from there.
        Ogre::Vector3 start = Ogre::Vector3::ZERO;
        float step = 1.0f;
        if (g.target.kind == TargetKind::Node) {
            if (Ogre::SceneNode* n = nodeByName(g.target.object)) start = n->getPosition();
        } else if (Ogre::Entity* e = entityByName(g.target.object)) {
            Ogre::Bone* b = e->getSkeleton()->getBone(g.target.sub.toStdString());
            start = b->getInitialPosition();
            step = std::max(0.05f, e->getBoundingRadius() * 0.25f);
        }
        g.pathPoints = {start, start + Ogre::Vector3(step, 0, 0), start + Ogre::Vector3(step, 0, step)};
    }
    Doc next = current();
    next.gens.push_back(g);
    r = commit(next, QStringLiteral("Add %1 generator").arg(AnimGen::typeLabel(g.type)), g.id, undoable);
    if (r.ok) crumb("add", typeId(g.type) + QStringLiteral(" → ") + formatTarget(g.target));
    return r;
}

AnimGeneratorManager::Result AnimGeneratorManager::remove(const QString& id, bool undoable)
{
    Result r;
    r.id = id;
    if (!find(id)) { r.error = QStringLiteral("no generator '%1'").arg(id); setStatus(r.error, false); return r; }
    Doc next = current();
    next.gens.erase(std::remove_if(next.gens.begin(), next.gens.end(), [&](const Generator& g) { return g.id == id; }),
                    next.gens.end());
    r = commit(next, QStringLiteral("Remove generator"), id, undoable);
    if (r.ok) crumb("remove", id);
    return r;
}

AnimGeneratorManager::Result AnimGeneratorManager::setParams(const QString& id,
                                                            const QList<QPair<QString, QString>>& params,
                                                            bool undoable)
{
    return update(id, params, nullptr, undoable);
}

AnimGeneratorManager::Result AnimGeneratorManager::update(const QString& id,
                                                         const QList<QPair<QString, QString>>& params,
                                                         const bool* enabled, bool undoable)
{
    Result r;
    r.id = id;
    Doc next = current();
    Generator* g = nullptr;
    for (auto& x : next.gens) if (x.id == id) g = &x;
    if (!g) { r.error = QStringLiteral("no generator '%1'").arg(id); setStatus(r.error, false); return r; }
    for (const auto& p : params) {
        QString err;
        if (!applyParam(g, p.first, p.second, &err)) { r.error = err; setStatus(err, false); return r; }
    }
    if (enabled) {
        if (g->baked && *enabled) {
            r.error = QStringLiteral("'%1' is baked — its motion is now keyframes; add a new generator to layer more").arg(id);
            setStatus(r.error, false);
            return r;
        }
        g->enabled = *enabled;
    }
    const QString label = !params.isEmpty() ? QStringLiteral("Edit generator")
                        : (enabled && *enabled) ? QStringLiteral("Enable generator")
                                                : QStringLiteral("Mute generator");
    r = commit(next, label, id, undoable);
    if (r.ok) {
        if (!params.isEmpty()) crumb("edit", QStringLiteral("%1 (%2 params)").arg(id).arg(params.size()));
        if (enabled) crumb("enable", QStringLiteral("%1 %2").arg(id, *enabled ? QStringLiteral("on") : QStringLiteral("off")));
    }
    return r;
}

AnimGeneratorManager::Result AnimGeneratorManager::setEnabled(const QString& id, bool enabled, bool undoable)
{
    return update(id, {}, &enabled, undoable);
}

AnimGeneratorManager::Result AnimGeneratorManager::bake(const QString& id, bool undoable)
{
    Result r;
    r.id = id;
    const Generator* g = find(id);
    if (!g) { r.error = QStringLiteral("no generator '%1'").arg(id); setStatus(r.error, false); return r; }
    if (g->baked) { r.error = QStringLiteral("'%1' is already baked").arg(id); setStatus(r.error, false); return r; }
    const QString key = trackKey(g->target);
    if (!m_bases.contains(key)) { r.error = QStringLiteral("'%1' is not bound to its target").arg(id); setStatus(r.error, false); return r; }
    Doc next = current();
    next.bases[key] = bakedBase(key, m_bases.value(key), *g);
    for (auto& x : next.gens) if (x.id == id) { x.baked = true; x.enabled = false; }
    r = commit(next, QStringLiteral("Bake generator to keyframes"), id, undoable);
    if (r.ok) crumb("bake", QStringLiteral("%1 → %2").arg(id, key));
    return r;
}

AnimGeneratorManager::Result AnimGeneratorManager::setPathPoints(const QString& id,
                                                                const std::vector<Ogre::Vector3>& pts,
                                                                bool undoable)
{
    Result r;
    r.id = id;
    Doc next = current();
    Generator* g = nullptr;
    for (auto& x : next.gens) if (x.id == id) g = &x;
    if (!g || g->type != Type::FollowPath) {
        r.error = QStringLiteral("'%1' is not a follow-path generator").arg(id);
        setStatus(r.error, false);
        return r;
    }
    g->pathPoints = pts;
    r = commit(next, QStringLiteral("Edit generator path"), id, undoable);
    if (r.ok) crumb("path", QStringLiteral("%1 (%2 points)").arg(id).arg(pts.size()));
    return r;
}

// ---------------------------------------------------------------------------
// Sidecar
// ---------------------------------------------------------------------------

QString AnimGeneratorManager::sidecarPath(const QString& assetPath)
{
    const QFileInfo fi(assetPath);
    return fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".generators.json"));
}

QJsonObject AnimGeneratorManager::sidecarMeta(const QString& assetPath)
{
    QFile f(sidecarPath(assetPath));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("meta")).toObject();
}

bool AnimGeneratorManager::writeSidecar(const QString& assetPath, const QStringList& objects,
                                        const QJsonObject& meta) const
{
    const QString path = sidecarPath(assetPath);
    Doc d;
    for (const auto& g : m_gens)
        if (objects.isEmpty() || objects.contains(g.target.object)) d.gens.push_back(g);
    for (const auto& g : d.gens) {
        const QString k = trackKey(g.target);
        if (m_bases.contains(k)) d.bases.insert(k, m_bases.value(k));
    }
    if (d.gens.empty()) {
        if (QFile::exists(path)) QFile::remove(path);
        return false;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    QJsonObject doc = makeDocument(d);
    if (!meta.isEmpty()) doc[QStringLiteral("meta")] = meta;
    f.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
    const bool ok = f.commit();
    crumb("save", QStringLiteral("%1 generators").arg(d.gens.size()));
    return ok;
}

int AnimGeneratorManager::loadSidecar(const QString& assetPath, const QHash<QString, QString>& rename,
                                     QString* error)
{
    QFile f(sidecarPath(assetPath));
    if (!f.exists()) return 0;
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = f.errorString(); return -1; }
    const QJsonObject json = QJsonDocument::fromJson(f.readAll()).object();
    Doc file;
    if (!parseDocument(json, &file, error)) return -1;

    auto renamed = [&](Target t) {
        if (rename.contains(t.object)) t.object = rename.value(t.object);
        return t;
    };
    ensureSceneHook();
    Doc next = current();
    // Every id the merged document will hold — the existing generators, the
    // ones already appended, and the file's own (not yet appended) — so a
    // renamed id can never collide with any of them.
    QSet<QString> taken;
    for (const auto& g : next.gens) taken.insert(g.id);
    for (Generator g : file.gens) {
        const QString oldKey = trackKey(g.target);
        g.target = renamed(g.target);
        if (taken.contains(g.id)) {
            QSet<QString> reserved = taken;
            for (const auto& f : file.gens) reserved.insert(f.id);
            int n = 1;
            QString id;
            do { id = QStringLiteral("gen_%1").arg(n++); } while (reserved.contains(id));
            g.id = id;
        }
        taken.insert(g.id);
        const QString newKey = trackKey(g.target);
        if (file.bases.contains(oldKey) && !next.bases.contains(newKey)) {
            QJsonObject b = file.bases.value(oldKey);
            Target bt;
            if (parseTarget(b.value(QStringLiteral("target")).toString(), &bt))
                b[QStringLiteral("target")] = formatTarget(renamed(bt));
            next.bases.insert(newKey, b);
        }
        next.gens.push_back(g);
    }
    const QJsonObject merged = makeDocument(next);
    Doc check;
    if (!parseDocument(merged, &check, error)) return -1; // nothing applied
    QString err;
    // Re-adopt; the file's tracks already hold the motion. A false return here
    // only means some target was not found (reported, generator kept unbound).
    applyDocument(merged, &err);
    if (error && !err.isEmpty()) *error = err;
    crumb("load", QStringLiteral("%1 generators").arg(file.gens.size()));
    return int(file.gens.size());
}

// ---------------------------------------------------------------------------
// QML helpers
// ---------------------------------------------------------------------------

QVariantList AnimGeneratorManager::generatorRows() const
{
    QVariantList rows;
    for (const auto& g : m_gens) {
        QVariantMap m;
        m[QStringLiteral("id")] = g.id;
        m[QStringLiteral("name")] = displayName(g);
        m[QStringLiteral("type")] = typeId(g.type);
        m[QStringLiteral("typeLabel")] = AnimGen::typeLabel(g.type);
        m[QStringLiteral("target")] = formatTarget(g.target);
        m[QStringLiteral("kind")] = kindId(g.target.kind);
        m[QStringLiteral("enabled")] = g.enabled;
        m[QStringLiteral("baked")] = g.baked;
        m[QStringLiteral("isPath")] = g.type == Type::FollowPath;
        m[QStringLiteral("bound")] = m_bases.contains(trackKey(g.target));
        rows << m;
    }
    return rows;
}

QVariantMap AnimGeneratorManager::details(const QString& id) const
{
    const Generator* g = find(id);
    if (!g) return {};
    QVariantMap m = toJson(*g).toVariantMap();
    m.remove(QStringLiteral("state"));
    QVariantList pts;
    // append(QVariant(...)), NOT `pts << QVariantList{…}`: QList::operator<<
    // with a list CONCATENATES, flattening the points into [x0,y0,z0,x1,…] —
    // every row of the panel then read undefined coordinates and showed NaN.
    for (const auto& p : g->pathPoints)
        pts.append(QVariant(QVariantList{double(p.x), double(p.y), double(p.z)}));
    m[QStringLiteral("points")] = pts;
    m[QStringLiteral("typeLabel")] = AnimGen::typeLabel(g->type);
    return m;
}

QString AnimGeneratorManager::typeLabel(const QString& id) const
{
    Type t;
    return typeFromId(id, &t) ? AnimGen::typeLabel(t) : id;
}

QStringList AnimGeneratorManager::objectsFor(const QString& kindStr) const
{
    TargetKind kind;
    if (!kindFromId(kindStr, &kind)) return {};
    QStringList out;
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return out;
    switch (kind) {
    case TargetKind::Bone:
    case TargetKind::Morph:
    case TargetKind::Pose:
        for (Ogre::Entity* e : mgr->getEntities()) {
            if (!e) continue;
            const bool ok = kind == TargetKind::Bone ? e->hasSkeleton() && e->getSkeleton()->getNumAnimations() > 0
                          : kind == TargetKind::Morph ? !MorphAnimationManager::instance()->morphTargetsFor(e).isEmpty()
                                                      : !PoseLibrary::instance()->listPoses(e).isEmpty();
            if (ok) out << QString::fromStdString(e->getName());
        }
        break;
    case TargetKind::Node:
        for (Ogre::SceneNode* n : mgr->getSceneNodes())
            if (n) out << QString::fromStdString(n->getName());
        break;
    case TargetKind::Light:
        if (LightManager* lm = LightManager::getSingletonPtr())
            for (const LightHandle& h : lm->lights()) out << h.name;
        break;
    case TargetKind::Material: {
        QSet<QString> seen;
        for (Ogre::Entity* e : mgr->getEntities()) {
            if (!e) continue;
            for (unsigned i = 0; i < e->getNumSubEntities(); ++i) {
                const QString n = QString::fromStdString(e->getSubEntity(i)->getMaterialName());
                if (!seen.contains(n)) { seen.insert(n); out << n; }
            }
        }
        break;
    }
    }
    out.removeDuplicates();
    return out;
}

QStringList AnimGeneratorManager::subsFor(const QString& kindStr, const QString& object) const
{
    TargetKind kind;
    if (!kindFromId(kindStr, &kind)) return {};
    Ogre::Entity* e = entityByName(object);
    if (!e) return {};
    QStringList out;
    if (kind == TargetKind::Bone && e->hasSkeleton()) {
        Ogre::SkeletonInstance* sk = e->getSkeleton();
        for (unsigned short i = 0; i < sk->getNumBones(); ++i)
            out << QString::fromStdString(sk->getBone(i)->getName());
    } else if (kind == TargetKind::Morph) {
        out = MorphAnimationManager::instance()->morphTargetsFor(e);
    } else if (kind == TargetKind::Pose) {
        out = PoseLibrary::instance()->listPoses(e);
    }
    return out;
}

QStringList AnimGeneratorManager::clipsFor(const QString& kindStr, const QString& object) const
{
    TargetKind kind;
    if (!kindFromId(kindStr, &kind)) return {};
    QStringList out;
    if (kind == TargetKind::Bone) {
        Ogre::Entity* e = entityByName(object);
        if (e && e->hasSkeleton())
            for (unsigned short i = 0; i < e->getSkeleton()->getNumAnimations(); ++i)
                out << QString::fromStdString(e->getSkeleton()->getAnimation(i)->getName());
    } else if (kind == TargetKind::Node) {
        out = NodeAnimationManager::instance()->listClips();
        if (!out.contains(QString::fromLatin1(kDefaultNodeClip))) out.prepend(QString::fromLatin1(kDefaultNodeClip));
    } else if (kind == TargetKind::Morph) {
        out << QString::fromLatin1(MorphAnimationManager::kWeightClipName);
        if (Ogre::Entity* e = entityByName(object)) {
            Ogre::MeshPtr mesh = e->getMesh();
            for (unsigned short i = 0; i < mesh->getNumAnimations(); ++i) {
                Ogre::Animation* a = mesh->getAnimation(i);
                const QString n = QString::fromStdString(a->getName());
                if (!a->_getVertexTrackList().empty() && !out.contains(n)) out << n;
            }
        }
    }
    return out;
}

QStringList AnimGeneratorManager::channelsFor(const QString& kindStr, const QString& typeStr) const
{
    TargetKind kind;
    Type type;
    if (!kindFromId(kindStr, &kind) || !typeFromId(typeStr, &type)) return {};
    QStringList out;
    for (const QString& ch : AnimGen::channelsFor(kind)) {
        Target t;
        t.kind = kind;
        t.channel = ch;
        if (validate(type, t)) out << ch;
    }
    return out;
}

bool AnimGeneratorManager::addFromUi(const QString& typeStr, const QString& targetStr, const QVariantMap& params)
{
    Generator g;
    QString err;
    if (!typeFromId(typeStr, &g.type)) { setStatus(QStringLiteral("unknown generator type '%1'").arg(typeStr), false); return false; }
    if (!parseTarget(targetStr, &g.target, &err)) { setStatus(err, false); return false; }
    for (auto it = params.cbegin(); it != params.cend(); ++it) {
        if (!applyParam(&g, it.key(), it.value().toString(), &err)) { setStatus(err, false); return false; }
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("Generators: add %1").arg(typeStr));
    return add(g).ok;
}

bool AnimGeneratorManager::removeFromUi(const QString& id) { return remove(id).ok; }
bool AnimGeneratorManager::setEnabledFromUi(const QString& id, bool enabled) { return setEnabled(id, enabled).ok; }
bool AnimGeneratorManager::bakeFromUi(const QString& id) { return bake(id).ok; }

bool AnimGeneratorManager::setParamFromUi(const QString& id, const QString& key, const QString& value)
{
    return setParams(id, {{key, value}}).ok;
}

// ---------------------------------------------------------------------------
// Path editing
// ---------------------------------------------------------------------------

const Generator* AnimGeneratorManager::pathGen() const
{
    const Generator* g = find(m_pathEditId);
    return (g && g->type == Type::FollowPath) ? g : nullptr;
}

bool AnimGeneratorManager::beginPathEdit(const QString& id)
{
    const Generator* g = find(id);
    if (!g || g->type != Type::FollowPath) {
        setStatus(QStringLiteral("'%1' is not a follow-path generator").arg(id), false);
        return false;
    }
    m_pathEditId = id;
    m_selectedPoint = -1;
    m_hoverPoint = -1;
    refreshPathOverlay();
    emit pathEditChanged();
    crumb("path", QStringLiteral("edit %1").arg(id));
    return true;
}

void AnimGeneratorManager::endPathEdit()
{
    if (m_pathEditId.isEmpty()) return;
    m_pathEditId.clear();
    m_selectedPoint = -1;
    m_dragActive = false;
    destroyPathOverlay();
    emit pathEditChanged();
}

bool AnimGeneratorManager::addPathPoint(const QString& id)
{
    const Generator* g = find(id);
    if (!g || g->type != Type::FollowPath) return false;
    std::vector<Ogre::Vector3> pts = g->pathPoints;
    Ogre::Vector3 next = Ogre::Vector3::ZERO;
    if (pts.size() >= 2) next = pts.back() + (pts.back() - pts[pts.size() - 2]);
    else if (pts.size() == 1) next = pts.back() + Ogre::Vector3(1, 0, 0);
    pts.push_back(next);
    const bool ok = setPathPoints(id, pts).ok;
    if (ok && id == m_pathEditId) { m_selectedPoint = int(pts.size()) - 1; emit pathEditChanged(); }
    return ok;
}

bool AnimGeneratorManager::removePathPoint(const QString& id, int index)
{
    const Generator* g = find(id);
    if (!g || g->type != Type::FollowPath || index < 0 || index >= int(g->pathPoints.size())) return false;
    std::vector<Ogre::Vector3> pts = g->pathPoints;
    pts.erase(pts.begin() + index);
    const bool ok = setPathPoints(id, pts).ok;
    if (ok && id == m_pathEditId) { m_selectedPoint = -1; emit pathEditChanged(); }
    return ok;
}

bool AnimGeneratorManager::setPathPoint(const QString& id, int index, double x, double y, double z)
{
    const Generator* g = find(id);
    if (!g || g->type != Type::FollowPath || index < 0 || index >= int(g->pathPoints.size())) return false;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
    std::vector<Ogre::Vector3> pts = g->pathPoints;
    pts[size_t(index)] = Ogre::Vector3(float(x), float(y), float(z));
    return setPathPoints(id, pts).ok;
}

void AnimGeneratorManager::selectPathPoint(int index)
{
    m_selectedPoint = index;
    refreshPathOverlay();
    emit pathEditChanged();
}

bool AnimGeneratorManager::pathSpace(const Target& t, Ogre::Vector3* pos, Ogre::Quaternion* rot,
                                     Ogre::Vector3* scale) const
{
    if (t.kind == TargetKind::Node) {
        Ogre::SceneNode* n = nodeByName(t.object);
        if (!n) return false;
        Ogre::Node* parent = n->getParent();
        if (!parent) { *pos = Ogre::Vector3::ZERO; *rot = Ogre::Quaternion::IDENTITY; *scale = Ogre::Vector3::UNIT_SCALE; return true; }
        *pos = parent->_getDerivedPosition();
        *rot = parent->_getDerivedOrientation();
        *scale = parent->_getDerivedScale();
        return true;
    }
    if (t.kind == TargetKind::Bone) {
        Ogre::Entity* e = entityByName(t.object);
        if (!e || !e->hasSkeleton() || !e->getParentSceneNode()) return false;
        Ogre::SceneNode* en = e->getParentSceneNode();
        Ogre::Bone* b = e->getSkeleton()->getBone(t.sub.toStdString());
        Ogre::Node* pb = b ? b->getParent() : nullptr;
        const Ogre::Vector3 bp = pb ? pb->_getDerivedPosition() : Ogre::Vector3::ZERO;
        const Ogre::Quaternion bq = pb ? pb->_getDerivedOrientation() : Ogre::Quaternion::IDENTITY;
        const Ogre::Vector3 bs = pb ? pb->_getDerivedScale() : Ogre::Vector3::UNIT_SCALE;
        *rot = en->_getDerivedOrientation() * bq;
        *scale = en->_getDerivedScale() * bs;
        *pos = en->_getDerivedOrientation() * (en->_getDerivedScale() * bp) + en->_getDerivedPosition();
        return true;
    }
    return false;
}

void AnimGeneratorManager::refreshPathOverlay()
{
    const Generator* g = pathGen();
    Ogre::SceneManager* scene = sceneMgr();
    if (!g || !scene) { destroyPathOverlay(); return; }
    Ogre::Vector3 sp, ss;
    Ogre::Quaternion sq;
    if (!pathSpace(g->target, &sp, &sq, &ss)) { destroyPathOverlay(); return; }
    ensurePathMaterial(kPathLineMat, 0.0f);
    ensurePathMaterial(kPathPointMat, 11.0f);
    if (!m_overlayNode || !scene->hasSceneNode(kOverlayNodeName)) {
        m_overlayNode = scene->getRootSceneNode()->createChildSceneNode(kOverlayNodeName);
        m_overlayObj = nullptr;
    }
    if (!m_overlayObj || !scene->hasManualObject(kOverlayObjName)) {
        m_overlayObj = scene->createManualObject(kOverlayObjName);
        m_overlayObj->setDynamic(true);
        m_overlayObj->setRenderQueueGroup(Ogre::RENDER_QUEUE_OVERLAY);
        m_overlayObj->setQueryFlags(0);
        m_overlayNode->attachObject(m_overlayObj);
    }
    m_overlayNode->setPosition(sp);
    m_overlayNode->setOrientation(sq);
    m_overlayNode->setScale(ss);

    m_overlayObj->clear();
    const PathSampler path(g->pathPoints, g->pathClosed);
    const auto line = path.polyline(24);
    if (line.size() >= 2) {
        m_overlayObj->begin(kPathLineMat, Ogre::RenderOperation::OT_LINE_STRIP);
        const Ogre::ColourValue c(1.0f, 0.78f, 0.2f, 0.9f);
        for (const auto& p : line) { m_overlayObj->position(p); m_overlayObj->colour(c); }
        m_overlayObj->end();
    }
    if (!g->pathPoints.empty()) {
        m_overlayObj->begin(kPathPointMat, Ogre::RenderOperation::OT_POINT_LIST);
        for (int i = 0; i < int(g->pathPoints.size()); ++i) {
            const Ogre::ColourValue c = i == m_selectedPoint ? Ogre::ColourValue(1.0f, 0.45f, 0.1f, 1.0f)
                                      : i == m_hoverPoint    ? Ogre::ColourValue(1, 1, 1, 1)
                                                             : Ogre::ColourValue(0.3f, 0.6f, 1.0f, 1.0f);
            m_overlayObj->position(g->pathPoints[size_t(i)]);
            m_overlayObj->colour(c);
        }
        m_overlayObj->end();
    }
}

void AnimGeneratorManager::destroyPathOverlay()
{
    Ogre::SceneManager* scene = sceneMgr();
    if (scene) {
        if (m_overlayObj && scene->hasManualObject(kOverlayObjName)) {
            if (m_overlayObj->getParentSceneNode()) m_overlayObj->getParentSceneNode()->detachObject(m_overlayObj);
            scene->destroyManualObject(m_overlayObj);
        }
        if (m_overlayNode && scene->hasSceneNode(kOverlayNodeName)) scene->destroySceneNode(m_overlayNode);
    }
    m_overlayObj = nullptr;
    m_overlayNode = nullptr;
}

int AnimGeneratorManager::hitTestPathPoint(OgreWidget* widget, const QPoint& screenPos) const
{
    const Generator* g = pathGen();
    if (!g || !widget) return -1;
    Ogre::Ray ray;
    Ogre::Camera* cam = nullptr;
    if (!screenRay(widget, screenPos, &ray, &cam)) return -1;
    Ogre::Vector3 sp, ss;
    Ogre::Quaternion sq;
    if (!pathSpace(g->target, &sp, &sq, &ss)) return -1;
    int best = -1;
    float bestD2 = kPickRadiusPx * kPickRadiusPx;
    for (int i = 0; i < int(g->pathPoints.size()); ++i) {
        const Ogre::Vector3 w = sq * (ss * g->pathPoints[size_t(i)]) + sp;
        float sx = 0, sy = 0;
        if (!projectToScreen(cam, widget, w, &sx, &sy)) continue;
        const float dx = sx - screenPos.x(), dy = sy - screenPos.y();
        const float d2 = dx * dx + dy * dy;
        if (d2 <= bestD2) { bestD2 = d2; best = i; }
    }
    return best;
}

bool AnimGeneratorManager::beginDrag(OgreWidget* widget, const QPoint& screenPos)
{
    const Generator* g = pathGen();
    if (!g) return false;
    const int hit = hitTestPathPoint(widget, screenPos);
    if (hit < 0) return false;
    Ogre::Ray ray;
    Ogre::Camera* cam = nullptr;
    Ogre::Vector3 sp, ss;
    Ogre::Quaternion sq;
    if (!screenRay(widget, screenPos, &ray, &cam) || !pathSpace(g->target, &sp, &sq, &ss)) return false;
    const Ogre::Vector3 w = sq * (ss * g->pathPoints[size_t(hit)]) + sp;
    m_dragPlaneNormal = cam->getDerivedDirection();
    const auto hp = ray.intersects(Ogre::Plane(m_dragPlaneNormal, w));
    if (!hp.first) return false;
    m_dragAnchorWorld = ray.getPoint(hp.second);
    m_dragStartPoint = g->pathPoints[size_t(hit)];
    m_dragBefore = document();
    m_selectedPoint = hit;
    m_dragActive = true;
    m_dragMoved = false;
    emit pathEditChanged();
    refreshPathOverlay();
    return true;
}

void AnimGeneratorManager::updateDrag(OgreWidget* widget, const QPoint& screenPos)
{
    if (!m_dragActive) return;
    const Generator* g = pathGen();
    if (!g || m_selectedPoint < 0 || m_selectedPoint >= int(g->pathPoints.size())) return;
    Ogre::Ray ray;
    Ogre::Vector3 sp, ss;
    Ogre::Quaternion sq;
    if (!screenRay(widget, screenPos, &ray, nullptr) || !pathSpace(g->target, &sp, &sq, &ss)) return;
    const auto hp = ray.intersects(Ogre::Plane(m_dragPlaneNormal, m_dragAnchorWorld));
    if (!hp.first) return;
    const Ogre::Vector3 worldDelta = ray.getPoint(hp.second) - m_dragAnchorWorld;
    // World → path-space delta (inverse rotation, then inverse scale).
    Ogre::Vector3 local = sq.Inverse() * worldDelta;
    local = Ogre::Vector3(ss.x != 0 ? local.x / ss.x : 0, ss.y != 0 ? local.y / ss.y : 0, ss.z != 0 ? local.z / ss.z : 0);
    Doc next = current();
    for (auto& x : next.gens)
        if (x.id == m_pathEditId) x.pathPoints[size_t(m_selectedPoint)] = m_dragStartPoint + local;
    applyDocument(makeDocument(next)); // live, no undo step until release
    m_dragMoved = true;
}

void AnimGeneratorManager::endDrag()
{
    if (!m_dragActive) return;
    m_dragActive = false;
    if (m_dragMoved) {
        if (UndoManager* um = UndoManager::getSingleton())
            um->push(new AnimGeneratorDocCommand(QStringLiteral("Move path point"), m_dragBefore, document()));
        crumb("path", QStringLiteral("drag point %1").arg(m_selectedPoint));
    }
    m_dragBefore = QJsonObject();
    refreshPathOverlay();
}

void AnimGeneratorManager::updateHover(OgreWidget* widget, const QPoint& screenPos)
{
    if (!pathEditActive() || m_dragActive) return;
    const int hit = hitTestPathPoint(widget, screenPos);
    if (hit == m_hoverPoint) return;
    m_hoverPoint = hit;
    refreshPathOverlay();
}
