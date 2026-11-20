#include "SamBodyRetarget.h"

#include "SamBodyMotion.h"
#include "../MotionInbetween.h"

#include <OgreAnimation.h>
#include <OgreSkeleton.h>

#include <QtGlobal>

#include <algorithm>
#include <cmath>
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

bool excludedFromMapping(const std::string& n)
{
    for (const char* bad : {"twist", "_proc", "_null", "body_world", "jaw", "teeth", "tongue", "eye"})
        if (n.find(bad) != std::string::npos) return true;
    return false;
}

RetargetOutcome retargetToSkeleton(Ogre::Skeleton* skeleton, const Retarget::RigDesc& mhr,
                                   const std::vector<Retarget::LocalPose>& frames, const std::vector<float>& times,
                                   const std::string& clip, bool replace, bool rootMotion)
{
    RetargetOutcome out;
    if (!skeleton) { out.error = QStringLiteral("missing skeleton"); return out; }
    if (frames.size() < 2 || times.size() != frames.size()) { out.error = QStringLiteral("need at least 2 pose frames"); return out; }
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
    options.translation = rootMotion ? Retarget::TranslationMode::Root : Retarget::TranslationMode::None;
    options.alignDirections = !qEnvironmentVariableIsSet("QTMESH_SAM_NO_ALIGN");
    // Spine, neck, head: orientation transport only (see Options::noAlignTargets). Role 0..5 = hips, abdomen, chest, neck, neck1, head.
    for (const std::string& name : target.names)
        if (const int role = MotionInbetween::canonicalIndexForBone(QString::fromStdString(name)); role >= 0 && role <= 5)
            options.noAlignTargets.push_back(name);
    // Feet and toes too: their rest "direction" is ankle -> toe, which depends on the ankle HEIGHT of each rig (Mixamo 35 degrees
    // down, MHR 16), not on how the foot is posed. Aligning those directions pitched a flat foot's toes up by the difference.
    for (const std::string& name : target.names) {
        QString lower = QString::fromStdString(name).toLower();
        if (lower.contains(QLatin1String("foot")) || lower.contains(QLatin1String("toe")))
            options.noAlignTargets.push_back(name);
    }
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

    if (skeleton->hasAnimation(clip)) skeleton->removeAnimation(clip);   // only now that the take is known good
    if (!Retarget::writeRetargetedClip(skeleton, clip, times, posed, target, handles, animated, &out.error)) return out;
    out.ok = true;
    return out;
}

RetargetOutcome retargetToSkeleton(Ogre::Skeleton* skeleton, const Retarget::RigDesc& mhr,
                                   const std::vector<Retarget::LocalPose>& frames, int fps, const std::string& clip,
                                   bool replace, bool rootMotion)
{
    std::vector<float> times(frames.size());
    for (size_t i = 0; i < times.size(); ++i) times[i] = fps > 0 ? static_cast<float>(i) / static_cast<float>(fps) : 0.0f;
    if (fps <= 0) { RetargetOutcome o; o.error = QStringLiteral("need at least 2 pose frames"); return o; }
    return retargetToSkeleton(skeleton, mhr, frames, times, clip, replace, rootMotion);
}

std::vector<Ogre::Vector3> rootTrajectory(const std::vector<std::array<float, 3>>& camT, int radiusXY, int radiusZ)
{
    const size_t n = camT.size();
    std::vector<Ogre::Vector3> raw(n, Ogre::Vector3::ZERO);
    if (n == 0) return raw;
    // MHR = camera with Y, Z negated; non-finite frames hold the previous value
    auto finite = [](const Ogre::Vector3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    Ogre::Vector3 last = Ogre::Vector3::ZERO;
    for (const auto& c : camT)                                 // start from the first finite frame
        if (const Ogre::Vector3 v(c[0], -c[1], -c[2]); finite(v)) { last = v; break; }
    std::vector<char> valid(n, 0);
    for (size_t i = 0; i < n; ++i) {
        const Ogre::Vector3 v(camT[i][0], -camT[i][1], -camT[i][2]);
        if (finite(v)) { last = v; valid[i] = 1; }
        raw[i] = last;
    }
    auto smooth = [&](int axis, int radius) {
        std::vector<float> out(n);
        for (size_t i = 0; i < n; ++i) {
            // the window shrinks symmetrically at the ends, so the first and last frames are not pulled towards the middle
            const int r = std::min<int>(std::max(radius, 0), static_cast<int>(std::min(i, n - 1 - i)));
            double sum = 0.0;
            for (int k = -r; k <= r; ++k) sum += raw[size_t(static_cast<int>(i) + k)][axis];
            out[i] = static_cast<float>(sum / (2 * r + 1));
        }
        return out;
    };
    const auto x = smooth(0, radiusXY), y = smooth(1, radiusXY), z = smooth(2, radiusZ);
    std::vector<Ogre::Vector3> traj(n);
    for (size_t i = 0; i < n; ++i) traj[i] = Ogre::Vector3(x[i] - x[0], y[i] - y[0], z[i] - z[0]);
    // a non-finite frame holds the previous OUTPUT (holding only the input still let the averages move it)
    for (size_t i = 1; i < n; ++i)
        if (!valid[i]) traj[i] = traj[i - 1];
    return traj;
}

RetargetOutcome retargetSequenceToSkeleton(Ogre::Skeleton* skeleton, const MhrSequence& seq, int fps, const std::string& clip,
                                           bool replace, bool rootMotion)
{
    RetargetOutcome out;
    if (seq.jointQuat.size() < 2 || seq.jointQuat.size() != seq.jointPos.size() || seq.times.size() != seq.jointQuat.size()) {
        out.error = QStringLiteral("need at least 2 solved frames");
        return out;
    }
    // Every frame must carry the full MHR skeleton: a short one would be read past its end below (diagnostics) or silently
    // replaced by a bind pose (mhrLocalPose).
    for (size_t f = 0; f < seq.jointQuat.size(); ++f)
        if (seq.jointQuat[f].size() != size_t(127) * 4 || seq.jointPos[f].size() != size_t(127) * 3) {
            out.error = QStringLiteral("solved frame %1 is incomplete").arg(f);
            return out;
        }
    if (qEnvironmentVariableIsSet("QTMESH_SAM_DUMP"))
        for (size_t f = 0; f < seq.jointPos.size(); ++f) {
            auto d = [&](const char* a, const char* b) {
                const auto& p = seq.jointPos[f];
                const int ia = mhrJointIndex(a), ib = mhrJointIndex(b);
                Ogre::Vector3 v(p[size_t(ib) * 3] - p[size_t(ia) * 3], p[size_t(ib) * 3 + 1] - p[size_t(ia) * 3 + 1],
                                p[size_t(ib) * 3 + 2] - p[size_t(ia) * 3 + 2]);
                v.normalise();
                return v;
            };
            const Ogre::Vector3 lu = d("l_uparm", "l_lowarm"), ll = d("l_lowarm", "l_wrist"), ru = d("r_uparm", "r_lowarm"),
                                rl = d("r_lowarm", "r_wrist");
            fprintf(stderr, "[sam-dump] f%zu t=%.2f LU(%.2f %.2f %.2f) LL(%.2f %.2f %.2f) RU(%.2f %.2f %.2f) RL(%.2f %.2f %.2f)\n", f,
                    seq.times[f], lu.x, lu.y, lu.z, ll.x, ll.y, ll.z, ru.x, ru.y, ru.z, rl.x, rl.y, rl.z);
        }
    const Retarget::RigDesc mhr = mhrRigDesc(seq.restPos, seq.restQuat);
    if (mhr.size() == 0) { out.error = QStringLiteral("the MHR rest skeleton is not available"); return out; }
    std::vector<Retarget::LocalPose> frames;
    frames.reserve(seq.jointQuat.size());
    for (const auto& q : seq.jointQuat) frames.push_back(mhrLocalPose(mhr, q));
    // Root travel: the source root (the rig's parentless joint) carries the offset; the retargeter's Root mode moves the target's
    // root-most mapped bone by it, scaled by the height ratio.
    const bool travel = rootMotion && seq.camT.size() == frames.size();
    if (travel) {
        const std::vector<Ogre::Vector3> traj = rootTrajectory(seq.camT);
        for (int b = 0; b < mhr.size(); ++b) {
            if (mhr.parent[size_t(b)] >= 0) continue;
            for (size_t f = 0; f < frames.size(); ++f) frames[f].pos[size_t(b)] = mhr.bindPos[size_t(b)] + traj[f];
        }
        if (qEnvironmentVariableIsSet("QTMESH_SAM_DUMP"))
            for (size_t f = 0; f < traj.size(); ++f)
                fprintf(stderr, "[sam-root] f%zu camT(%.3f %.3f %.3f) offset(%.3f %.3f %.3f)\n", f, seq.camT[f][0], seq.camT[f][1],
                        seq.camT[f][2], traj[f].x, traj[f].y, traj[f].z);
    }
    if (fps > 0) return retargetToSkeleton(skeleton, mhr, frames, fps, clip, replace, travel);
    std::vector<float> times(seq.times.begin(), seq.times.end());
    const float t0 = times.front();
    for (float& t : times) t -= t0;                                   // the clip starts at 0
    return retargetToSkeleton(skeleton, mhr, frames, times, clip, replace, travel);
}

} // namespace SamBody
