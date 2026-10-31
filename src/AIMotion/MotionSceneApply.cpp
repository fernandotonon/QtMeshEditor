#include "MotionSceneApply.h"

#include "../AnimationMerger.h"
#include "../AnimationRetargeter.h"
#include "../MotionInbetween.h"
#include "../SentryReporter.h"
#include "../UndoManager.h"
#include "../commands/RetargetAnimationCommand.h"

#include <QJsonArray>

#include <algorithm>

#include <OgreAnimation.h>
#include <OgreEntity.h>
#include <OgreBone.h>
#include <OgreKeyFrame.h>
#include <OgreMesh.h>
#include <OgreSkeletonInstance.h>

namespace AIMotion {

QString variantClipName(const QString& base, int index)
{
    return index == 0 ? base : QStringLiteral("%1_v%2").arg(base).arg(index + 1);
}

QJsonObject outcomeJson(const ApplyOutcome& out)
{
    return QJsonObject{
        {QStringLiteral("name"), QString::fromStdString(out.clip)},
        {QStringLiteral("frames"), out.frames},
        {QStringLiteral("length"), out.length},
        {QStringLiteral("mappedBones"), out.mappedBones},
        {QStringLiteral("fingerBones"), out.fingerBones},
        {QStringLiteral("unmappedTarget"), QJsonArray::fromStringList(out.unmappedTarget)},
        {QStringLiteral("unmappedSource"), QJsonArray::fromStringList(out.unmappedSource)},
        {QStringLiteral("excludedSource"), QJsonArray::fromStringList(out.excludedSource)},
        {QStringLiteral("inferredFingers"), QJsonArray::fromStringList(out.inferredFingers)},
        {QStringLiteral("heightScale"), out.report.heightScale},
        {QStringLiteral("rootBone"), QString::fromStdString(out.report.rootTarget)},
        {QStringLiteral("mapVersion"), out.mapVersion},
    };
}

bool loadMotionFiles(const QStringList& files, std::vector<Motion>* out, QString* error)
{
    out->clear();
    for (const QString& f : files) {
        Motion m;
        QString e;
        if (!loadInterchange(f, &m, &e)) {
            if (error) *error = QStringLiteral("%1: %2").arg(f, e);
            return false;
        }
        out->push_back(std::move(m));
    }
    return true;
}

SceneApplyResult applyMotionsToEntity(Ogre::Entity* entity, const std::vector<Motion>& motions,
                                      const SceneApplyOptions& options)
{
    SceneApplyResult r;
    if (!entity || !entity->hasSkeleton()) { r.error = QStringLiteral("no rigged entity: Kimodo motion needs a skeleton"); return r; }
    if (motions.empty()) { r.error = QStringLiteral("no motion to apply"); return r; }
    Ogre::Skeleton* skel = entity->getSkeleton();
    const QString base = options.baseName.trimmed().isEmpty() ? QStringLiteral("generated_kimodo") : options.baseName.trimmed();

    // Retarget every variation first; a failure removes what this call wrote so nothing half-applied is left behind.
    for (size_t v = 0; v < motions.size(); ++v) {
        const std::string clip = Retarget::uniqueAnimationName(skel, variantClipName(base, int(v)).toStdString());
        ApplyOptions ao;
        ao.rootMotion = options.rootMotion;
        ApplyOutcome out = applyMotionToSkeleton(skel, motions[v], clip, ao);
        if (!out.ok) {
            for (const QString& c : r.clips) skel->removeAnimation(c.toStdString());
            r.clips.clear();
            r.outcomes.clear();
            r.error = motions.size() > 1 ? QStringLiteral("variation %1: %2").arg(v + 1).arg(out.error) : out.error;
            return r;
        }
        if (options.footPin) AnimationMerger::pinFeet(skel, clip);
        r.clips << QString::fromStdString(clip);
        r.outcomes.push_back(std::move(out));
    }
    entity->refreshAvailableAnimationState();

    if (options.undoable)
        if (auto* um = UndoManager::getSingleton()) {
            um->stack()->beginMacro(QStringLiteral("Kimodo motion (%1 clip%2)").arg(r.clips.size()).arg(r.clips.size() == 1 ? "" : "s"));
            for (const QString& c : r.clips) um->push(new RetargetAnimationCommand(entity->getName(), c.toStdString(), true));
            um->stack()->endMacro();
            r.undoIndexAfter = um->stack()->index();
        }
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.kimodo"),
                                  QStringLiteral("applied %1 clip(s) to %2").arg(r.clips.size()).arg(QString::fromStdString(entity->getName())));
    r.ok = true;
    return r;
}

namespace {

// The 22 canonical roles as a rig the incompatible-skeleton retargeter (#523) can read: T-pose joints laid out
// along the clip's rest directions (body facing +Z, left at +X: the frame the model was trained in), metres.
// Names are the canonical role names, which the role matcher maps back onto any humanoid.
Retarget::RigDesc canonicalRig(const std::vector<std::array<float, 3>>& restDir)
{
    static const float kLength[22] = {0.10f, 0.12f, 0.23f, 0.07f, 0.06f, 0.15f,   // hips .. head
                                      0.15f, 0.28f, 0.25f, 0.08f,                   // right collar .. hand
                                      0.15f, 0.28f, 0.25f, 0.08f,                   // left
                                      0.05f, 0.42f, 0.41f, 0.15f,                   // right buttock .. foot
                                      0.05f, 0.42f, 0.41f, 0.15f};                  // left
    Retarget::RigDesc rig;
    std::vector<Ogre::Vector3> world(22);
    for (int r = 0; r < 22; ++r) {
        const int par = MotionInbetween::canonicalParentOf(r);
        Ogre::Vector3 p(0.0f, 1.0f, 0.0f);
        if (par >= 0) {
            const auto& d = restDir[size_t(par)];
            // A joint starts where its parent's bone ends, except the branches: the hips' children (abdomen, buttocks)
            // and the chest's (neck, collars) spread sideways from the branching joint so the left/right axis exists.
            if (r == 14 || r == 18) p = world[size_t(par)] + Ogre::Vector3(r == 14 ? -0.09f : 0.09f, -0.03f, 0.0f);
            else if (r == 6 || r == 10) p = world[size_t(par)] + Ogre::Vector3(r == 6 ? -0.03f : 0.03f, 0.20f, 0.0f);
            else p = world[size_t(par)] + Ogre::Vector3(d[0], d[1], d[2]) * kLength[par];
        }
        world[size_t(r)] = p;
        rig.names.push_back(MotionInbetween::canonicalJointName(r).toStdString());
        rig.parent.push_back(par);
        rig.bindPos.push_back(par < 0 ? p : p - world[size_t(par)]);   // rest rotations are identity: world offset == local
        rig.bindRot.push_back(Ogre::Quaternion::IDENTITY);
    }
    return rig;
}

// Canonical WORLD-frame quats (rest = identity) -> local poses of canonicalRig.
std::vector<Retarget::LocalPose> canonicalLocalPoses(const Retarget::RigDesc& rig,
                                                      const std::vector<std::vector<std::array<float, 4>>>& quats)
{
    std::vector<Retarget::LocalPose> out;
    out.reserve(quats.size());
    for (const auto& frame : quats) {
        Retarget::LocalPose pose = Retarget::bindPose(rig);
        std::vector<Ogre::Quaternion> w(22);
        for (int r = 0; r < 22; ++r) {
            w[size_t(r)] = Ogre::Quaternion(frame[size_t(r)][3], frame[size_t(r)][0], frame[size_t(r)][1], frame[size_t(r)][2]);
            w[size_t(r)].normalise();
        }
        for (int r = 0; r < 22; ++r) {
            const int par = rig.parent[size_t(r)];
            Ogre::Quaternion l = par < 0 ? w[size_t(r)] : w[size_t(par)].Inverse() * w[size_t(r)];
            l.normalise();
            pose.rot[size_t(r)] = l;
        }
        out.push_back(std::move(pose));
    }
    return out;
}

// One canonical clip onto `skel` through Retarget::retargetFrames with the SAM 3D Body options (rotation only;
// limbs aligned A-pose vs T-pose, spine/neck/head transported as is; feet unmapped, see below). The retargeter aligns the two
// rigs' humanoid frames, so a rig facing -Z gets the motion turned, never mirrored.
bool retargetCanonical(Ogre::Skeleton* skel, const std::string& clip, const CanonicalMotion& m, QString* error)
{
    if (m.quats.size() < 2 || m.restDir.size() != 22) { if (error) *error = QStringLiteral("empty or malformed canonical clip"); return false; }
    for (const auto& f : m.quats)
        if (f.size() != 22) { if (error) *error = QStringLiteral("canonical clip frames need 22 roles"); return false; }
    const Retarget::RigDesc source = canonicalRig(m.restDir);
    std::vector<unsigned short> handles;
    const Retarget::RigDesc target = Retarget::describeSkeleton(skel, &handles);
    Retarget::AutoMapReport mapped = Retarget::autoMap(source.names, target.names);
    // The canonical foot role is no foot: in the training data it points along the shin (77 degrees down when
    // standing), so driving the rig's foot from it stands the character on tiptoe. Unmapped, the foot keeps its bind
    // angle to the shin (flat when standing).
    auto& pairs = mapped.map.pairs;
    pairs.erase(std::remove_if(pairs.begin(), pairs.end(),
                               [](const Retarget::BonePair& p) { return p.source == "rfoot" || p.source == "lfoot"; }),
                pairs.end());
    if (mapped.map.pairs.size() < 8) {
        if (error) *error = QStringLiteral("only %1 bones matched the humanoid roles: not a humanoid rig").arg(mapped.map.pairs.size());
        return false;
    }
    if (qEnvironmentVariableIsSet("QTMESH_RETARGET_DEBUG"))
        for (const auto& p : mapped.map.pairs) fprintf(stderr, "[t2m-map] %s -> %s\n", p.source.c_str(), p.target.c_str());
    Retarget::Options options;
    options.translation = Retarget::TranslationMode::None;
    options.sourceRest = Retarget::SourceRest::Bind;
    for (const std::string& name : target.names)
        if (const int role = MotionInbetween::canonicalIndexForBone(QString::fromStdString(name)); role >= 0 && role <= 5)
            options.noAlignTargets.push_back(name);   // spine/neck/head: rest curvature is a rig convention, not a pose
    Retarget::RetargetReport report;
    const std::vector<Retarget::LocalPose> posed =
        Retarget::retargetFrames(source, canonicalLocalPoses(source, m.quats), target, mapped.map, options, &report);
    if (posed.empty()) { if (error) *error = report.error; return false; }
    std::vector<char> animated(size_t(target.size()), 0);
    for (const auto& p : mapped.map.pairs)
        if (const int t = target.indexOf(p.target); t >= 0) animated[size_t(t)] = 1;
    std::vector<float> times(m.quats.size());
    for (size_t i = 0; i < times.size(); ++i) times[i] = float(i) / float(m.fps > 0 ? m.fps : 30);
    return Retarget::writeRetargetedClip(skel, clip, times, posed, target, handles, animated, error);
}

Ogre::Bone* firstBoneWithRole(Ogre::Skeleton* skel, int role)
{
    for (unsigned short i = 0; i < skel->getNumBones(); ++i)
        if (MotionInbetween::canonicalIndexForBone(QString::fromStdString(skel->getBone(i)->getName())) == role)
            return skel->getBone(i);
    return nullptr;
}

} // namespace

bool addFootDrivenRootMotion(Ogre::Skeleton* skel, const std::string& clip, QString* error)
{
    const auto fail = [error](const QString& m) {
        if (error) *error = m;
        return false;
    };
    if (!skel || !skel->hasAnimation(clip)) return fail(QStringLiteral("animation not found"));
    Ogre::Bone* hips = firstBoneWithRole(skel, 0);
    Ogre::Bone* head = firstBoneWithRole(skel, 5);
    Ogre::Bone* feet[2] = {firstBoneWithRole(skel, 17), firstBoneWithRole(skel, 21)};
    if (!hips || !head || !feet[0] || !feet[1]) return fail(QStringLiteral("the rig has no hips/head/feet roles"));
    Ogre::Animation* anim = skel->getAnimation(clip);
    if (!anim->hasNodeTrack(hips->getHandle())) return fail(QStringLiteral("the clip does not animate the hips"));
    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(hips->getHandle());
    const int nk = int(track->getNumKeyFrames());
    if (nk < 2) return fail(QStringLiteral("too few keyframes"));

    // Up = hips -> head in the bind pose (rig axes vary); the travel lives in the plane across it.
    skel->reset(true);
    Ogre::Vector3 up = head->_getDerivedPosition() - hips->_getDerivedPosition();
    if (up.squaredLength() < 1e-12f) return fail(QStringLiteral("degenerate bind pose"));
    up.normalise();
    const Ogre::Node* parent = hips->getParent();
    const Ogre::Quaternion parentRot = parent ? parent->_getDerivedOrientation() : Ogre::Quaternion::IDENTITY;
    const Ogre::Vector3 parentScale = parent ? parent->_getDerivedScale() : Ogre::Vector3::UNIT_SCALE;
    const float legLength = (hips->_getDerivedPosition() - feet[0]->_getDerivedPosition()).length();

    // Sample the feet (root locked) at every hips keyframe.
    std::vector<std::array<Ogre::Vector3, 2>> foot(static_cast<size_t>(nk));
    for (int k = 0; k < nk; ++k) {
        skel->reset(true);
        anim->apply(skel, track->getNodeKeyFrame(static_cast<unsigned short>(k))->getTime(), 1.0f, 1.0f);
        for (int f = 0; f < 2; ++f) foot[size_t(k)][size_t(f)] = feet[f]->_getDerivedPosition();
    }
    skel->reset(true);

    // The lower foot is planted: the body moves by the opposite of its horizontal slide.
    const auto flat = [&up](const Ogre::Vector3& v) { return v - up * v.dotProduct(up); };
    Ogre::Vector3 offset = Ogre::Vector3::ZERO;
    for (int k = 1; k < nk; ++k) {
        const auto& a = foot[size_t(k - 1)];
        const auto& b = foot[size_t(k)];
        const int stance = b[0].dotProduct(up) <= b[1].dotProduct(up) ? 0 : 1;
        const Ogre::Vector3 slide = flat(b[size_t(stance)] - a[size_t(stance)]);
        if (slide.length() < 0.35f * legLength) offset -= slide;   // a larger jump is a stance switch artefact (a run slides ~0.11)
        Ogre::TransformKeyFrame* kf = track->getNodeKeyFrame(static_cast<unsigned short>(k));
        Ogre::Vector3 local = parentRot.Inverse() * offset;
        local /= parentScale;
        kf->setTranslate(kf->getTranslate() + local);
    }
    track->_keyFrameDataChanged();
    if (qEnvironmentVariableIsSet("QTMESH_RETARGET_DEBUG"))
        fprintf(stderr, "[t2m-root] %s: travel %.3f over %d keys (leg length %.3f, rig units)\n", clip.c_str(),
                double(offset.length()), nk, double(legLength));
    return true;
}

SceneApplyResult applyCanonicalMotionsToEntity(Ogre::Entity* entity, const std::vector<CanonicalMotion>& motions,
                                               const SceneApplyOptions& options)
{
    SceneApplyResult r;
    if (!entity || !entity->hasSkeleton()) { r.error = QStringLiteral("no rigged entity: generated motion needs a skeleton"); return r; }
    if (motions.empty()) { r.error = QStringLiteral("no motion to apply"); return r; }
    Ogre::Skeleton* skel = entity->getMesh()->getSkeleton().get();
    const QString base = options.baseName.trimmed().isEmpty() ? QStringLiteral("generated_qtmesh_t2m") : options.baseName.trimmed();
    for (size_t v = 0; v < motions.size(); ++v) {
        const CanonicalMotion& m = motions[v];
        const std::string clip = Retarget::uniqueAnimationName(skel, variantClipName(base, int(v)).toStdString());
        if (QString err; !retargetCanonical(skel, clip, m, &err)) {
            if (skel->hasAnimation(clip)) skel->removeAnimation(clip);
            for (const QString& c : r.clips) skel->removeAnimation(c.toStdString());
            r.clips.clear();
            r.error = motions.size() > 1 ? QStringLiteral("variation %1: %2").arg(v + 1).arg(err) : err;
            return r;
        }
        if (options.rootMotion)
            if (QString err; !addFootDrivenRootMotion(skel, clip, &err))
                qWarning("QtMesh T2M root motion skipped on %s: %s", clip.c_str(), qPrintable(err));
        if (options.smoothFps > 0) AnimationMerger::smoothBakeAnimation(skel, clip, options.smoothFps, m.fps);   // removes trembling
        if (options.footPin) AnimationMerger::pinFeet(skel, clip);
        r.clips << QString::fromStdString(clip);
    }
    entity->refreshAvailableAnimationState();

    if (options.undoable)
        if (auto* um = UndoManager::getSingleton()) {
            um->stack()->beginMacro(QStringLiteral("QtMesh T2M motion (%1 clip%2)").arg(r.clips.size()).arg(r.clips.size() == 1 ? "" : "s"));
            for (const QString& c : r.clips) um->push(new RetargetAnimationCommand(entity->getName(), c.toStdString(), true));
            um->stack()->endMacro();
            r.undoIndexAfter = um->stack()->index();
        }
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.qtmesh_t2m"),
                                  QStringLiteral("applied %1 clip(s) to %2").arg(r.clips.size()).arg(QString::fromStdString(entity->getName())));
    r.ok = true;
    return r;
}

void rollbackAppliedMotions(Ogre::Entity* entity, const SceneApplyResult& applied)
{
    if (!entity || !entity->hasSkeleton() || applied.clips.isEmpty()) return;
    auto* um = UndoManager::getSingleton();
    if (um && applied.undoIndexAfter >= 0 && um->stack()->index() == applied.undoIndexAfter && um->stack()->canUndo()) {
        um->stack()->undo();   // the batch is still the newest step: undoing it removes every clip
        return;
    }
    Ogre::Skeleton* skel = entity->getSkeleton();
    Ogre::AnimationStateSet* states = entity->getAllAnimationStates();
    for (const QString& c : applied.clips) {
        const std::string name = c.toStdString();
        // refreshAvailableAnimationState only ADDS states: drop the clip's own, as RetargetAnimationCommand::undo does.
        if (states && states->hasAnimationState(name)) states->removeAnimationState(name);
        if (skel->hasAnimation(name)) skel->removeAnimation(name);
    }
}

} // namespace AIMotion
