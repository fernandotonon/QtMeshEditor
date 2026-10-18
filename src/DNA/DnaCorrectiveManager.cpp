#include "DnaCorrectiveManager.h"

#include "DnaRigLogic.h"

#include "../Manager.h"
#include "../SentryReporter.h"

#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreHardwareBufferManager.h>
#include <OgreRoot.h>
#include <OgreMesh.h>
#include <OgreSkeletonInstance.h>
#include <OgreSubEntity.h>
#include <OgreSubMesh.h>

#include <QQmlEngine>

#include <chrono>

DnaCorrectiveManager* DnaCorrectiveManager::m_pSingleton = nullptr;

struct DnaCorrectiveManager::Character {
    std::shared_ptr<Dna::RigEvaluator> evaluator;
    Stats stats;
    QHash<QString, std::array<double, 4>> overrides;   // joint -> delta wxyz
    bool skipping = false;   ///< we set setSkipAnimationStateUpdate on the entity
};

namespace {

Ogre::SceneManager* sceneMgr()
{
    Manager* m = Manager::getSingletonPtr();
    return m ? m->getSceneMgr() : nullptr;
}

Ogre::Entity* entityByName(const QString& name)
{
    Ogre::SceneManager* s = sceneMgr();
    const std::string n = name.toStdString();
    return (s && s->hasEntity(n)) ? s->getEntity(n) : nullptr;
}

std::array<double, 4> toArray(const Ogre::Quaternion& q) { return {q.w, q.x, q.y, q.z}; }

}  // namespace

DnaCorrectiveManager* DnaCorrectiveManager::instance()
{
    if (!m_pSingleton) m_pSingleton = new DnaCorrectiveManager();
    return m_pSingleton;
}

DnaCorrectiveManager* DnaCorrectiveManager::qmlInstance(QQmlEngine*, QJSEngine*)
{
    DnaCorrectiveManager* m = instance();
    QQmlEngine::setObjectOwnership(m, QQmlEngine::CppOwnership);
    return m;
}

void DnaCorrectiveManager::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

DnaCorrectiveManager::DnaCorrectiveManager() = default;

DnaCorrectiveManager::~DnaCorrectiveManager()
{
    detachListener();
}

void DnaCorrectiveManager::ensureListener()
{
    Ogre::SceneManager* s = sceneMgr();
    if (!s || s == m_listened) return;
    detachListener();
    s->addListener(this);
    m_listened = s;
}

void DnaCorrectiveManager::detachListener()
{
    if (!m_listened) return;
    if (m_listened == sceneMgr()) m_listened->removeListener(this);
    m_listened = nullptr;
}

std::array<double, 4> DnaCorrectiveManager::localDelta(const std::array<double, 4>& initial,
                                                       const std::array<double, 4>& current)
{
    const Ogre::Quaternion qi{static_cast<float>(initial[0]), static_cast<float>(initial[1]),
                              static_cast<float>(initial[2]), static_cast<float>(initial[3])};
    const Ogre::Quaternion qc{static_cast<float>(current[0]), static_cast<float>(current[1]),
                              static_cast<float>(current[2]), static_cast<float>(current[3])};
    return toArray(qi.Inverse() * qc);
}

bool DnaCorrectiveManager::attach(const QString& entityName, std::shared_ptr<Dna::RigEvaluator> evaluator)
{
    if (!evaluator || evaluator->driverJoints().isEmpty() || evaluator->drivenJoints().empty())
        return false;
    auto c = std::make_shared<Character>();
    c->evaluator = std::move(evaluator);
    m_chars.insert(entityName, c);
    ensureListener();
    SentryReporter::addBreadcrumb(
        QStringLiteral("scene.anim.dna_correctives"),
        QStringLiteral("attach: %1 drivers, %2 driven joints, %3 RBF solvers")
            .arg(c->evaluator->driverJoints().size()).arg(c->evaluator->drivenJoints().size())
            .arg(c->evaluator->rbfSolverCount()));
    emit changed();
    return true;
}

void DnaCorrectiveManager::detach(const QString& entityName)
{
    if (m_chars.remove(entityName)) {
        restorePlain(entityName);
        emit changed();
    }
}

void DnaCorrectiveManager::clear()
{
    if (m_chars.isEmpty()) return;
    m_chars.clear();
    emit changed();
}

bool DnaCorrectiveManager::isAttached(const QString& entityName) const
{
    return m_chars.contains(entityName);
}

void DnaCorrectiveManager::setEnabled(bool on)
{
    if (m_enabled == on) return;
    m_enabled = on;
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.dna_correctives"),
                                  on ? QStringLiteral("enable") : QStringLiteral("disable"));
    if (!on) {
        for (auto it = m_chars.cbegin(); it != m_chars.cend(); ++it)
            restorePlain(it.key());
    } else {
        evaluateNow();
    }
    if (!on) evaluateNow();   // re-apply a held preview pose without correctives
    emit changed();
}

void DnaCorrectiveManager::restorePlain(const QString& name)
{
    Ogre::Entity* e = entityByName(name);
    auto it = m_chars.constFind(name);
    if (it != m_chars.constEnd()) it.value()->skipping = false;
    if (!e || !e->hasSkeleton()) return;
    // Hand sampling back to Ogre and re-pose from the clips: with nothing
    // playing, no later frame would overwrite the last corrected pose.
    e->setSkipAnimationStateUpdate(false);
    if (Ogre::AnimationStateSet* states = e->getAllAnimationStates()) {
        e->getSkeleton()->setAnimationState(*states);
        states->_notifyDirty();
    } else {
        e->getSkeleton()->reset(false);
    }
}

bool DnaCorrectiveManager::applyTo(const QString& name, Character& c, bool correctives)
{
    Ogre::Entity* e = entityByName(name);
    if (!e || !e->hasSkeleton()) return false;
    Ogre::SkeletonInstance* skel = e->getSkeleton();
    Dna::RigEvaluator& ev = *c.evaluator;

    // Pose from the clips here (resetting every bone, corrective joints
    // included, to its neutral), then stop Ogre re-sampling over our writes.
    if (Ogre::AnimationStateSet* states = e->getAllAnimationStates()) skel->setAnimationState(*states);
    else skel->reset(false);
    e->setSkipAnimationStateUpdate(true);
    c.skipping = true;

    // Preview poses: neutral * delta, applied before the drivers are read.
    for (auto o = c.overrides.cbegin(); o != c.overrides.cend(); ++o) {
        const std::string bn = o.key().toStdString();
        if (!skel->hasBone(bn)) continue;
        Ogre::Bone* b = skel->getBone(bn);
        const auto& q = o.value();
        b->setOrientation(b->getInitialOrientation() * Ogre::Quaternion(float(q[0]), float(q[1]), float(q[2]), float(q[3])));
    }

    if (!correctives) {
        // Plain skinning with the preview pose only: corrective bones stay at
        // neutral (the reset above), nothing is evaluated.
        for (Ogre::Bone* b : skel->getBones())
            if (b && !b->getParent()) b->_update(true, false);
        if (Ogre::AnimationStateSet* states = e->getAllAnimationStates()) states->_notifyDirty();
        c.stats.calcMs = c.stats.applyMs = 0.0;
        return true;
    }

    const auto t0 = std::chrono::steady_clock::now();
    ev.resetControls();
    int drivers = 0;
    for (const QString& joint : ev.driverJoints()) {
        const std::string bn = joint.toStdString();
        if (!skel->hasBone(bn)) continue;
        Ogre::Bone* b = skel->getBone(bn);
        ev.setDriverRotation(joint, localDelta(toArray(b->getInitialOrientation()), toArray(b->getOrientation())));
        ++drivers;
    }
    c.stats.calcMs = ev.calculate();

    int driven = 0, missing = 0;
    for (const Dna::JointPose& p : ev.drivenPoses()) {
        const std::string bn = ev.jointName(p.joint).toStdString();
        if (!skel->hasBone(bn)) { ++missing; continue; }
        Ogre::Bone* b = skel->getBone(bn);
        b->setPosition(Ogre::Vector3(float(p.translation[0]), float(p.translation[1]), float(p.translation[2])));
        b->setOrientation(Ogre::Quaternion(float(p.orientation[0]), float(p.orientation[1]),
                                           float(p.orientation[2]), float(p.orientation[3])));
        b->setScale(Ogre::Vector3(float(p.scale[0]), float(p.scale[1]), float(p.scale[2])));
        ++driven;
    }
    // Ogre flags only the changed node: descendants keep stale derived
    // transforms until the next graph update, so refresh every root's subtree.
    for (Ogre::Bone* b : skel->getBones())
        if (b && !b->getParent()) b->_update(true, false);
    if (Ogre::AnimationStateSet* states = e->getAllAnimationStates()) states->_notifyDirty();

    c.stats.drivers = drivers;
    c.stats.driven = driven;
    c.stats.missingDriven = missing;
    c.stats.applyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count()
                      - c.stats.calcMs;
    return true;
}

bool DnaCorrectiveManager::evaluateNow()
{
    // A deleted entity must be forgotten even while correctives are off, or the
    // panel and the MCP status would keep reporting a character that is gone.
    bool pruned = false;
    for (auto it = m_chars.begin(); it != m_chars.end();) {
        if (!entityByName(it.key())) { it = m_chars.erase(it); pruned = true; }
        else ++it;
    }
    if (pruned) emit changed();
    if (m_evaluating || m_chars.isEmpty() || (!m_enabled && !anyOverrides())) return false;
    m_evaluating = true;
    bool any = false;
    QStringList dead;
    for (auto it = m_chars.begin(); it != m_chars.end(); ++it) {
        // Disabled = plain skinning, but a held preview pose still applies.
        if (!m_enabled && it.value()->overrides.isEmpty()) continue;
        if (applyTo(it.key(), *it.value(), m_enabled)) any = true;
        else dead << it.key();       // entity destroyed (scene cleared / node deleted)
    }
    for (const QString& n : dead) m_chars.remove(n);
    m_evaluating = false;
    if (any) emit statsChanged();
    if (!dead.isEmpty()) emit changed();
    return any;
}

bool DnaCorrectiveManager::anyOverrides() const
{
    for (auto it = m_chars.cbegin(); it != m_chars.cend(); ++it)
        if (!it.value()->overrides.isEmpty()) return true;
    return false;
}

void DnaCorrectiveManager::preUpdateSceneGraph(Ogre::SceneManager* source, Ogre::Camera*)
{
    if (source != m_listened) return;
    evaluateNow();
}

bool DnaCorrectiveManager::setPoseOverride(const QString& entityName, const QString& joint,
                                           const std::array<double, 4>& delta)
{
    const auto it = m_chars.constFind(entityName);
    Ogre::Entity* e = entityByName(entityName);
    if (it == m_chars.constEnd() || !e || !e->hasSkeleton()
        || !e->getSkeleton()->hasBone(joint.toStdString()))
        return false;
    it.value()->overrides.insert(joint, delta);
    return true;
}

void DnaCorrectiveManager::clearPoseOverrides(const QString& entityName)
{
    for (auto it = m_chars.begin(); it != m_chars.end(); ++it)
        if (entityName.isEmpty() || it.key() == entityName) it.value()->overrides.clear();
    for (auto it = m_chars.cbegin(); it != m_chars.cend(); ++it)
        if (entityName.isEmpty() || it.key() == entityName) {
            if (m_enabled) { evaluateNow(); } else { restorePlain(it.key()); }
        }
}

std::vector<float> DnaCorrectiveManager::skinnedPositions(Ogre::Entity* entity)
{
    std::vector<float> out;
    if (!entity) return out;
    entity->addSoftwareAnimationRequest(false);
    // Headless callers never render a frame, so Ogre's frame counter would never
    // advance and the bone matrices would stay cached from the first read (the
    // VATBaker lesson): mark the state set dirty and bump the counter by hand.
    if (Ogre::AnimationStateSet* states = entity->getAllAnimationStates()) states->_notifyDirty();
    Ogre::FrameEvent ev{};
    Ogre::Root::getSingleton()._fireFrameRenderingQueued(ev);
    entity->_updateAnimation();
    const Ogre::MeshPtr mesh = entity->getMesh();
    for (unsigned int si = 0; si < mesh->getNumSubMeshes(); ++si) {
        Ogre::SubMesh* sm = mesh->getSubMesh(si);
        Ogre::VertexData* vd = sm->useSharedVertices ? entity->_getSkelAnimVertexData()
                                                     : entity->getSubEntity(si)->_getSkelAnimVertexData();
        if (!vd) vd = sm->useSharedVertices ? mesh->sharedVertexData : sm->vertexData;
        if (!vd) continue;
        const Ogre::VertexElement* pe = vd->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
        if (!pe) continue;
        Ogre::HardwareVertexBufferSharedPtr vb = vd->vertexBufferBinding->getBuffer(pe->getSource());
        const size_t stride = vb->getVertexSize();
        auto* base = static_cast<unsigned char*>(vb->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
        for (size_t v = 0; v < vd->vertexCount; ++v) {
            float* p = nullptr;
            pe->baseVertexPointerToElement(base + v * stride, &p);
            out.push_back(p[0]); out.push_back(p[1]); out.push_back(p[2]);
        }
        vb->unlock();
    }
    entity->removeSoftwareAnimationRequest(false);
    return out;
}

DnaCorrectiveManager::Stats DnaCorrectiveManager::statsFor(const QString& entityName) const
{
    const auto it = m_chars.constFind(entityName);
    return it == m_chars.constEnd() ? Stats() : it.value()->stats;
}

QVariantMap DnaCorrectiveManager::statsMap(const QString& entityName) const
{
    QVariantMap out;
    out.insert(QStringLiteral("enabled"), m_enabled);
    out.insert(QStringLiteral("characters"), m_chars.size());
    const QString key = !entityName.isEmpty() ? entityName
                        : (m_chars.isEmpty() ? QString() : m_chars.cbegin().key());
    const auto it = m_chars.constFind(key);
    if (it != m_chars.constEnd()) {
        const Stats& s = it.value()->stats;
        out.insert(QStringLiteral("entity"), key);
        out.insert(QStringLiteral("driverBones"), s.drivers);
        out.insert(QStringLiteral("correctiveBones"), s.driven);
        out.insert(QStringLiteral("correctiveJointsMissing"), s.missingDriven);
        out.insert(QStringLiteral("rigLogicMs"), s.calcMs);
        out.insert(QStringLiteral("applyMs"), s.applyMs);
        out.insert(QStringLiteral("rbfSolvers"), it.value()->evaluator->rbfSolverCount());
        out.insert(QStringLiteral("rbfPoses"), it.value()->evaluator->rbfPoseCount());
    }
    return out;
}

QString DnaCorrectiveManager::summary() const
{
    if (m_chars.isEmpty()) return QStringLiteral("No DNA character");
    const Stats s = m_chars.cbegin().value()->stats;
    QString t = QStringLiteral("%1 corrective joints, %2 drivers, %3 ms")
                    .arg(s.driven).arg(s.drivers).arg(s.calcMs + s.applyMs, 0, 'f', 2);
    if (s.missingDriven > 0)
        t += QStringLiteral(" · %1 not in skeleton").arg(s.missingDriven);
    return t;
}
