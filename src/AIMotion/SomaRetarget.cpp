#include "SomaRetarget.h"

#include "SomaSkeleton.h"
#include "../MotionInbetween.h"

#include <OgreSkeleton.h>

#include <QtGlobal>

#include <algorithm>
#include <array>
#include <cstdio>
#include <unordered_map>

namespace AIMotion {

namespace {

bool isFingerName(const QString& n)
{
    for (const char* f : {"thumb", "index", "middle", "ring", "pinky", "finger"})
        if (n.contains(QLatin1String(f), Qt::CaseInsensitive)) return true;
    return false;
}

// Bind-pose world positions of a parents-first RigDesc.
std::vector<Ogre::Vector3> worldPositions(const Retarget::RigDesc& rig)
{
    std::vector<Ogre::Vector3> pos(size_t(rig.size()));
    std::vector<Ogre::Quaternion> rot(size_t(rig.size()));
    for (int i = 0; i < rig.size(); ++i) {
        const int p = rig.parent[size_t(i)];
        if (p < 0) {
            pos[size_t(i)] = rig.bindPos[size_t(i)];
            rot[size_t(i)] = rig.bindRot[size_t(i)];
        } else {
            pos[size_t(i)] = pos[size_t(p)] + rot[size_t(p)] * rig.bindPos[size_t(i)];
            rot[size_t(i)] = rot[size_t(p)] * rig.bindRot[size_t(i)];
        }
    }
    return pos;
}

std::vector<int> childrenOf(const Retarget::RigDesc& rig, int bone)
{
    std::vector<int> kids;
    for (int i = 0; i < rig.size(); ++i)
        if (rig.parent[size_t(i)] == bone) kids.push_back(i);
    return kids;
}

// The single-child path starting at `root`; empty when it branches.
std::vector<int> fingerChain(const Retarget::RigDesc& rig, int root)
{
    std::vector<int> chain{root};
    for (std::vector<int> kids = childrenOf(rig, root); !kids.empty(); kids = childrenOf(rig, chain.back())) {
        if (kids.size() != 1) return {};
        chain.push_back(kids.front());
    }
    return chain;
}

// Five chains under one hand, thumb first; empty with `why` set when the hand does not have that shape.
std::array<std::vector<int>, 5> handChains(const Retarget::RigDesc& rig, const std::vector<Ogre::Vector3>& world,
                                           int hand, QString* why)
{
    int branch = hand;
    std::vector<int> kids = childrenOf(rig, branch);
    for (int skip = 0; skip < 2 && kids.size() == 1; ++skip) {   // palm joints between the hand and the fingers
        branch = kids.front();
        kids = childrenOf(rig, branch);
    }
    if (kids.size() != 5) {
        *why = QStringLiteral("%1 child chains, expected 5").arg(kids.size());
        return {};
    }
    struct Finger { std::vector<int> chain; float along = 0.0f; };
    std::vector<Finger> fingers;
    // Along the arm: forearm -> hand when the hand IS the branch, else hand -> palm joint.
    const int from = branch == hand ? rig.parent[size_t(hand)] : hand;
    if (from < 0) { *why = QStringLiteral("the hand has no parent"); return {}; }
    const Ogre::Vector3 axis = (world[size_t(branch)] - world[size_t(from)]).normalisedCopy();
    for (int k : kids) {
        std::vector<int> c = fingerChain(rig, k);
        if (c.size() < 2 || c.size() > 4) {
            *why = QStringLiteral("a finger chain has %1 joints (or branches)").arg(c.size());
            return {};
        }
        fingers.push_back({c, (world[size_t(k)] - world[size_t(branch)]).dotProduct(axis)});
    }
    std::sort(fingers.begin(), fingers.end(), [](const Finger& a, const Finger& b) { return a.along < b.along; });
    // The thumb has to stand out clearly, else thumb vs index would be a guess.
    if (!(fingers[1].along > 0.0f && fingers[0].along < 0.7f * fingers[1].along)) {
        *why = QStringLiteral("no chain is rooted clearly nearer the wrist (cannot tell the thumb)");
        return {};
    }
    const Ogre::Vector3 thumbRoot = world[size_t(fingers[0].chain.front())];
    std::sort(fingers.begin() + 1, fingers.end(), [&](const Finger& a, const Finger& b) {
        return world[size_t(a.chain.front())].squaredDistance(thumbRoot) < world[size_t(b.chain.front())].squaredDistance(thumbRoot);
    });
    std::array<std::vector<int>, 5> out;
    for (size_t i = 0; i < 5; ++i) out[i] = fingers[i].chain;
    return out;
}

} // namespace

void freezeHorizontalRoot(Motion* m)
{
    if (!m || m->rootPos.empty()) return;
    // Pin x/z to the REST root (what the retargeter measures the root delta from), keep y: a jump still lifts and a
    // sit still lowers. Pinning to the first frame instead left its offset from the rest on every frame.
    const Ogre::Vector3 rest = m->restJoints.empty() ? m->rootPos.front() : m->restJoints.front();
    for (Ogre::Vector3& p : m->rootPos) {
        p.x = rest.x;
        p.z = rest.z;
    }
}

namespace {

const std::array<const char*, 5> kFingers = {"Thumb", "Index", "Middle", "Ring", "Pinky"};

// The normal name match already found this hand's fingers.
bool handHasNamedFingers(const Retarget::BoneMap& map, const std::string& side)
{
    return std::any_of(kFingers.begin(), kFingers.end(), [&](const char* f) {
        return !map.targetFor(side + "Hand" + f + (std::string(f) == "Thumb" ? "1" : "2")).empty();
    });
}

// SOMA -> rig pairs for one hand's five chains (thumb first). `clash` = a target joint is already driven.
std::vector<Retarget::BonePair> fingerPairs(const Motion& m, const Retarget::RigDesc& target, const std::string& side,
                                            const std::array<std::vector<int>, 5>& chains, const std::vector<char>& used,
                                            bool* clash)
{
    std::vector<Retarget::BonePair> pairs;
    for (size_t f = 0; f < chains.size(); ++f) {
        const std::vector<int>& c = chains[f];
        const size_t first = c.size() > 3 ? c.size() - 3 : 0;   // a 4-joint chain starts with a metacarpal
        for (size_t j = first; j < c.size(); ++j) {
            const int phalanx = int(j - first) + 1;   // 1..3 in standard numbering
            // SOMA numbers the long fingers from the metacarpal (Index2 = proximal phalanx), the thumb 1:1.
            const std::string src = side + "Hand" + kFingers[f] + std::to_string(f == 0 ? phalanx : phalanx + 1);
            if (m.indexOf(src) < 0) continue;
            *clash = *clash || used[size_t(c[j])];
            pairs.push_back({src, target.names[size_t(c[j])]});
        }
    }
    return pairs;
}

} // namespace

QStringList addUnnamedFingerChains(const Motion& m, const Retarget::RigDesc& target, Retarget::BoneMap* map,
                                   QStringList* notes)
{
    QStringList added;
    const std::vector<Ogre::Vector3> world = worldPositions(target);
    std::vector<char> used(size_t(target.size()), 0);
    for (const auto& p : map->pairs)
        if (const int t = target.indexOf(p.target); t >= 0) used[size_t(t)] = 1;
    const auto skip = [notes](const std::string& side, const QString& why) {
        if (notes) *notes << QStringLiteral("%1 fingers not inferred: %2").arg(QString::fromStdString(side), why);
    };

    for (const std::string side : {"Left", "Right"}) {
        const int hand = target.indexOf(map->targetFor(side + "Hand"));
        if (hand < 0 || handHasNamedFingers(*map, side)) continue;
        QString why;
        const std::array<std::vector<int>, 5> chains = handChains(target, world, hand, &why);
        if (chains[0].empty()) {
            skip(side, why);
            continue;
        }
        bool clash = false;
        const std::vector<Retarget::BonePair> pairs = fingerPairs(m, target, side, chains, used, &clash);
        if (clash) {
            skip(side, QStringLiteral("a finger joint is already driven"));
            continue;
        }
        for (const auto& p : pairs) {
            map->pairs.push_back(p);
            used[size_t(target.indexOf(p.target))] = 1;
            added << QStringLiteral("%1 -> %2").arg(QString::fromStdString(p.source), QString::fromStdString(p.target));
        }
    }
    return added;
}

Retarget::BoneMap somaBoneMap(const Motion& m, const std::vector<std::string>& targetNames,
                              QStringList* unmappedSource, QStringList* excludedSource)
{
    // Only joints with a standard counterpart take part in the auto-map: anything else would be fair game for its fuzzy pass.
    std::vector<std::string> standard;
    std::unordered_map<std::string, std::string> back;   // standard -> SOMA
    for (const std::string& n : m.names) {
        const SomaRole role = somaJointRole(n);
        if (role != SomaRole::Mapped) {
            if (excludedSource) *excludedSource << QStringLiteral("%1 (%2)").arg(QString::fromStdString(n), QLatin1String(somaRoleId(role)));
            continue;
        }
        const std::string s = somaStandardName(n);
        standard.push_back(s);
        back[s] = n;
    }
    const Retarget::AutoMapReport mapped = Retarget::autoMap(standard, targetNames);
    Retarget::BoneMap map;
    map.name = QString::fromLatin1(kSomaMapVersion);
    for (const auto& p : mapped.map.pairs) map.pairs.push_back({back[p.source], p.target});
    if (unmappedSource)
        for (const std::string& s : mapped.unmappedSource) *unmappedSource << QString::fromStdString(back[s]);
    return map;
}

namespace {

Retarget::Options somaRetargetOptions(const Retarget::RigDesc& target)
{
    Retarget::Options ro;
    // In-place clips still carry the root's HEIGHT (a jump lifts, a sit lowers): freezeHorizontalRoot already removed
    // the travel, so the root translation is always retargeted. TranslationMode::None would drop the height too.
    ro.translation = Retarget::TranslationMode::Root;
    ro.alignDirections = true;   // SOMA rest is a T-pose; A-pose rigs need the per-limb alignment
    // Spine/neck/head and feet keep plain orientation transport: their rest curvature / ankle pitch is a rig convention, not a
    // pose (the same rule as the SAM 3D Body path, measured there: 25-38 degrees of torso bend otherwise).
    for (const std::string& name : target.names) {
        const int role = MotionInbetween::canonicalIndexForBone(QString::fromStdString(name));
        const QString lower = QString::fromStdString(name).toLower();
        if ((role >= 0 && role <= 5) || lower.contains(QLatin1String("foot")) || lower.contains(QLatin1String("toe")))
            ro.noAlignTargets.push_back(name);
    }
    ro.sourceRest = Retarget::SourceRest::Bind;
    return ro;
}

// Target bones the clip drives (mapped bones + the root, which carries translation even when its rotation is
// unmapped); fills the counts and the undriven list of `out`.
std::vector<char> drivenBones(const Retarget::BoneMap& map, const Retarget::RigDesc& target, ApplyOutcome* out)
{
    std::vector<char> animated(size_t(target.size()), 0);
    for (const auto& p : map.pairs) {
        const int t = target.indexOf(p.target);
        if (t < 0) continue;
        animated[size_t(t)] = 1;
        ++out->mappedBones;
        if (isFingerName(QString::fromStdString(p.source))) ++out->fingerBones;   // SOMA names: also counts unnamed rig fingers
    }
    if (const int r = out->report.rootTarget.empty() ? -1 : target.indexOf(out->report.rootTarget); r >= 0)
        animated[size_t(r)] = 1;
    for (int i = 0; i < target.size(); ++i)
        if (!animated[size_t(i)]) out->unmappedTarget << QString::fromStdString(target.names[size_t(i)]);
    return animated;
}

} // namespace

ApplyOutcome applyMotionToSkeleton(Ogre::Skeleton* skeleton, const Motion& motion, const std::string& clip,
                                   const ApplyOptions& options)
{
    ApplyOutcome out;
    out.clip = clip;
    out.mapVersion = QString::fromLatin1(kSomaMapVersion);
    if (!skeleton) { out.error = QStringLiteral("the selected mesh has no skeleton"); return out; }
    if (motion.frameCount() < 2) { out.error = QStringLiteral("the motion has fewer than 2 frames"); return out; }
    if (!looksLikeSoma(motion.names)) { out.error = QStringLiteral("the motion's skeleton is not SOMA (unknown joint names)"); return out; }
    if (skeleton->hasAnimation(clip) && !options.replace) {
        out.error = QStringLiteral("animation '%1' already exists").arg(QString::fromStdString(clip));
        return out;
    }

    Motion m = resample(motion, options.fps > 0.0f ? options.fps : motion.fps);
    if (!options.rootMotion) freezeHorizontalRoot(&m);
    std::vector<unsigned short> handles;
    const Retarget::RigDesc target = Retarget::describeSkeleton(skeleton, &handles);
    Retarget::BoneMap map = somaBoneMap(m, target.names, &out.unmappedSource, &out.excludedSource);
    QStringList fingerNotes;
    out.inferredFingers = addUnnamedFingerChains(m, target, &map, &fingerNotes);
    for (const QString& pair : out.inferredFingers) out.unmappedSource.removeAll(pair.section(QLatin1String(" -> "), 0, 0));
    if (qEnvironmentVariableIsSet("QTMESH_RETARGET_DEBUG"))
        for (const QString& n : fingerNotes) fprintf(stderr, "[soma-map] %s\n", qPrintable(n));
    if (map.pairs.size() < 6) {
        out.error = QStringLiteral("only %1 bones of the rig could be matched to the SOMA skeleton: not a humanoid rig").arg(map.pairs.size());
        return out;
    }
    if (qEnvironmentVariableIsSet("QTMESH_RETARGET_DEBUG"))
        for (const auto& p : map.pairs) fprintf(stderr, "[soma-map] %s -> %s\n", p.source.c_str(), p.target.c_str());

    const Retarget::Options ro = somaRetargetOptions(target);

    const std::vector<Retarget::LocalPose> posed =
        Retarget::retargetFrames(rigDesc(m), localPoses(m), target, map, ro, &out.report);
    if (posed.empty()) { out.error = out.report.error; return out; }

    const std::vector<char> animated = drivenBones(map, target, &out);

    std::vector<float> times(posed.size());
    for (size_t i = 0; i < times.size(); ++i) times[i] = float(i) / m.fps;
    if (skeleton->hasAnimation(clip)) skeleton->removeAnimation(clip);   // only now that the take is known good
    if (!Retarget::writeRetargetedClip(skeleton, clip, times, posed, target, handles, animated, &out.error)) return out;
    out.frames = int(posed.size());
    out.length = times.back();
    out.ok = true;
    return out;
}

} // namespace AIMotion
