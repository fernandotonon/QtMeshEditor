#include "SamBodyRetarget.h"

#include "SamBodyMotion.h"
#include "../MotionInbetween.h"

#include <OgreAnimation.h>
#include <OgreSkeleton.h>

#include <QtGlobal>

#include <algorithm>
#include <cstdio>

namespace SamBody {

bool excludedFromMapping(const std::string& n);

namespace {
Ogre::Quaternion quatXyzw(const std::vector<float>& q, int i)
{
    Ogre::Quaternion r(q[size_t(i) * 4 + 3], q[size_t(i) * 4], q[size_t(i) * 4 + 1], q[size_t(i) * 4 + 2]);
    r.normalise();
    return r;
}
} // namespace

Retarget::RigDesc mhrRigDesc(const std::vector<float>& restPos, const std::vector<float>& restQuat)
{
    Retarget::RigDesc rig;
    const int n = 127;
    if (restPos.size() != size_t(n) * 3 || restQuat.size() != size_t(n) * 4) return rig;
    const auto& names = mhrJointNames();
    const auto& parents = mhrJointParents();

    // Twist / corrective / face helpers are dropped and their children re-parented to the nearest kept ancestor: the
    // retargeter aims each bone at its child, and a forearm with five twist joints hanging off it picked the wrong one
    // (measured: 94-160 degree constant offsets on the arms). Local transforms are recomputed against the kept parent.
    std::vector<int> keptIndex(size_t(n), -1);                      // full index -> rig index
    std::vector<int> keptParent(size_t(n), -1);                     // full index -> nearest kept ancestor (full index)
    for (int i = 0; i < n; ++i) {
        int p = parents[size_t(i)];
        while (p >= 0 && excludedFromMapping(names[size_t(p)])) p = parents[size_t(p)];
        keptParent[size_t(i)] = p;
    }
    auto vec = [&](int i) { return Ogre::Vector3(restPos[size_t(i) * 3], restPos[size_t(i) * 3 + 1], restPos[size_t(i) * 3 + 2]); };
    for (int i = 0; i < n; ++i) {
        if (excludedFromMapping(names[size_t(i)])) continue;
        keptIndex[size_t(i)] = rig.size();
        rig.names.emplace_back(names[size_t(i)]);
        const int par = keptParent[size_t(i)];
        rig.parent.push_back(par < 0 ? -1 : keptIndex[size_t(par)]);
        const Ogre::Quaternion q = quatXyzw(restQuat, i);
        if (par < 0) {
            rig.bindPos.push_back(vec(i));
            rig.bindRot.push_back(q);
        } else {
            const Ogre::Quaternion pq = quatXyzw(restQuat, par);
            rig.bindPos.push_back(pq.Inverse() * (vec(i) - vec(par)));
            Ogre::Quaternion l = pq.Inverse() * q;
            l.normalise();
            rig.bindRot.push_back(l);
        }
    }
    return rig;
}

Retarget::LocalPose mhrLocalPose(const Retarget::RigDesc& rig, const std::vector<float>& jointQuat)
{
    Retarget::LocalPose pose = Retarget::bindPose(rig);
    if (rig.size() == 0 || jointQuat.size() != size_t(127) * 4) return pose;
    std::vector<int> full(size_t(rig.size()), -1);
    for (int i = 0; i < rig.size(); ++i) full[size_t(i)] = mhrJointIndex(rig.names[size_t(i)].c_str());
    for (int i = 0; i < rig.size(); ++i) {
        if (full[size_t(i)] < 0) continue;
        const Ogre::Quaternion q = quatXyzw(jointQuat, full[size_t(i)]);
        const int par = rig.parent[size_t(i)];
        if (par < 0 || full[size_t(par)] < 0) { pose.rot[size_t(i)] = q; continue; }
        Ogre::Quaternion l = quatXyzw(jointQuat, full[size_t(par)]).Inverse() * q;
        l.normalise();
        pose.rot[size_t(i)] = l;
    }
    return pose;
}

std::string mhrAlias(const std::string& n)
{
    static const std::pair<const char*, const char*> fixed[] = {
        {"root", "Hips"}, {"c_spine1", "Spine"}, {"c_spine2", "Spine1"}, {"c_spine3", "Spine2"},   // spine0 stays out: Mixamo has 3 spine bones, the neck/clavicles hang off spine3
        {"c_neck", "Neck"}, {"c_head", "Head"},
    };
    for (const auto& f : fixed) if (n == f.first) return f.second;
    if (n.size() < 3 || (n[0] != 'l' && n[0] != 'r') || n[1] != '_') return n;
    const std::string side = n[0] == 'l' ? "Left" : "Right";
    const std::string part = n.substr(2);
    static const std::pair<const char*, const char*> limbs[] = {
        {"clavicle", "Shoulder"}, {"uparm", "Arm"}, {"lowarm", "ForeArm"}, {"wrist", "Hand"},
        {"upleg", "UpLeg"}, {"lowleg", "Leg"}, {"foot", "Foot"}, {"ball", "ToeBase"},
    };
    for (const auto& l : limbs) if (part == l.first) return side + l.second;
    // fingers: MHR pinky0..3 / ring1..3 / ... -> <Side>Hand<Finger><1..3>; the metacarpal-like joint 0 stays out
    // (Mixamo's thumb starts at the wrist, so thumb1..3 take Thumb1..3)
    static const std::pair<const char*, const char*> fingers[] = {
        {"thumb", "Thumb"}, {"index", "Index"}, {"middle", "Middle"}, {"ring", "Ring"}, {"pinky", "Pinky"},
    };
    for (const auto& f : fingers) {
        const std::string base = f.first;
        if (part.size() == base.size() + 1 && part.compare(0, base.size(), base) == 0 && part.back() >= '1' && part.back() <= '3')
            return side + "Hand" + f.second + part.back();
    }
    return n;
}

Ogre::Quaternion uprightCorrection(const std::vector<std::vector<float>>& frames)
{
    const int root = mhrJointIndex("root"), neck = mhrJointIndex("c_neck");
    Ogre::Vector3 sum = Ogre::Vector3::ZERO;
    int n = 0;
    for (const auto& f : frames) {
        if (f.size() != size_t(127) * 3) continue;
        Ogre::Vector3 a(f[size_t(neck) * 3] - f[size_t(root) * 3], f[size_t(neck) * 3 + 1] - f[size_t(root) * 3 + 1],
                        f[size_t(neck) * 3 + 2] - f[size_t(root) * 3 + 2]);
        if (a.length() < 1e-6f) continue;
        a.normalise();
        sum += a;
        ++n;
    }
    if (n == 0 || sum.length() < 1e-6f) return Ogre::Quaternion::IDENTITY;
    sum.normalise();
    // Shortest arc sum -> +Y, but only its horizontal part: project the rotation axis onto the ground plane (axis.y = 0) so
    // the heading is not changed. The axis of the arc is sum x Y, which is already horizontal.
    Ogre::Quaternion c = sum.getRotationTo(Ogre::Vector3::UNIT_Y);
    c.normalise();
    return c;
}

void applyWorldRotation(std::vector<float>& q, const Ogre::Quaternion& c)
{
    for (size_t i = 0; i + 3 < q.size(); i += 4) {
        Ogre::Quaternion r(q[i + 3], q[i], q[i + 1], q[i + 2]);
        Ogre::Quaternion o = c * r;
        o.normalise();
        q[i] = o.x; q[i + 1] = o.y; q[i + 2] = o.z; q[i + 3] = o.w;
    }
}

bool excludedFromMapping(const std::string& n)
{
    for (const char* bad : {"twist", "_proc", "_null", "body_world", "jaw", "teeth", "tongue", "eye"})
        if (n.find(bad) != std::string::npos) return true;
    return false;
}

RetargetOutcome retargetToSkeleton(Ogre::Skeleton* skeleton, const Retarget::RigDesc& mhr,
                                   const std::vector<Retarget::LocalPose>& frames, int fps, const std::string& clip,
                                   bool replace)
{
    RetargetOutcome out;
    if (!skeleton) { out.error = QStringLiteral("missing skeleton"); return out; }
    if (frames.size() < 2 || fps <= 0) { out.error = QStringLiteral("need at least 2 pose frames"); return out; }
    if (mhr.size() < 6) { out.error = QStringLiteral("the MHR rest skeleton is not available"); return out; }
    if (skeleton->hasAnimation(clip) && !replace) {
        out.error = QStringLiteral("animation '%1' already exists").arg(QString::fromStdString(clip));
        return out;
    }

    std::vector<unsigned short> handles;
    const Retarget::RigDesc target = Retarget::describeSkeleton(skeleton, &handles);

    // The role matcher knows the common spellings but not MHR's (root, lowarm, spine0..3, pinky0): hand it standard aliases.
    Retarget::RigDesc aliased = mhr;
    for (std::string& n : aliased.names) n = mhrAlias(n);
    const Retarget::AutoMapReport mapped = Retarget::autoMap(aliased.names, target.names);
    if (mapped.map.pairs.size() < 6) {
        out.error = QStringLiteral("only %1 bones of the rig could be matched to the MHR skeleton: not a humanoid rig")
                        .arg(mapped.map.pairs.size());
        return out;
    }

    if (qEnvironmentVariableIsSet("QTMESH_RETARGET_DEBUG"))
        for (const auto& p : mapped.map.pairs)
            fprintf(stderr, "[sam-map] %s -> %s\n", p.source.c_str(), p.target.c_str());
    Retarget::Options options;
    options.translation = Retarget::TranslationMode::None;
    options.alignDirections = !qEnvironmentVariableIsSet("QTMESH_SAM_NO_ALIGN");
    // Spine, neck, head: orientation transport only (see Options::noAlignTargets). Role 0..5 = hips, abdomen, chest, neck, neck1, head.
    for (const std::string& name : target.names)
        if (const int role = MotionInbetween::canonicalIndexForBone(QString::fromStdString(name)); role >= 0 && role <= 5)
            options.noAlignTargets.push_back(name);
    options.sourceRest = Retarget::SourceRest::Bind;
    const std::vector<Retarget::LocalPose> posed =
        Retarget::retargetFrames(aliased, frames, target, mapped.map, options, &out.report);
    if (posed.empty()) { out.error = out.report.error; return out; }

    std::vector<char> animated(size_t(target.size()), 0);
    for (const auto& p : mapped.map.pairs) {
        const int t = target.indexOf(p.target);
        if (t < 0) continue;
        animated[size_t(t)] = 1;
        ++out.mappedBones;
        if (QString::fromStdString(p.target).contains(QLatin1String("finger"), Qt::CaseInsensitive)
            || QString::fromStdString(p.target).contains(QLatin1String("thumb"), Qt::CaseInsensitive)
            || QString::fromStdString(p.target).contains(QLatin1String("index"), Qt::CaseInsensitive)
            || QString::fromStdString(p.target).contains(QLatin1String("pinky"), Qt::CaseInsensitive))
            ++out.fingerBones;
    }
    for (int i = 0; i < target.size(); ++i)
        if (!animated[size_t(i)]) out.unmappedTarget << QString::fromStdString(target.names[size_t(i)]);

    std::vector<float> times(posed.size());
    for (size_t i = 0; i < times.size(); ++i) times[i] = static_cast<float>(i) / static_cast<float>(fps);
    if (skeleton->hasAnimation(clip)) skeleton->removeAnimation(clip);   // only now that the take is known good
    if (!Retarget::writeRetargetedClip(skeleton, clip, times, posed, target, handles, animated, &out.error)) return out;
    out.ok = true;
    return out;
}

} // namespace SamBody
