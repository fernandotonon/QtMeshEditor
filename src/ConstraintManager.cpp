/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "ConstraintManager.h"

#include "AnimationControlController.h"
#include "Manager.h"
#include "NodeAnimationManager.h"
#include "PoseLibrary.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/ConstraintCommands.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QQmlEngine>
#include <QSaveFile>

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreAnimationTrack.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreKeyFrame.h>
#include <OgreSceneNode.h>
#include <OgreSkeletonInstance.h>

#include <algorithm>
#include <cmath>

using namespace AnimCon;

namespace {

constexpr const char* kDefaultNodeClip = "Constraints";

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

Ogre::Bone* boneOf(Ogre::Entity* e, const QString& bone)
{
    if (!e || !e->hasSkeleton()) return nullptr;
    const std::string b = bone.toStdString();
    return e->getSkeleton()->hasBone(b) ? e->getSkeleton()->getBone(b) : nullptr;
}

Transform derivedOf(Ogre::Node* n)
{
    Transform t;
    if (!n) return t;
    t.position = n->_getDerivedPosition();
    t.rotation = n->_getDerivedOrientation();
    t.scale = n->_getDerivedScale();
    return t;
}

Transform localOf(Ogre::Node* n)
{
    Transform t;
    t.position = n->getPosition();
    t.rotation = n->getOrientation();
    t.scale = n->getScale();
    return t;
}

/// Ogre only flags the CHANGED node; its descendants keep stale derived
/// transforms until the next scene-graph update. A later constraint in the
/// same evaluation that reads a child (an IK chain below a look-at, a node
/// parented under a constrained one) must see the new values, so refresh the
/// subtree right away.
void refreshSubtree(Ogre::Node* n)
{
    n->_update(true, false);
}

void writeLocal(Ogre::Node* n, const Transform& t)
{
    n->setPosition(t.position);
    n->setOrientation(t.rotation);
    n->setScale(t.scale);
    refreshSubtree(n);
}

Transform entityWorld(Ogre::Entity* e)
{
    return e && e->getParentSceneNode() ? derivedOf(e->getParentSceneNode()) : Transform{};
}

/// World transform of the PARENT space a bone's local TRS lives in.
Transform boneParentWorld(Ogre::Entity* e, Ogre::Bone* b)
{
    Transform w = entityWorld(e);
    if (Ogre::Node* p = b->getParent()) w = compose(w, derivedOf(p));
    return w;
}

Transform nodeParentWorld(Ogre::SceneNode* n)
{
    Ogre::Node* p = n->getParent();
    return p ? derivedOf(p) : Transform{};
}

bool refWorld(const Ref& r, Transform* out)
{
    if (r.isEmpty()) return false;
    if (r.isBone()) {
        Ogre::Entity* e = entityByName(r.object);
        Ogre::Bone* b = boneOf(e, r.bone);
        if (!b) return false;
        *out = compose(entityWorld(e), derivedOf(b));
        return true;
    }
    Ogre::SceneNode* n = nodeByName(r.object);
    if (!n) return false;
    *out = derivedOf(n);
    return true;
}

bool sameTransform(const Transform& a, const Transform& b)
{
    return (a.position - b.position).squaredLength() < 1e-10f
        && std::abs(std::abs(a.rotation.Dot(b.rotation)) - 1.0f) < 1e-6f
        && (a.scale - b.scale).squaredLength() < 1e-10f;
}

/// `world` expressed in `parent`'s space: the exact inverse of compose()
/// (translate, then inverse rotation, THEN inverse scale). compose(inverse(p),
/// w) applies scale before rotation and is wrong for a non-uniformly scaled,
/// rotated parent.
Transform relativeTo(const Transform& parent, const Transform& world)
{
    auto div = [](const Ogre::Vector3& a, const Ogre::Vector3& b) {
        return Ogre::Vector3(b.x != 0 ? a.x / b.x : 0.0f, b.y != 0 ? a.y / b.y : 0.0f, b.z != 0 ? a.z / b.z : 0.0f);
    };
    Transform t;
    t.position = div(parent.rotation.Inverse() * (world.position - parent.position), parent.scale);
    t.rotation = parent.rotation.Inverse() * world.rotation;
    t.scale = div(world.scale, parent.scale);
    return t;
}

/// One non-IK constraint on a LOCAL transform whose parent space is
/// `parentWorld`. Returns the new local.
Transform applyLocal(const Constraint& c, const Transform& parentWorld, const Transform& local)
{
    Transform out = local;
    const Transform ownerWorld = compose(parentWorld, local);
    Transform target;
    const bool hasTarget = refWorld(c.target, &target);
    switch (c.type) {
    case Type::LookAt: {
        if (!hasTarget) return local;
        const Ogre::Quaternion w = aimRotation(ownerWorld.position, target.position, c.aimAxis, c.upAxis,
                                               Ogre::Vector3::UNIT_Y, ownerWorld.rotation);
        out.rotation = blend(local.rotation, parentWorld.rotation.Inverse() * w, c.influence);
        break;
    }
    case Type::CopyRotation:
        if (!hasTarget) return local;
        out.rotation = blend(local.rotation, parentWorld.rotation.Inverse() * target.rotation, c.influence);
        break;
    case Type::CopyPosition: {
        if (!hasTarget) return local;
        Ogre::Vector3 p = ownerWorld.position;
        if (c.useX) p.x = target.position.x;
        if (c.useY) p.y = target.position.y;
        if (c.useZ) p.z = target.position.z;
        Transform w;
        w.position = p;
        const Ogre::Vector3 lp = relativeTo(parentWorld, w).position;
        out.position = blend(local.position, lp, c.influence);
        break;
    }
    case Type::ParentOf: {
        if (!hasTarget || !c.hasOffset) return local;
        const Transform want = relativeTo(parentWorld, compose(target, c.offset));
        out.position = blend(local.position, want.position, c.influence);
        out.rotation = blend(local.rotation, want.rotation, c.influence);
        out.scale = blend(local.scale, want.scale, c.influence);
        break;
    }
    case Type::LimitRotation:
        out.rotation = blend(local.rotation, limitRotation(local.rotation, c), c.influence);
        break;
    case Type::IK:
        break;
    }
    out.rotation.normalise();
    return out;
}

std::vector<double> times(double len, int fps)
{
    std::vector<double> out;
    fps = std::clamp(fps, 1, 240);
    const int n = std::max(1, int(std::ceil(len * fps - 1e-6)));
    for (int i = 0; i <= n; ++i) out.push_back(i == n ? len : double(i) / fps);
    return out;
}

QJsonObject trsKey(double t, const Ogre::Vector3& p, const Ogre::Quaternion& r, const Ogre::Vector3& s)
{
    return QJsonObject{{QStringLiteral("t"), t},
                       {QStringLiteral("p"), QJsonArray{p.x, p.y, p.z}},
                       {QStringLiteral("r"), QJsonArray{r.w, r.x, r.y, r.z}},
                       {QStringLiteral("s"), QJsonArray{s.x, s.y, s.z}}};
}

QJsonArray readTrack(Ogre::NodeAnimationTrack* t)
{
    QJsonArray keys;
    if (!t) return keys;
    for (unsigned short i = 0; i < t->getNumKeyFrames(); ++i) {
        auto* k = t->getNodeKeyFrame(i);
        keys.append(trsKey(k->getTime(), k->getTranslate(), k->getRotation(), k->getScale()));
    }
    return keys;
}

void writeTrack(Ogre::NodeAnimationTrack* t, const QJsonArray& keys)
{
    t->removeAllKeyFrames();
    for (const QJsonValue& v : keys) {
        const QJsonObject o = v.toObject();
        const QJsonArray p = o.value(QStringLiteral("p")).toArray(), r = o.value(QStringLiteral("r")).toArray(),
                         s = o.value(QStringLiteral("s")).toArray();
        if (p.size() != 3 || r.size() != 4 || s.size() != 3) continue;
        auto* k = t->createNodeKeyFrame(float(o.value(QStringLiteral("t")).toDouble()));
        k->setTranslate(Ogre::Vector3(float(p[0].toDouble()), float(p[1].toDouble()), float(p[2].toDouble())));
        k->setRotation(Ogre::Quaternion(float(r[0].toDouble()), float(r[1].toDouble()), float(r[2].toDouble()),
                                        float(r[3].toDouble())));
        k->setScale(Ogre::Vector3(float(s[0].toDouble()), float(s[1].toDouble()), float(s[2].toDouble())));
    }
    t->_keyFrameDataChanged();
}

Ogre::NodeAnimationTrack* nodeTrackFor(Ogre::Animation* a, Ogre::Node* n)
{
    if (!a || !n) return nullptr;
    for (const auto& kv : a->_getNodeTrackList())
        if (kv.second && kv.second->getAssociatedNode() == n) return kv.second;
    return nullptr;
}

void refreshEntity(Ogre::Entity* e)
{
    if (!e) return;
    if (Ogre::AnimationStateSet* states = e->getAllAnimationStates()) states->_notifyDirty();
    if (auto* acc = AnimationControlController::instance()) acc->notifyExternalAnimationEdit();
}

void crumb(const char* op, const QString& msg)
{
    SentryReporter::addBreadcrumb(QStringLiteral("scene.anim.constraint.%1").arg(QLatin1String(op)), msg);
}

QString ownerKey(const Ref& r) { return formatRef(r); }

} // namespace

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

ConstraintManager* ConstraintManager::m_pSingleton = nullptr;

ConstraintManager* ConstraintManager::instance()
{
    if (!m_pSingleton) m_pSingleton = new ConstraintManager();
    return m_pSingleton;
}

ConstraintManager* ConstraintManager::qmlInstance(QQmlEngine*, QJSEngine*)
{
    ConstraintManager* m = instance();
    QQmlEngine::setObjectOwnership(m, QQmlEngine::CppOwnership);
    return m;
}

void ConstraintManager::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

ConstraintManager::ConstraintManager()
{
    ensureSceneHook();
}

ConstraintManager::~ConstraintManager()
{
    detachListener();
}

void ConstraintManager::setStatus(const QString& text, bool ok)
{
    m_status = text;
    m_lastOk = ok;
    emit statusChanged();
}

const Constraint* ConstraintManager::find(const QString& id) const
{
    for (const auto& c : m_cons) if (c.id == id) return &c;
    return nullptr;
}

int ConstraintManager::activeCount() const
{
    int n = 0;
    for (const auto& c : m_cons) if (c.enabled && c.influence > 0.0) ++n;
    return n;
}

void ConstraintManager::ensureSceneHook()
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr || m_hookedManager == mgr) return;
    m_hookedManager = mgr;
    connect(mgr, &Manager::sceneClearing, this, &ConstraintManager::discardForSceneClear);
}

void ConstraintManager::discardForSceneClear()
{
    // Every owner is about to be destroyed: drop state, restore nothing.
    detachListener();
    const bool had = !m_cons.empty();
    m_cons.clear();
    m_nodeStates.clear();
    m_skippingEntities.clear();
    if (had) {
        emit constraintsChanged();
        crumb("clear", QStringLiteral("scene replaced"));
    }
}

void ConstraintManager::ensureListener()
{
    Ogre::SceneManager* s = sceneMgr();
    if (!s || s == m_listened) return;
    detachListener();
    s->addListener(this);
    m_listened = s;
}

void ConstraintManager::detachListener()
{
    if (!m_listened) return;
    // The scene manager may already be gone (teardown): only detach from the
    // live one.
    if (m_listened == sceneMgr()) m_listened->removeListener(this);
    m_listened = nullptr;
}

void ConstraintManager::preUpdateSceneGraph(Ogre::SceneManager* source, Ogre::Camera*)
{
    if (source != m_listened) return;
    evaluate();
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------

void ConstraintManager::evaluate()
{
    if (m_evaluating) return;
    m_evaluating = true;
    m_touchedBones.clear();

    // Owners in first-appearance order, each with its stack (list order =
    // top first).
    QStringList owners;
    QHash<QString, std::vector<const Constraint*>> stacks;
    for (const auto& c : m_cons) {
        if (!c.enabled || c.influence <= 0.0) continue;
        const QString k = ownerKey(c.owner);
        if (!stacks.contains(k)) owners << k;
        stacks[k].push_back(&c);
    }

    // ---- node owners ----
    for (const QString& k : owners) {
        const auto& stack = stacks[k];
        const Ref& owner = stack.front()->owner;
        if (owner.isBone()) continue;
        Ogre::SceneNode* node = nodeByName(owner.object);
        if (!node) continue;
        NodeState& st = m_nodeStates[owner.object];
        const Transform cur = localOf(node);
        if (!(st.written && sameTransform(cur, st.lastWritten))) st.base = cur; // animated or moved by the user
        Transform local = st.base;
        const Transform parentWorld = nodeParentWorld(node);
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) // bottom → top: the top wins
            local = applyLocal(**it, parentWorld, local);
        writeLocal(node, local);
        st.lastWritten = local;
        st.written = true;
    }

    // ---- bone owners, per entity ----
    QHash<QString, QStringList> byEntity;
    for (const QString& k : owners) {
        const Ref& owner = stacks[k].front()->owner;
        if (owner.isBone()) byEntity[owner.object] << k;
    }
    for (auto it = byEntity.cbegin(); it != byEntity.cend(); ++it) {
        Ogre::Entity* e = entityByName(it.key());
        if (!e || !e->hasSkeleton()) continue;
        Ogre::SkeletonInstance* skel = e->getSkeleton();
        // Pose the skeleton from its clips HERE, then stop Ogre re-sampling
        // over our writes this frame.
        if (Ogre::AnimationStateSet* states = e->getAllAnimationStates()) skel->setAnimationState(*states);
        else skel->reset(false);
        e->setSkipAnimationStateUpdate(true);
        m_skippingEntities.insert(it.key());

        // Parents before children.
        QStringList keys = it.value();
        auto depth = [&](const QString& key) {
            Ogre::Bone* b = boneOf(e, stacks[key].front()->owner.bone);
            int d = 0;
            for (Ogre::Node* p = b ? b->getParent() : nullptr; p; p = p->getParent()) ++d;
            return d;
        };
        std::stable_sort(keys.begin(), keys.end(), [&](const QString& a, const QString& b) { return depth(a) < depth(b); });

        for (const QString& k : keys) {
            const auto& stack = stacks[k];
            Ogre::Bone* bone = boneOf(e, stack.front()->owner.bone);
            if (!bone) continue;
            for (auto cit = stack.rbegin(); cit != stack.rend(); ++cit) {
                const Constraint& c = **cit;
                if (c.type != Type::IK) {
                    const Transform out = applyLocal(c, boneParentWorld(e, bone), localOf(bone));
                    writeLocal(bone, out);
                    m_touchedBones.insert(it.key() + QLatin1Char('/') + QString::fromStdString(bone->getName()));
                    continue;
                }
                // 2-bone IK: the owner is the END bone; rotate its parent and
                // grandparent so it reaches the target.
                auto* mid = dynamic_cast<Ogre::Bone*>(bone->getParent());
                auto* root = mid ? dynamic_cast<Ogre::Bone*>(mid->getParent()) : nullptr;
                Transform target, pole;
                if (!mid || !root || !refWorld(c.target, &target)) continue;
                const bool hasPole = refWorld(c.pole, &pole);
                const Transform ew = entityWorld(e);
                const Transform wa = compose(ew, derivedOf(root)), wb = compose(ew, derivedOf(mid)),
                                wc = compose(ew, derivedOf(bone));
                const IkSolution s = solveTwoBone(wa.position, wb.position, wc.position, target.position,
                                                  pole.position, hasPole);
                if (!s.valid) continue;
                const Transform rootParent = boneParentWorld(e, root);
                const Ogre::Quaternion rootLocal = rootParent.rotation.Inverse() * (s.rootDelta * wa.rotation);
                root->setOrientation(blend(root->getOrientation(), rootLocal, c.influence));
                const Ogre::Quaternion midWorld = s.midDelta * s.rootDelta * wb.rotation;
                const Transform midParent = boneParentWorld(e, mid); // the root, already updated
                const Ogre::Quaternion midLocal = midParent.rotation.Inverse() * midWorld;
                mid->setOrientation(blend(mid->getOrientation(), midLocal, c.influence));
                refreshSubtree(root);
                m_touchedBones.insert(it.key() + QLatin1Char('/') + QString::fromStdString(root->getName()));
                m_touchedBones.insert(it.key() + QLatin1Char('/') + QString::fromStdString(mid->getName()));
            }
        }
        // Entity::_updateAnimation only re-reads the skeleton when the state
        // set is dirty — without this a paused clip would never show the
        // constrained pose.
        if (Ogre::AnimationStateSet* states = e->getAllAnimationStates()) states->_notifyDirty();
    }
    m_evaluating = false;
}

void ConstraintManager::syncOwners()
{
    QSet<QString> boneEntities, nodes;
    for (const auto& c : m_cons) {
        if (!c.enabled || c.influence <= 0.0) continue;
        if (c.owner.isBone()) boneEntities.insert(c.owner.object);
        else nodes.insert(c.owner.object);
    }
    for (const QString& name : QSet<QString>(m_skippingEntities)) {
        if (boneEntities.contains(name)) continue;
        if (Ogre::Entity* e = entityByName(name)) {
            // Hand sampling back to Ogre — unless a pose-library hold owns it.
            const PoseLibrary* lib = PoseLibrary::instance();
            e->setSkipAnimationStateUpdate(lib && lib->hasHeldBones(e));
            // Re-pose from the clips now: with no clip playing nothing would
            // ever overwrite the last constrained pose. Manual (held) bones
            // are left alone by the reset.
            if (e->hasSkeleton()) {
                if (Ogre::AnimationStateSet* states = e->getAllAnimationStates())
                    e->getSkeleton()->setAnimationState(*states);
                else
                    e->getSkeleton()->reset(false);
            }
            refreshEntity(e);
        }
        m_skippingEntities.remove(name);
    }
    for (auto it = m_nodeStates.begin(); it != m_nodeStates.end();) {
        if (nodes.contains(it.key())) { ++it; continue; }
        // A node we no longer drive goes back to its base, unless something
        // else moved it since our last write.
        if (Ogre::SceneNode* n = nodeByName(it.key()))
            if (it->written && sameTransform(localOf(n), it->lastWritten)) writeLocal(n, it->base);
        it = m_nodeStates.erase(it);
    }
    if (activeCount() > 0) ensureListener();
    else detachListener();
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

bool ConstraintManager::validate(Constraint* c, QString* error) const
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    if (c->owner.isEmpty()) return fail(QStringLiteral("a constraint needs an owner (node:Name or bone:Entity/Bone)"));
    if (c->owner.isBone()) {
        Ogre::Entity* e = entityByName(c->owner.object);
        if (!e) return fail(QStringLiteral("no entity named '%1'").arg(c->owner.object));
        if (!boneOf(e, c->owner.bone)) return fail(QStringLiteral("'%1' has no bone '%2'").arg(c->owner.object, c->owner.bone));
        if (c->type == Type::IK) {
            Ogre::Bone* b = boneOf(e, c->owner.bone);
            auto* mid = dynamic_cast<Ogre::Bone*>(b->getParent());
            if (!mid || !dynamic_cast<Ogre::Bone*>(mid->getParent()))
                return fail(QStringLiteral("IK needs the END bone of a 2-bone chain: '%1' has no parent and grandparent bone")
                                .arg(c->owner.bone));
        }
    } else {
        if (!nodeByName(c->owner.object)) return fail(QStringLiteral("no scene node named '%1'").arg(c->owner.object));
        if (!nodeCanOwn(c->type))
            return fail(QStringLiteral("%1 needs a bone chain: pick a bone owner (the END bone, e.g. a hand)").arg(AnimCon::typeLabel(c->type)));
    }
    if (needsTarget(c->type)) {
        Transform t;
        if (c->target.isEmpty()) return fail(QStringLiteral("%1 needs a target").arg(AnimCon::typeLabel(c->type)));
        if (!refWorld(c->target, &t)) return fail(QStringLiteral("target '%1' not found").arg(formatRef(c->target)));
        if (c->target == c->owner) return fail(QStringLiteral("a constraint cannot target its own owner"));
    }
    if (!c->pole.isEmpty()) {
        Transform t;
        if (!refWorld(c->pole, &t)) return fail(QStringLiteral("pole '%1' not found").arg(formatRef(c->pole)));
    }
    if (c->type == Type::ParentOf && !c->hasOffset) {
        // Keep the owner exactly where it is when the constraint is added.
        Transform owner, target;
        if (c->owner.isBone()) {
            Ogre::Entity* e = entityByName(c->owner.object);
            owner = compose(entityWorld(e), derivedOf(boneOf(e, c->owner.bone)));
        } else {
            owner = derivedOf(nodeByName(c->owner.object));
        }
        refWorld(c->target, &target);
        c->offset = relativeTo(target, owner);
        c->hasOffset = true;
    }
    return true;
}

ConstraintManager::Result ConstraintManager::commit(const std::vector<Constraint>& next, const QString& label,
                                                   const QString& id, bool undoable)
{
    Result r;
    r.id = id;
    const QJsonObject before = document();
    m_cons = next;
    syncOwners();
    const QJsonObject after = document();
    if (undoable)
        if (UndoManager* um = UndoManager::getSingleton())
            um->push(new ConstraintDocCommand(label, before, after));
    emit constraintsChanged();
    r.ok = true;
    setStatus(label, true);
    return r;
}

bool ConstraintManager::applyDocument(const QJsonObject& doc, QString* error)
{
    std::vector<Constraint> next;
    if (!fromDocument(doc, &next, error)) return false;
    m_cons = std::move(next);
    syncOwners();
    emit constraintsChanged();
    return true;
}

void ConstraintManager::clear()
{
    m_cons.clear();
    syncOwners();
    emit constraintsChanged();
}

ConstraintManager::Result ConstraintManager::add(Constraint c, bool undoable)
{
    ensureSceneHook();
    Result r;
    QString err;
    if (!validate(&c, &err)) { r.error = err; setStatus(err, false); crumb("error", err); return r; }
    if (c.id.isEmpty() || find(c.id)) c.id = uniqueId(m_cons);
    std::vector<Constraint> next = m_cons;
    // Insert at the TOP of the owner's stack (just before its first entry).
    auto pos = std::find_if(next.begin(), next.end(), [&](const Constraint& x) { return x.owner == c.owner; });
    next.insert(pos, c);
    r = commit(next, QStringLiteral("Add %1 constraint").arg(AnimCon::typeLabel(c.type)), c.id, undoable);
    crumb("add", typeId(c.type) + QStringLiteral(" ") + formatRef(c.owner) + QStringLiteral(" → ") + formatRef(c.target));
    return r;
}

ConstraintManager::Result ConstraintManager::remove(const QString& id, bool undoable)
{
    Result r;
    r.id = id;
    if (!find(id)) { r.error = QStringLiteral("no constraint '%1'").arg(id); setStatus(r.error, false); return r; }
    std::vector<Constraint> next = m_cons;
    next.erase(std::remove_if(next.begin(), next.end(), [&](const Constraint& c) { return c.id == id; }), next.end());
    r = commit(next, QStringLiteral("Remove constraint"), id, undoable);
    crumb("remove", id);
    return r;
}

ConstraintManager::Result ConstraintManager::setParams(const QString& id, const QList<QPair<QString, QString>>& params,
                                                      const bool* enabled, bool undoable)
{
    Result r;
    r.id = id;
    std::vector<Constraint> next = m_cons;
    Constraint* c = nullptr;
    for (auto& x : next) if (x.id == id) c = &x;
    if (!c) { r.error = QStringLiteral("no constraint '%1'").arg(id); setStatus(r.error, false); return r; }
    for (const auto& p : params) {
        QString err;
        if (!applyParam(c, p.first, p.second, &err)) { r.error = err; setStatus(err, false); return r; }
    }
    if (enabled) c->enabled = *enabled;
    QString err;
    if (!params.isEmpty() && !validate(c, &err)) { r.error = err; setStatus(err, false); return r; }
    r = commit(next, params.isEmpty() ? (c->enabled ? QStringLiteral("Enable constraint") : QStringLiteral("Mute constraint"))
                                      : QStringLiteral("Edit constraint"),
               id, undoable);
    crumb(params.isEmpty() ? "enable" : "edit", id);
    return r;
}

ConstraintManager::Result ConstraintManager::move(const QString& id, int direction, bool undoable)
{
    Result r;
    r.id = id;
    std::vector<Constraint> next = m_cons;
    int i = -1;
    for (int k = 0; k < int(next.size()); ++k) if (next[size_t(k)].id == id) i = k;
    if (i < 0) { r.error = QStringLiteral("no constraint '%1'").arg(id); setStatus(r.error, false); return r; }
    // Swap with the neighbour in the SAME owner's stack.
    const Ref owner = next[size_t(i)].owner;
    int j = i + (direction < 0 ? -1 : 1);
    while (j >= 0 && j < int(next.size()) && !(next[size_t(j)].owner == owner)) j += (direction < 0 ? -1 : 1);
    if (j < 0 || j >= int(next.size())) {
        r.error = direction < 0 ? QStringLiteral("already at the top") : QStringLiteral("already at the bottom");
        setStatus(r.error, false);
        return r;
    }
    std::swap(next[size_t(i)], next[size_t(j)]);
    r = commit(next, QStringLiteral("Reorder constraints"), id, undoable);
    crumb("reorder", QStringLiteral("%1 %2").arg(id, direction < 0 ? QStringLiteral("up") : QStringLiteral("down")));
    return r;
}

// ---------------------------------------------------------------------------
// Bake
// ---------------------------------------------------------------------------

void ConstraintManager::restoreTracks(const QJsonArray& snapshots)
{
    Ogre::SceneManager* s = sceneMgr();
    for (const QJsonValue& v : snapshots) {
        const QJsonObject o = v.toObject();
        const QString kind = o.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("bone")) {
            Ogre::Entity* e = entityByName(o.value(QStringLiteral("entity")).toString());
            Ogre::Bone* b = boneOf(e, o.value(QStringLiteral("bone")).toString());
            const std::string clip = o.value(QStringLiteral("clip")).toString().toStdString();
            if (!b || !e->getSkeleton()->hasAnimation(clip)) continue;
            Ogre::Animation* a = e->getSkeleton()->getAnimation(clip);
            const unsigned short h = b->getHandle();
            if (!o.value(QStringLiteral("hadTrack")).toBool()) {
                if (a->hasNodeTrack(h)) a->destroyNodeTrack(h);
            } else {
                writeTrack(a->hasNodeTrack(h) ? a->getNodeTrack(h) : a->createNodeTrack(h, b),
                           o.value(QStringLiteral("keys")).toArray());
            }
            refreshEntity(e);
        } else if (kind == QLatin1String("node") && s) {
            const QString clip = o.value(QStringLiteral("clip")).toString();
            Ogre::SceneNode* n = nodeByName(o.value(QStringLiteral("node")).toString());
            const bool existed = o.value(QStringLiteral("clipExisted")).toBool();
            if (!existed) {
                if (s->hasAnimation(clip.toStdString())) NodeAnimationManager::instance()->deleteClip(clip);
                continue;
            }
            if (!s->hasAnimation(clip.toStdString())) {
                NodeAnimationManager::instance()->createClip(clip, o.value(QStringLiteral("length")).toDouble(1.0));
                NodeAnimationManager::instance()->setClipEnabled(clip, true);
            }
            Ogre::Animation* a = s->getAnimation(clip.toStdString());
            a->setLength(float(o.value(QStringLiteral("length")).toDouble(a->getLength())));
            if (s->hasAnimationState(clip.toStdString())) s->getAnimationState(clip.toStdString())->setLength(a->getLength());
            Ogre::NodeAnimationTrack* t = nodeTrackFor(a, n);
            if (!o.value(QStringLiteral("hadTrack")).toBool()) {
                if (t) a->destroyNodeTrack(t->getHandle());
            } else if (n) {
                if (!t) {
                    NodeAnimationManager::instance()->addKeyframe(clip, QString::fromStdString(n->getName()), 0.0,
                                                                  n->getPosition(), n->getOrientation(), n->getScale());
                    t = nodeTrackFor(a, n);
                }
                if (t) writeTrack(t, o.value(QStringLiteral("keys")).toArray());
            }
            emit NodeAnimationManager::instance()->keyframesChanged(clip);
            emit NodeAnimationManager::instance()->clipsChanged();
        }
    }
}

ConstraintManager::Result ConstraintManager::bake(const BakeOptions& opts, bool undoable)
{
    Result r;
    Ogre::SceneManager* scene = sceneMgr();
    std::vector<Constraint> selected;
    for (const auto& c : m_cons)
        if (c.enabled && c.influence > 0.0 && (opts.ids.isEmpty() || opts.ids.contains(c.id))) selected.push_back(c);
    if (selected.empty() || !scene) {
        r.error = QStringLiteral("nothing to bake: no enabled constraint selected");
        setStatus(r.error, false);
        return r;
    }

    // Resolve every entity's target clip BEFORE touching anything, so a bake
    // that cannot complete changes nothing (no half-written tracks without an
    // undo step).
    QSet<QString> entities;
    for (const auto& c : selected) if (c.owner.isBone()) entities.insert(c.owner.object);
    QHash<QString, QString> clipFor;
    for (const QString& ename : entities) {
        Ogre::Entity* e = entityByName(ename);
        if (!e || !e->hasSkeleton()) continue;
        Ogre::SkeletonInstance* skel = e->getSkeleton();
        Ogre::AnimationStateSet* states = e->getAllAnimationStates();
        QString clip = opts.clip;
        if (clip.isEmpty() || !skel->hasAnimation(clip.toStdString())) {
            clip.clear();
            if (states)
                for (const auto& [n, st] : states->getAnimationStates())
                    if (st->getEnabled() && skel->hasAnimation(n)) { clip = QString::fromStdString(n); break; }
            auto* acc = AnimationControlController::instance();
            if (clip.isEmpty() && acc && acc->selectedEntityName() == ename && skel->hasAnimation(acc->selectedAnimation().toStdString()))
                clip = acc->selectedAnimation();
            if (clip.isEmpty() && skel->getNumAnimations() > 0) clip = QString::fromStdString(skel->getAnimation(0)->getName());
        }
        if (clip.isEmpty() || !states || !states->hasAnimationState(clip.toStdString())) {
            r.error = QStringLiteral("'%1' has no skeletal clip to bake into").arg(ename);
            setStatus(r.error, false);
            crumb("error", r.error);
            return r;
        }
        clipFor.insert(ename, clip);
    }
    // Whether each node clip existed is recorded ONCE per clip: when two
    // nodes bake into a new "Constraints" clip, the second must not see the
    // clip the first just created, or undo would delete it and then recreate
    // it empty.
    QHash<QString, bool> nodeClipExisted;

    // Evaluate ONLY the selected constraints while sampling.
    const std::vector<Constraint> all = m_cons;
    m_cons = selected;
    const int fps = std::clamp(opts.fps, 1, 240);
    QJsonArray before, after;
    int bakedKeys = 0;
    QStringList places;

    // Scene (node) animation states: their time drives animated targets.
    struct SceneStateSave { std::string name; float time; };
    std::vector<SceneStateSave> sceneStates;
    for (const auto& [name, st] : scene->getAnimationStates()) sceneStates.push_back({name, st->getTimePosition()});
    auto setSceneTime = [&](double t) {
        for (const auto& [name, st] : scene->getAnimationStates())
            if (st->getEnabled()) st->setTimePosition(float(std::fmod(t, std::max(1e-3f, st->getLength()))));
        scene->_applySceneAnimations();
    };

    // ---- bone owners, per entity ----
    for (auto cit = clipFor.cbegin(); cit != clipFor.cend(); ++cit) {
        const QString& ename = cit.key();
        const QString& clip = cit.value();
        Ogre::Entity* e = entityByName(ename);
        Ogre::SkeletonInstance* skel = e->getSkeleton();
        Ogre::AnimationStateSet* states = e->getAllAnimationStates();
        Ogre::Animation* anim = skel->getAnimation(clip.toStdString());
        // Play only the target clip while sampling; restore afterwards.
        struct Save { std::string name; bool enabled; float time; float weight; };
        std::vector<Save> saves;
        for (const auto& [n, st] : states->getAnimationStates()) {
            saves.push_back({n, st->getEnabled(), st->getTimePosition(), st->getWeight()});
            st->setEnabled(n == clip.toStdString());
        }
        Ogre::AnimationState* state = states->getAnimationState(clip.toStdString());
        state->setWeight(1.0f);
        QHash<QString, QJsonArray> samples; // bone → keys (bind-relative)
        for (double t : times(anim->getLength(), fps)) {
            state->setTimePosition(float(t));
            setSceneTime(t);
            evaluate();
            for (const QString& tb : m_touchedBones) {
                if (!tb.startsWith(ename + QLatin1Char('/'))) continue;
                Ogre::Bone* b = boneOf(e, tb.mid(ename.size() + 1));
                if (!b) continue;
                const Ogre::Quaternion rot = b->getInitialOrientation().Inverse() * b->getOrientation();
                const Ogre::Vector3 pos = b->getPosition() - b->getInitialPosition();
                const Ogre::Vector3 sc = b->getScale() / b->getInitialScale();
                samples[tb].append(trsKey(t, pos, rot, sc));
            }
        }
        for (const auto& s : saves) {
            Ogre::AnimationState* st = states->getAnimationState(s.name);
            st->setEnabled(s.enabled);
            st->setTimePosition(s.time);
            st->setWeight(s.weight);
        }
        for (auto it = samples.cbegin(); it != samples.cend(); ++it) {
            Ogre::Bone* b = boneOf(e, it.key().mid(ename.size() + 1));
            const unsigned short h = b->getHandle();
            const bool had = anim->hasNodeTrack(h);
            QJsonObject snap{{QStringLiteral("kind"), QStringLiteral("bone")}, {QStringLiteral("entity"), ename},
                             {QStringLiteral("bone"), QString::fromStdString(b->getName())}, {QStringLiteral("clip"), clip}};
            QJsonObject b0 = snap, b1 = snap;
            b0[QStringLiteral("hadTrack")] = had;
            b0[QStringLiteral("keys")] = had ? readTrack(anim->getNodeTrack(h)) : QJsonArray{};
            writeTrack(had ? anim->getNodeTrack(h) : anim->createNodeTrack(h, b), it.value());
            b1[QStringLiteral("hadTrack")] = true;
            b1[QStringLiteral("keys")] = it.value();
            before.append(b0);
            after.append(b1);
            bakedKeys += int(it.value().size());
        }
        places << QStringLiteral("'%1' of %2 (%3 bones)").arg(clip, ename).arg(samples.size());
        refreshEntity(e);
    }

    // ---- node owners ----
    QSet<QString> nodes;
    for (const auto& c : selected) if (!c.owner.isBone()) nodes.insert(c.owner.object);
    for (const QString& nname : nodes) {
        Ogre::SceneNode* node = nodeByName(nname);
        if (!node) continue;
        QString clip = opts.nodeClip;
        if (clip.isEmpty())
            for (const QString& cl : NodeAnimationManager::instance()->listClips())
                if (NodeAnimationManager::instance()->animatedNodes(cl).contains(nname)) { clip = cl; break; }
        if (clip.isEmpty()) clip = QString::fromLatin1(kDefaultNodeClip);
        if (!nodeClipExisted.contains(clip)) nodeClipExisted.insert(clip, scene->hasAnimation(clip.toStdString()));
        const bool existed = nodeClipExisted.value(clip);
        const bool exists = scene->hasAnimation(clip.toStdString());
        double len = opts.nodeLength;
        if (len <= 0.0) {
            for (const QString& cl : NodeAnimationManager::instance()->listClips())
                len = std::max(len, NodeAnimationManager::instance()->clipLength(cl));
            if (len <= 0.0) len = 1.0;
        }
        if (!exists) {
            NodeAnimationManager::instance()->createClip(clip, len);
            NodeAnimationManager::instance()->setClipEnabled(clip, true);
        }
        Ogre::Animation* anim = scene->getAnimation(clip.toStdString());
        const double clipLen = exists ? anim->getLength() : len;
        Ogre::NodeAnimationTrack* track = nodeTrackFor(anim, node);
        QJsonObject snap{{QStringLiteral("kind"), QStringLiteral("node")}, {QStringLiteral("node"), nname},
                         {QStringLiteral("clip"), clip}};
        QJsonObject b0 = snap, b1 = snap;
        b0[QStringLiteral("clipExisted")] = existed;
        b0[QStringLiteral("length")] = double(anim->getLength());
        b0[QStringLiteral("hadTrack")] = track != nullptr;
        b0[QStringLiteral("keys")] = readTrack(track);
        QJsonArray keys;
        for (double t : times(clipLen, fps)) {
            setSceneTime(t);
            evaluate();
            keys.append(trsKey(t, node->getPosition(), node->getOrientation(), node->getScale()));
        }
        if (!track) {
            NodeAnimationManager::instance()->addKeyframe(clip, nname, 0.0, node->getPosition(), node->getOrientation(),
                                                          node->getScale());
            track = nodeTrackFor(anim, node);
        }
        if (track) writeTrack(track, keys);
        b1[QStringLiteral("clipExisted")] = true;
        b1[QStringLiteral("length")] = double(anim->getLength());
        b1[QStringLiteral("hadTrack")] = true;
        b1[QStringLiteral("keys")] = keys;
        before.append(b0);
        after.append(b1);
        bakedKeys += int(keys.size());
        places << QStringLiteral("node clip '%1' (%2)").arg(clip, nname);
        emit NodeAnimationManager::instance()->keyframesChanged(clip);
        emit NodeAnimationManager::instance()->clipsChanged();
    }

    // Restore scene clocks and the full constraint list, then mute what was
    // baked (the keys now carry the motion).
    for (const auto& s : sceneStates)
        if (scene->hasAnimationState(s.name)) scene->getAnimationState(s.name)->setTimePosition(s.time);
    m_cons = all;
    const QJsonObject docBefore = document();
    for (auto& c : m_cons)
        for (const auto& s : selected) if (c.id == s.id) c.enabled = false;
    syncOwners();
    const QJsonObject docAfter = document();
    if (undoable)
        if (UndoManager* um = UndoManager::getSingleton())
            um->push(new ConstraintDocCommand(QStringLiteral("Bake constraints"), docBefore, docAfter, before, after));
    emit constraintsChanged();
    r.ok = true;
    setStatus(QStringLiteral("Baked %1 keyframes into %2; the baked constraints are muted.").arg(bakedKeys).arg(places.join(QStringLiteral(", "))), true);
    crumb("bake", QStringLiteral("%1 constraints, %2 keys").arg(selected.size()).arg(bakedKeys));
    return r;
}

// ---------------------------------------------------------------------------
// Sidecar
// ---------------------------------------------------------------------------

QString ConstraintManager::sidecarPath(const QString& assetPath)
{
    const QFileInfo fi(assetPath);
    return fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".constraints.json"));
}

QJsonObject ConstraintManager::sidecarMeta(const QString& assetPath)
{
    QFile f(sidecarPath(assetPath));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("meta")).toObject();
}

bool ConstraintManager::writeSidecar(const QString& assetPath, const QStringList& objects, const QJsonObject& meta) const
{
    const QString path = sidecarPath(assetPath);
    std::vector<Constraint> keep;
    for (const auto& c : m_cons)
        if (objects.isEmpty() || objects.contains(c.owner.object)) keep.push_back(c);
    if (keep.empty()) {
        if (QFile::exists(path)) QFile::remove(path);
        return false;
    }
    QJsonObject doc = toDocument(keep);
    if (!meta.isEmpty()) doc[QStringLiteral("meta")] = meta;
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
    crumb("save", QStringLiteral("%1 constraints").arg(keep.size()));
    return f.commit();
}

int ConstraintManager::loadSidecar(const QString& assetPath, const QHash<QString, QString>& rename, QString* error)
{
    QFile f(sidecarPath(assetPath));
    if (!f.exists()) return 0;
    if (!f.open(QIODevice::ReadOnly)) { if (error) *error = f.errorString(); return -1; }
    std::vector<Constraint> file;
    if (!fromDocument(QJsonDocument::fromJson(f.readAll()).object(), &file, error)) return -1;
    ensureSceneHook();
    auto ren = [&](Ref r) { if (rename.contains(r.object)) r.object = rename.value(r.object); return r; };
    std::vector<Constraint> next = m_cons;
    QSet<QString> taken;
    for (const auto& c : next) taken.insert(c.id);
    QSet<QString> reserved = taken;
    for (const auto& c : file) reserved.insert(c.id);
    // Same content (ignoring the id) on the same owner = already loaded:
    // re-importing an asset must not stack a second copy of its constraints.
    auto sameContent = [](const Constraint& a, const Constraint& b) {
        QJsonObject ja = toJson(a), jb = toJson(b);
        ja.remove(QStringLiteral("id"));
        jb.remove(QStringLiteral("id"));
        return ja == jb;
    };
    int added = 0;
    for (Constraint c : file) {
        c.owner = ren(c.owner);
        c.target = ren(c.target);
        c.pole = ren(c.pole);
        if (std::any_of(next.begin(), next.end(), [&](const Constraint& x) { return sameContent(x, c); })) continue;
        ++added;
        if (taken.contains(c.id)) {
            int n = 1;
            QString id;
            do { id = QStringLiteral("con_%1").arg(n++); } while (reserved.contains(id) || taken.contains(id));
            c.id = id;
        }
        taken.insert(c.id);
        next.push_back(c);
    }
    m_cons = std::move(next);
    syncOwners();
    emit constraintsChanged();
    crumb("load", QStringLiteral("%1 constraints (%2 new)").arg(file.size()).arg(added));
    return added;
}

// ---------------------------------------------------------------------------
// QML helpers
// ---------------------------------------------------------------------------

namespace {
QVariantMap rowOf(const Constraint& c, bool bound)
{
    QVariantMap m;
    m[QStringLiteral("id")] = c.id;
    m[QStringLiteral("name")] = displayName(c);
    m[QStringLiteral("type")] = typeId(c.type);
    m[QStringLiteral("typeLabel")] = AnimCon::typeLabel(c.type);
    m[QStringLiteral("owner")] = formatRef(c.owner);
    m[QStringLiteral("target")] = formatRef(c.target);
    m[QStringLiteral("enabled")] = c.enabled;
    m[QStringLiteral("influence")] = c.influence;
    m[QStringLiteral("bound")] = bound;
    return m;
}
bool isBound(const Constraint& c)
{
    Transform t;
    const bool ownerOk = c.owner.isBone() ? boneOf(entityByName(c.owner.object), c.owner.bone) != nullptr
                                          : nodeByName(c.owner.object) != nullptr;
    return ownerOk && (!needsTarget(c.type) || refWorld(c.target, &t));
}
} // namespace

QVariantList ConstraintManager::constraintRows() const
{
    QVariantList rows;
    for (const auto& c : m_cons) rows << rowOf(c, isBound(c));
    return rows;
}

QVariantList ConstraintManager::rowsFor(const QString& owner) const
{
    Ref o;
    parseRef(owner, &o);
    QVariantList rows;
    for (const auto& c : m_cons) if (c.owner == o) rows << rowOf(c, isBound(c));
    return rows;
}

QVariantMap ConstraintManager::details(const QString& id) const
{
    const Constraint* c = find(id);
    if (!c) return {};
    QVariantMap m = toJson(*c).toVariantMap();
    m[QStringLiteral("typeLabel")] = AnimCon::typeLabel(c->type);
    m[QStringLiteral("aim")] = axisId(c->aimAxis);
    m[QStringLiteral("up")] = axisId(c->upAxis);
    m[QStringLiteral("x")] = c->useX; m[QStringLiteral("y")] = c->useY; m[QStringLiteral("z")] = c->useZ;
    m[QStringLiteral("limit_x")] = c->limitX; m[QStringLiteral("limit_y")] = c->limitY; m[QStringLiteral("limit_z")] = c->limitZ;
    m[QStringLiteral("min_x")] = c->minDeg.x; m[QStringLiteral("min_y")] = c->minDeg.y; m[QStringLiteral("min_z")] = c->minDeg.z;
    m[QStringLiteral("max_x")] = c->maxDeg.x; m[QStringLiteral("max_y")] = c->maxDeg.y; m[QStringLiteral("max_z")] = c->maxDeg.z;
    return m;
}

QString ConstraintManager::typeLabel(const QString& id) const
{
    Type t;
    return typeFromId(id, &t) ? AnimCon::typeLabel(t) : id;
}

QString ConstraintManager::ownerFromSelection() const
{
    // Pointer-membership checks against Ogre's registries before any
    // dereference (the selection can briefly hold destroyed objects).
    SelectionSet* sel = SelectionSet::getSingletonPtr();
    Ogre::SceneManager* scene = sceneMgr();
    if (!sel || !scene) return {};
    Ogre::Entity* ent = nullptr;
    const auto& entities = scene->getMovableObjects("Entity");
    for (Ogre::Entity* e : sel->getResolvedEntities())
        for (const auto& kv : entities) if (kv.second == e) { ent = e; break; }
    if (ent && ent->hasSkeleton()) {
        auto* acc = AnimationControlController::instance();
        if (acc && acc->selectedEntityName() == QString::fromStdString(ent->getName()) && !acc->selectedBone().isEmpty()
            && boneOf(ent, acc->selectedBone()))
            return QStringLiteral("bone:%1/%2").arg(QString::fromStdString(ent->getName()), acc->selectedBone());
    }
    std::function<bool(Ogre::Node*, Ogre::Node*)> live = [&](Ogre::Node* root, Ogre::Node* want) {
        if (root == want) return true;
        for (Ogre::Node* c : root->getChildren()) if (live(c, want)) return true;
        return false;
    };
    for (Ogre::SceneNode* n : sel->getNodesSelectionList())
        if (n && live(scene->getRootSceneNode(), n)) return QStringLiteral("node:%1").arg(QString::fromStdString(n->getName()));
    if (ent && ent->getParentSceneNode() && live(scene->getRootSceneNode(), ent->getParentSceneNode()))
        return QStringLiteral("node:%1").arg(QString::fromStdString(ent->getParentSceneNode()->getName()));
    return {};
}

QStringList ConstraintManager::nodeNames() const
{
    QStringList out;
    if (Manager* m = Manager::getSingletonPtr())
        for (Ogre::SceneNode* n : m->getSceneNodes()) if (n) out << QString::fromStdString(n->getName());
    return out;
}

QStringList ConstraintManager::skinnedEntities() const
{
    QStringList out;
    if (Manager* m = Manager::getSingletonPtr())
        for (Ogre::Entity* e : m->getEntities()) if (e && e->hasSkeleton()) out << QString::fromStdString(e->getName());
    return out;
}

QStringList ConstraintManager::bonesOf(const QString& entity) const
{
    QStringList out;
    Ogre::Entity* e = entityByName(entity);
    if (e && e->hasSkeleton())
        for (unsigned short i = 0; i < e->getSkeleton()->getNumBones(); ++i)
            out << QString::fromStdString(e->getSkeleton()->getBone(i)->getName());
    return out;
}

QStringList ConstraintManager::clipsOf(const QString& entity) const
{
    QStringList out;
    Ogre::Entity* e = entityByName(entity);
    if (e && e->hasSkeleton())
        for (unsigned short i = 0; i < e->getSkeleton()->getNumAnimations(); ++i)
            out << QString::fromStdString(e->getSkeleton()->getAnimation(i)->getName());
    return out;
}

bool ConstraintManager::addFromUi(const QString& typeStr, const QString& owner, const QString& target, const QString& pole)
{
    Constraint c;
    QString err;
    if (!typeFromId(typeStr, &c.type)) { setStatus(QStringLiteral("unknown constraint type '%1'").arg(typeStr), false); return false; }
    if (!parseRef(owner, &c.owner, &err) || !parseRef(target, &c.target, &err) || !parseRef(pole, &c.pole, &err)) {
        setStatus(err, false);
        return false;
    }
    if (!needsTarget(c.type)) c.target = Ref{};
    if (c.type != Type::IK) c.pole = Ref{};
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("Constraints: add %1").arg(typeStr));
    return add(c).ok;
}

bool ConstraintManager::removeFromUi(const QString& id) { return remove(id).ok; }
bool ConstraintManager::setEnabledFromUi(const QString& id, bool enabled) { return setParams(id, {}, &enabled).ok; }
bool ConstraintManager::setParamFromUi(const QString& id, const QString& key, const QString& value)
{
    return setParams(id, {{key, value}}, nullptr).ok;
}
bool ConstraintManager::moveFromUi(const QString& id, int direction) { return move(id, direction).ok; }

bool ConstraintManager::bakeFromUi(const QString& owner, const QString& clip, int fps)
{
    BakeOptions o;
    Ref r;
    parseRef(owner, &r);
    for (const auto& c : m_cons) if (c.enabled && (r.isEmpty() || c.owner == r)) o.ids << c.id;
    o.clip = clip;
    o.fps = fps > 0 ? fps : 30;
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"), QStringLiteral("Constraints: bake"));
    return bake(o).ok;
}
