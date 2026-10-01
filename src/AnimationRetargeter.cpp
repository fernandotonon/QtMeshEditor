/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "AnimationRetargeter.h"

#include "MotionInbetween.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

#include <OgreAnimation.h>
#include <OgreAnimationTrack.h>
#include <OgreBone.h>
#include <OgreKeyFrame.h>
#include <OgreSkeleton.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <map>
#include <set>

namespace Retarget {

// ===========================================================================
// RigDesc / BoneMap basics
// ===========================================================================

int RigDesc::indexOf(const std::string& name) const
{
    for (int i = 0; i < size(); ++i)
        if (names[size_t(i)] == name) return i;
    return -1;
}

std::string BoneMap::targetFor(const std::string& source) const
{
    for (const auto& p : pairs)
        if (p.source == source) return p.target;
    return {};
}

void BoneMap::setPair(const std::string& source, const std::string& target)
{
    // one-to-one: drop any existing pair for this source, and any other
    // source currently claiming this target.
    pairs.erase(std::remove_if(pairs.begin(), pairs.end(), [&](const BonePair& p) {
                    return p.source == source || (!target.empty() && p.target == target);
                }),
                pairs.end());
    if (!target.empty()) pairs.push_back({source, target});
}

QByteArray BoneMap::toJson() const
{
    QJsonObject root;
    root[QStringLiteral("schema")] = QStringLiteral("qtmesh-bonemap-v1");
    root[QStringLiteral("name")] = name;
    QJsonArray arr;
    for (const auto& p : pairs) {
        QJsonObject o;
        o[QStringLiteral("source")] = QString::fromStdString(p.source);
        o[QStringLiteral("target")] = QString::fromStdString(p.target);
        arr.append(o);
    }
    root[QStringLiteral("pairs")] = arr;
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool BoneMap::fromJson(const QByteArray& json, BoneMap* out, QString* error)
{
    auto fail = [&](const QString& e) { if (error) *error = e; return false; };
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject())
        return fail(QStringLiteral("not a JSON object: %1").arg(pe.errorString()));
    const QJsonObject root = doc.object();
    const QString schema = root.value(QStringLiteral("schema")).toString();
    if (schema != QLatin1String("qtmesh-bonemap-v1"))
        return fail(QStringLiteral("unsupported schema '%1' (expected qtmesh-bonemap-v1)").arg(schema));
    if (!root.value(QStringLiteral("pairs")).isArray())
        return fail(QStringLiteral("missing 'pairs' array"));
    BoneMap m;
    m.name = root.value(QStringLiteral("name")).toString();
    std::set<std::string> seenSrc, seenTgt;
    for (const QJsonValue& v : root.value(QStringLiteral("pairs")).toArray()) {
        const QJsonObject o = v.toObject();
        const std::string s = o.value(QStringLiteral("source")).toString().toStdString();
        const std::string t = o.value(QStringLiteral("target")).toString().toStdString();
        if (s.empty() || t.empty())
            return fail(QStringLiteral("every pair needs a non-empty 'source' and 'target'"));
        if (!seenSrc.insert(s).second)
            return fail(QStringLiteral("source bone '%1' is mapped twice").arg(QString::fromStdString(s)));
        if (!seenTgt.insert(t).second)
            return fail(QStringLiteral("target bone '%1' is mapped twice").arg(QString::fromStdString(t)));
        m.pairs.push_back({s, t});
    }
    if (out) *out = std::move(m);
    return true;
}

bool BoneMap::save(const QString& path, QString* error) const
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = QStringLiteral("cannot write %1: %2").arg(path, f.errorString());
        return false;
    }
    f.write(toJson());
    if (!f.commit()) {
        if (error) *error = QStringLiteral("cannot write %1: %2").arg(path, f.errorString());
        return false;
    }
    return true;
}

bool BoneMap::load(const QString& path, BoneMap* out, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read %1: %2").arg(path, f.errorString());
        return false;
    }
    return fromJson(f.readAll(), out, error);
}

// ===========================================================================
// Bundled maps
// ===========================================================================

namespace {

// Mixamo bones use the Autodesk HumanIK names behind a `mixamorig:` namespace
// (which is also the convention Unity's HumanIK/Humanoid importer expects).
const char* const kMixamoBody[] = {
    "Hips", "Spine", "Spine1", "Spine2", "Neck", "Head", "HeadTop_End",
    "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand",
    "RightShoulder", "RightArm", "RightForeArm", "RightHand",
    "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase", "LeftToe_End",
    "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase", "RightToe_End",
};
const char* const kFingers[] = { "Thumb", "Index", "Middle", "Ring", "Pinky" };

// Unreal Engine mannequin names for the same bones ("" = no counterpart).
std::string unrealFor(const std::string& mixamo)
{
    static const std::map<std::string, std::string> body = {
        {"Hips", "pelvis"}, {"Spine", "spine_01"}, {"Spine1", "spine_02"},
        {"Spine2", "spine_03"}, {"Neck", "neck_01"}, {"Head", "head"},
        {"LeftShoulder", "clavicle_l"}, {"LeftArm", "upperarm_l"},
        {"LeftForeArm", "lowerarm_l"}, {"LeftHand", "hand_l"},
        {"RightShoulder", "clavicle_r"}, {"RightArm", "upperarm_r"},
        {"RightForeArm", "lowerarm_r"}, {"RightHand", "hand_r"},
        {"LeftUpLeg", "thigh_l"}, {"LeftLeg", "calf_l"}, {"LeftFoot", "foot_l"},
        {"LeftToeBase", "ball_l"},
        {"RightUpLeg", "thigh_r"}, {"RightLeg", "calf_r"}, {"RightFoot", "foot_r"},
        {"RightToeBase", "ball_r"},
    };
    auto it = body.find(mixamo);
    if (it != body.end()) return it->second;
    // fingers: LeftHandIndex1 → index_01_l
    for (const char* side : {"Left", "Right"}) {
        const std::string pre = std::string(side) + "Hand";
        if (mixamo.rfind(pre, 0) != 0) continue;
        for (const char* fn : kFingers) {
            const std::string fp = pre + fn;
            if (mixamo.rfind(fp, 0) != 0 || mixamo.size() != fp.size() + 1) continue;
            const char seg = mixamo.back();
            if (seg < '1' || seg > '3') continue;
            std::string lower = fn;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            return lower + "_0" + seg + (side[0] == 'L' ? "_l" : "_r");
        }
    }
    return {};
}

std::vector<std::string> mixamoBoneList()
{
    std::vector<std::string> out(std::begin(kMixamoBody), std::end(kMixamoBody));
    for (const char* side : {"Left", "Right"})
        for (const char* fn : kFingers)
            for (int seg = 1; seg <= 4; ++seg)
                out.push_back(std::string(side) + "Hand" + fn + std::to_string(seg));
    return out;
}

}  // namespace

QStringList bundledBoneMapNames()
{
    // mixamo_to_unity == mixamo_to_humanik: Unity's Humanoid/HumanIK rigs
    // use the Autodesk HumanIK bone names Mixamo carries under its prefix.
    return { QStringLiteral("mixamo_to_humanik"), QStringLiteral("mixamo_to_unity"),
             QStringLiteral("mixamo_to_unreal") };
}

bool bundledBoneMap(const QString& name, BoneMap* out)
{
    BoneMap m;
    m.name = name;
    if (name == QLatin1String("mixamo_to_humanik") || name == QLatin1String("mixamo_to_unity")) {
        for (const auto& b : mixamoBoneList())
            m.pairs.push_back({"mixamorig:" + b, b});
    } else if (name == QLatin1String("mixamo_to_unreal")) {
        for (const auto& b : mixamoBoneList()) {
            const std::string u = unrealFor(b);
            if (!u.empty()) m.pairs.push_back({"mixamorig:" + b, u});
        }
    } else {
        return false;
    }
    if (out) *out = std::move(m);
    return true;
}

// ===========================================================================
// Name handling
// ===========================================================================

namespace {

QString stripNamespace(QString n)
{
    // `mixamorig:Hips`, `Armature|Hips`, `rig/Hips` → `Hips`
    for (QChar sep : {QChar(':'), QChar('|'), QChar('/')}) {
        const int i = n.lastIndexOf(sep);
        if (i >= 0) n = n.mid(i + 1);
    }
    static const QRegularExpression prefix(
        QStringLiteral("^(bip0*\\d*[ _]?|def[-_]|org[-_]|mch[-_]|b_|bn_|bone_)"),
        QRegularExpression::CaseInsensitiveOption);
    n.remove(prefix);
    return n;
}

// Tokenise a bone name (camelCase + separators) into lowercase tokens.
QStringList tokens(const QString& raw)
{
    const QString n = stripNamespace(raw);
    QStringList out;
    QString cur;
    auto flush = [&]() { if (!cur.isEmpty()) { out << cur.toLower(); cur.clear(); } };
    for (int i = 0; i < n.size(); ++i) {
        const QChar c = n[i];
        if (!c.isLetterOrNumber()) { flush(); continue; }
        const bool boundary = !cur.isEmpty() && (
            (c.isUpper() && cur.back().isLower()) ||
            (c.isDigit() != cur.back().isDigit()));
        if (boundary) flush();
        cur += c;
    }
    flush();
    return out;
}

// Side + synonym-normalised key: "L|thigh", "R|forearm", "|spine2".
QString synonymKey(const QString& raw)
{
    QStringList t = tokens(raw);
    QString side;
    QStringList rest;
    for (int i = 0; i < t.size(); ++i) {
        const QString& s = t[i];
        if (s == QLatin1String("l") || s == QLatin1String("left")) { side = QStringLiteral("L"); continue; }
        if (s == QLatin1String("r") || s == QLatin1String("right")) { side = QStringLiteral("R"); continue; }
        rest << s;
    }
    QString k = rest.join(QString());
    // Ordered: longest compounds first so "upleg" is consumed before "leg".
    // Outputs are UPPERCASE so no later lowercase pattern can rewrite them
    // ("upperarm" → "UARM" must not then become "UUARM" via "arm").
    static const std::vector<std::pair<const char*, const char*>> syn = {
        {"upperleg", "THIGH"}, {"upleg", "THIGH"}, {"femur", "THIGH"}, {"thigh", "THIGH"},
        {"lowerleg", "CALF"}, {"shin", "CALF"}, {"knee", "CALF"}, {"calf", "CALF"}, {"leg", "CALF"},
        {"upperarm", "UARM"}, {"uparm", "UARM"}, {"humerus", "UARM"},
        {"forearm", "FARM"}, {"lowerarm", "FARM"}, {"elbow", "FARM"}, {"arm", "UARM"},
        {"clavicle", "CLAV"}, {"collar", "CLAV"}, {"shoulder", "CLAV"},
        {"toebase", "BALL"}, {"toe0", "BALL"}, {"ball", "BALL"},
        {"pelvis", "HIPS"}, {"hips", "HIPS"}, {"hip", "HIPS"},
        {"neck01", "NECK"}, {"spine01", "SPINE1"}, {"spine02", "SPINE2"},
        {"spine03", "SPINE3"}, {"wrist", "HAND"}, {"hand", "HAND"},
        {"ankle", "FOOT"}, {"foot", "FOOT"},
    };
    for (const auto& [a, b] : syn) k.replace(QLatin1String(a), QLatin1String(b));
    return side + QLatin1Char('|') + k;
}

int levenshtein(const QString& a, const QString& b)
{
    std::vector<int> prev(size_t(b.size()) + 1), cur(size_t(b.size()) + 1);
    for (int j = 0; j <= b.size(); ++j) prev[size_t(j)] = j;
    for (int i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (int j = 1; j <= b.size(); ++j) {
            const int sub = prev[size_t(j - 1)] + (a[i - 1] == b[j - 1] ? 0 : 1);
            cur[size_t(j)] = std::min({prev[size_t(j)] + 1, cur[size_t(j - 1)] + 1, sub});
        }
        std::swap(prev, cur);
    }
    return prev[size_t(b.size())];
}

}  // namespace

QString normalizeBoneName(const QString& name)
{
    return tokens(name).join(QString());
}

std::string resolveBoneName(const std::vector<std::string>& names, const std::string& wanted)
{
    for (const auto& n : names) if (n == wanted) return n;
    const QString w = QString::fromStdString(wanted);
    for (const auto& n : names)
        if (QString::fromStdString(n).compare(w, Qt::CaseInsensitive) == 0) return n;
    const QString wn = normalizeBoneName(w);
    if (wn.isEmpty()) return {};
    for (const auto& n : names)
        if (normalizeBoneName(QString::fromStdString(n)) == wn) return n;
    return {};
}

BoneMap resolveMap(const BoneMap& map,
                   const std::vector<std::string>& sourceNames,
                   const std::vector<std::string>& targetNames,
                   QStringList* unresolved)
{
    BoneMap out;
    out.name = map.name;
    for (const auto& p : map.pairs) {
        const std::string s = resolveBoneName(sourceNames, p.source);
        const std::string t = resolveBoneName(targetNames, p.target);
        if (s.empty() || t.empty()) {
            if (unresolved)
                unresolved->append(QStringLiteral("%1 -> %2").arg(
                    QString::fromStdString(p.source), QString::fromStdString(p.target)));
            continue;
        }
        out.setPair(s, t);
    }
    return out;
}

AutoMapReport autoMap(const std::vector<std::string>& sourceNames,
                      const std::vector<std::string>& targetNames)
{
    AutoMapReport r;
    r.map.name = QStringLiteral("auto");
    std::set<std::string> usedT;
    std::set<std::string> doneS;
    auto take = [&](const std::string& s, const std::string& t, int& counter) {
        r.map.pairs.push_back({s, t});
        usedT.insert(t);
        doneS.insert(s);
        ++counter;
    };

    // 1) exact name after namespace stripping
    for (const auto& s : sourceNames) {
        const QString sn = normalizeBoneName(QString::fromStdString(s));
        if (sn.isEmpty()) continue;
        for (const auto& t : targetNames) {
            if (usedT.count(t)) continue;
            if (normalizeBoneName(QString::fromStdString(t)) == sn) { take(s, t, r.byExactName); break; }
        }
    }

    // 2) humanoid role (body + fingers). Several bones can share a role (a
    //    rig with two lower-spine bones, Mixamo's Spine + Spine1): pair them
    //    in HIERARCHY order — the name lists come parents-first — so the
    //    lowest goes to the lowest. A few spellings the shared matcher does
    //    not know are resolved here rather than in MotionInbetween, whose
    //    role table the trained motion models depend on.
    auto roleOf = [](const std::string& n) -> int {
        const int r = MotionInbetween::canonicalIndexForBoneV2(QString::fromStdString(n));
        if (r >= 0) return r;
        const QString key = synonymKey(QString::fromStdString(n));
        const QString side = key.section(QLatin1Char('|'), 0, 0);
        const QString body = key.section(QLatin1Char('|'), 1);
        if (side.isEmpty() && (body == QLatin1String("torso") || body == QLatin1String("upperchest")))
            return 2;                                           // chest
        if (body == QLatin1String("palm"))
            return side == QLatin1String("R") ? 9 : side == QLatin1String("L") ? 13 : -1;  // hand
        return -1;
    };
    auto roleTable = [&](const std::vector<std::string>& names) {
        std::map<int, std::vector<std::string>> m;
        for (const auto& n : names) {
            const int role = roleOf(n);
            if (role >= 0) m[role].push_back(n);
        }
        return m;
    };
    const auto rs = roleTable(sourceNames);
    const auto rt = roleTable(targetNames);
    for (const auto& [role, srcList] : rs) {
        auto it = rt.find(role);
        if (it == rt.end()) continue;
        std::vector<std::string> freeS, freeT;
        for (const auto& s : srcList) if (!doneS.count(s)) freeS.push_back(s);
        for (const auto& t : it->second) if (!usedT.count(t)) freeT.push_back(t);
        for (size_t k = 0; k < std::min(freeS.size(), freeT.size()); ++k)
            take(freeS[k], freeT[k], r.byRole);
    }

    // 3) side + synonym key, then 4) close spelling (same side)
    for (const auto& s : sourceNames) {
        if (doneS.count(s)) continue;
        const QString sk = synonymKey(QString::fromStdString(s));
        if (sk.size() <= 1) continue;
        std::string best;
        for (const auto& t : targetNames) {
            if (usedT.count(t)) continue;
            if (synonymKey(QString::fromStdString(t)) == sk) { best = t; break; }
        }
        if (best.empty()) {
            const QString sideS = sk.section(QLatin1Char('|'), 0, 0);
            const QString bodyS = sk.section(QLatin1Char('|'), 1);
            int bestD = 1 << 30;
            for (const auto& t : targetNames) {
                if (usedT.count(t)) continue;
                const QString tk = synonymKey(QString::fromStdString(t));
                if (tk.section(QLatin1Char('|'), 0, 0) != sideS) continue;
                const QString bodyT = tk.section(QLatin1Char('|'), 1);
                const int maxLen = std::max(bodyS.size(), bodyT.size());
                if (maxLen < 4) continue;
                const int d = levenshtein(bodyS, bodyT);
                if (d * 5 <= maxLen && d < bestD) { bestD = d; best = t; }  // ≥ 80 % similar
            }
        }
        if (!best.empty()) take(s, best, r.byFuzzy);
    }

    for (const auto& s : sourceNames) if (!doneS.count(s)) r.unmappedSource.push_back(s);
    for (const auto& t : targetNames) if (!usedT.count(t)) r.unmappedTarget.push_back(t);
    return r;
}

// ===========================================================================
// Option ids
// ===========================================================================

QString translationModeId(TranslationMode m)
{
    switch (m) {
    case TranslationMode::None: return QStringLiteral("none");
    case TranslationMode::Root: return QStringLiteral("root");
    case TranslationMode::All:  return QStringLiteral("all");
    }
    return QStringLiteral("root");
}

bool translationModeFromId(const QString& id, TranslationMode* out)
{
    const QString t = id.trimmed().toLower();
    TranslationMode m;
    if (t == QLatin1String("none") || t == QLatin1String("rotation-only")) m = TranslationMode::None;
    else if (t == QLatin1String("root")) m = TranslationMode::Root;
    else if (t == QLatin1String("all")) m = TranslationMode::All;
    else return false;
    if (out) *out = m;
    return true;
}

QString sourceRestId(SourceRest r)
{
    return r == SourceRest::FirstFrame ? QStringLiteral("first-frame") : QStringLiteral("bind");
}

bool sourceRestFromId(const QString& id, SourceRest* out)
{
    const QString t = id.trimmed().toLower();
    SourceRest r;
    if (t == QLatin1String("bind")) r = SourceRest::Bind;
    else if (t == QLatin1String("first-frame") || t == QLatin1String("first_frame")
             || t == QLatin1String("firstframe")) r = SourceRest::FirstFrame;
    else return false;
    if (out) *out = r;
    return true;
}

// ===========================================================================
// Retarget core
// ===========================================================================

void forwardKinematics(const RigDesc& rig, const LocalPose& pose,
                       std::vector<Ogre::Quaternion>& worldRot,
                       std::vector<Ogre::Vector3>& worldPos)
{
    const int n = rig.size();
    worldRot.assign(size_t(n), Ogre::Quaternion::IDENTITY);
    worldPos.assign(size_t(n), Ogre::Vector3::ZERO);
    for (int i = 0; i < n; ++i) {
        const int p = rig.parent[size_t(i)];
        const Ogre::Quaternion pr = p >= 0 ? worldRot[size_t(p)] : Ogre::Quaternion::IDENTITY;
        const Ogre::Vector3 pp = p >= 0 ? worldPos[size_t(p)] : Ogre::Vector3::ZERO;
        worldRot[size_t(i)] = pr * pose.rot[size_t(i)];
        worldPos[size_t(i)] = pp + pr * pose.pos[size_t(i)];
    }
}

LocalPose bindPose(const RigDesc& rig)
{
    LocalPose p;
    p.pos = rig.bindPos;
    p.rot = rig.bindRot;
    return p;
}

namespace {

bool validRig(const RigDesc& r)
{
    const size_t n = r.names.size();
    if (n == 0 || r.parent.size() != n || r.bindPos.size() != n || r.bindRot.size() != n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (r.parent[i] >= int(i)) return false;   // parents must precede children
    return true;
}

bool isAncestor(const RigDesc& rig, int anc, int node)
{
    for (int p = rig.parent[size_t(node)]; p >= 0; p = rig.parent[size_t(p)])
        if (p == anc) return true;
    return false;
}

// Orthonormal humanoid frame (right, up, forward) as a rotation.
bool humanoidFrame(const Ogre::Vector3& hip, const Ogre::Vector3& head,
                   const Ogre::Vector3& left, const Ogre::Vector3& right,
                   Ogre::Quaternion* out)
{
    Ogre::Vector3 up = head - hip;
    Ogre::Vector3 rt = right - left;
    if (up.length() < 1e-6f || rt.length() < 1e-6f) return false;
    up.normalise();
    rt = rt - up * rt.dotProduct(up);
    if (rt.length() < 1e-6f) return false;
    rt.normalise();
    const Ogre::Vector3 fw = rt.crossProduct(up);
    out->FromAxes(rt, up, fw);
    return true;
}

}  // namespace

std::vector<LocalPose> retargetFrames(const RigDesc& source,
                                      const std::vector<LocalPose>& sourceFrames,
                                      const RigDesc& target,
                                      const BoneMap& map,
                                      const Options& opt,
                                      RetargetReport* report)
{
    RetargetReport rep;
    auto fail = [&](const QString& e) {
        rep.error = e;
        if (report) *report = rep;
        return std::vector<LocalPose>{};
    };
    if (!validRig(source) || !validRig(target))
        return fail(QStringLiteral("invalid skeleton description"));
    if (sourceFrames.empty())
        return fail(QStringLiteral("the source clip has no frames"));
    for (const auto& f : sourceFrames)
        if (f.pos.size() != size_t(source.size()) || f.rot.size() != size_t(source.size()))
            return fail(QStringLiteral("a source frame does not match the source skeleton"));

    const int nt = target.size();
    std::vector<int> srcOf(size_t(nt), -1);
    for (const auto& p : map.pairs) {
        const int s = source.indexOf(p.source);
        const int t = target.indexOf(p.target);
        if (s < 0 || t < 0 || srcOf[size_t(t)] >= 0) continue;
        srcOf[size_t(t)] = s;
        ++rep.mappedBones;
    }
    if (rep.mappedBones == 0)
        return fail(QStringLiteral("no bone pair of the map exists in both skeletons"));

    // Rest poses (world).
    const LocalPose srcRest = opt.sourceRest == SourceRest::FirstFrame ? sourceFrames.front()
                                                                       : bindPose(source);
    std::vector<Ogre::Quaternion> Rs, Rt;
    std::vector<Ogre::Vector3> Ps, Pt;
    forwardKinematics(source, srcRest, Rs, Ps);
    forwardKinematics(target, bindPose(target), Rt, Pt);

    // Global alignment from the humanoid frame of both rests. The frames
    // themselves are kept: each rig's own UP axis comes from its frame
    // (qs/qt · Y), so a Z-up source measures height along ITS Z.
    Ogre::Quaternion G = Ogre::Quaternion::IDENTITY;
    Ogre::Quaternion frameS = Ogre::Quaternion::IDENTITY, frameT = Ogre::Quaternion::IDENTITY;
    {
        auto targetRole = [&](std::initializer_list<int> roles) -> int {
            for (int role : roles)
                for (int t = 0; t < nt; ++t)
                    if (srcOf[size_t(t)] >= 0
                        && MotionInbetween::canonicalIndexForBone(
                               QString::fromStdString(target.names[size_t(t)])) == role)
                        return t;
            return -1;
        };
        const int tHip = targetRole({0});
        const int tHead = targetRole({5, 4, 3});
        int tL = targetRole({19}), tR = targetRole({15});      // upper legs
        if (tL < 0 || tR < 0) { tL = targetRole({11}); tR = targetRole({7}); }  // upper arms
        if (tHip >= 0 && tHead >= 0 && tL >= 0 && tR >= 0) {
            Ogre::Quaternion qs, qt;
            const bool okS = humanoidFrame(Ps[size_t(srcOf[size_t(tHip)])], Ps[size_t(srcOf[size_t(tHead)])],
                                           Ps[size_t(srcOf[size_t(tL)])], Ps[size_t(srcOf[size_t(tR)])], &qs);
            const bool okT = humanoidFrame(Pt[size_t(tHip)], Pt[size_t(tHead)],
                                           Pt[size_t(tL)], Pt[size_t(tR)], &qt);
            if (okS && okT) {
                G = qt * qs.Inverse();
                G.normalise();
                frameS = qs;
                frameT = qt;
                rep.globalAlignment = true;
            }
        }
    }
    const Ogre::Quaternion Ginv = G.Inverse();

    // Direction alignment per mapped bone (inherited by mapped leaves).
    std::vector<Ogre::Quaternion> A(size_t(nt), Ogre::Quaternion::IDENTITY);
    if (opt.alignDirections) {
        for (int t = 0; t < nt; ++t) {
            const int s = srcOf[size_t(t)];
            if (s < 0) continue;
            // Which child defines this bone's direction. A BRANCHING bone
            // (chest → neck + both arms, hips → spine + both legs) must use
            // its CENTRAL child: aligning a chest toward one arm rolls the
            // whole torso sideways — 36° on a UniRig rig whose arms hang
            // straight off the chest (no clavicles), where the arm happened
            // to be the first child. Sided children only decide when there
            // is no central one.
            int child = -1, sided = -1;
            for (int c = t + 1; c < nt; ++c) {
                if (target.parent[size_t(c)] != t || srcOf[size_t(c)] < 0) continue;
                if (!isAncestor(source, s, srcOf[size_t(c)])) continue;
                const QString side = synonymKey(QString::fromStdString(target.names[size_t(c)]))
                                         .section(QLatin1Char('|'), 0, 0);
                if (side.isEmpty()) { child = c; break; }
                if (sided < 0) sided = c;
            }
            if (child < 0) child = sided;
            bool set = false;
            if (child >= 0) {
                const Ogre::Vector3 dt = Pt[size_t(child)] - Pt[size_t(t)];
                const Ogre::Vector3 ds = G * (Ps[size_t(srcOf[size_t(child)])] - Ps[size_t(s)]);
                if (dt.length() > 1e-6f && ds.length() > 1e-6f) {
                    A[size_t(t)] = dt.getRotationTo(ds, Ogre::Vector3::UNIT_Y);
                    set = true;
                }
            }
            if (!set) {   // leaf: inherit the nearest mapped ancestor's alignment
                for (int p = target.parent[size_t(t)]; p >= 0; p = target.parent[size_t(p)])
                    if (srcOf[size_t(p)] >= 0) { A[size_t(t)] = A[size_t(p)]; break; }
            }
        }
    }

    if (std::getenv("QTMESH_RETARGET_DEBUG")) {
        Ogre::Radian ga; Ogre::Vector3 gx; G.ToAngleAxis(ga, gx);
        std::fprintf(stderr, "[retarget] G: %.1f deg about (%.2f %.2f %.2f), humanoid=%d\n",
                     ga.valueDegrees(), gx.x, gx.y, gx.z, rep.globalAlignment ? 1 : 0);
        for (int t = 0; t < nt; ++t) {
            const int s = srcOf[size_t(t)];
            if (s < 0) continue;
            int child = -1, sided = -1;
            for (int c = t + 1; c < nt; ++c) {
                if (target.parent[size_t(c)] != t || srcOf[size_t(c)] < 0) continue;
                if (synonymKey(QString::fromStdString(target.names[size_t(c)])).startsWith(QLatin1Char('|'))) { child = c; break; }
                if (sided < 0) sided = c;
            }
            if (child < 0) child = sided;
            if (child < 0) continue;
            Ogre::Vector3 dt = Pt[size_t(child)] - Pt[size_t(t)];
            Ogre::Vector3 ds = G * (Ps[size_t(srcOf[size_t(child)])] - Ps[size_t(s)]);
            dt.normalise(); ds.normalise();
            std::fprintf(stderr, "[retarget] %-24s rest dir tgt(%.2f %.2f %.2f) src(%.2f %.2f %.2f) %.1f deg\n",
                         target.names[size_t(t)].c_str(), dt.x, dt.y, dt.z, ds.x, ds.y, ds.z,
                         std::acos(std::clamp(dt.dotProduct(ds), -1.0f, 1.0f)) * 57.2958f);
        }
    }

    // Height ratio along the target up axis (translations only).
    // Each rig's own up: the humanoid frame's Y (both are Y-up by
    // convention only when no frame was found). G·Y would assume a Y-up
    // SOURCE and measure depth for a Z-up one.
    const Ogre::Vector3 upT = frameT * Ogre::Vector3::UNIT_Y;
    {
        float tmin = 1e30f, tmax = -1e30f, smin = 1e30f, smax = -1e30f;
        const Ogre::Vector3 upS = frameS * Ogre::Vector3::UNIT_Y;
        for (int t = 0; t < nt; ++t) {
            const int s = srcOf[size_t(t)];
            if (s < 0) continue;
            const float ht = Pt[size_t(t)].dotProduct(upT), hs = Ps[size_t(s)].dotProduct(upS);
            tmin = std::min(tmin, ht); tmax = std::max(tmax, ht);
            smin = std::min(smin, hs); smax = std::max(smax, hs);
        }
        const float ht = tmax - tmin, hs = smax - smin;
        rep.heightScale = (ht > 1e-6f && hs > 1e-6f) ? ht / hs : 1.0f;
    }

    // The root translation follows the SOURCE's root-most mapped bone (the
    // hips on any humanoid). Choosing by TARGET order instead picked an IK
    // foot on rigs whose feet hang off a top-level bone, ahead of the hips
    // (Quaternius characters), and walked the whole body by its ankle.
    int rootT = -1;
    {
        std::vector<int> sdepth(size_t(source.size()), 0);
        for (int i = 0; i < source.size(); ++i)
            sdepth[size_t(i)] = source.parent[size_t(i)] >= 0 ? sdepth[size_t(source.parent[size_t(i)])] + 1 : 0;
        // A mapped HIPS bone wins outright: rigs often share a static helper
        // root ("Root", "Armature") that maps by name and sits above the
        // hips, and giving it the root translation would leave the body
        // walking in place.
        for (int t = 0; t < nt && rootT < 0; ++t)
            if (srcOf[size_t(t)] >= 0
                && MotionInbetween::canonicalIndexForBone(QString::fromStdString(target.names[size_t(t)])) == 0)
                rootT = t;
        int best = 1 << 30;
        if (rootT < 0)
            for (int t = 0; t < nt; ++t) {
                const int s = srcOf[size_t(t)];
                if (s >= 0 && sdepth[size_t(s)] < best) { best = sdepth[size_t(s)]; rootT = t; }
            }
    }
    rep.rootTarget = target.names[size_t(rootT)];

    const float k = rep.heightScale;
    std::vector<LocalPose> out;
    out.reserve(sourceFrames.size());
    std::vector<Ogre::Quaternion> Sw;
    std::vector<Ogre::Quaternion> W(static_cast<size_t>(nt));
    std::vector<Ogre::Vector3> Sp;
    std::vector<Ogre::Vector3> P(static_cast<size_t>(nt));
    for (const LocalPose& frame : sourceFrames) {
        forwardKinematics(source, frame, Sw, Sp);
        LocalPose lp;
        lp.pos = target.bindPos;
        lp.rot = target.bindRot;
        for (int t = 0; t < nt; ++t) {
            const int p = target.parent[size_t(t)];
            const Ogre::Quaternion pr = p >= 0 ? W[size_t(p)] : Ogre::Quaternion::IDENTITY;
            const Ogre::Vector3 pp = p >= 0 ? P[size_t(p)] : Ogre::Vector3::ZERO;
            const Ogre::Quaternion prInv = pr.Inverse();
            const int s = srcOf[size_t(t)];
            if (s >= 0) {
                const Ogre::Quaternion delta = Sw[size_t(s)] * Rs[size_t(s)].Inverse();
                Ogre::Quaternion want = G * delta * Ginv * A[size_t(t)] * Rt[size_t(t)];
                want.normalise();
                Ogre::Quaternion local = prInv * want;
                local.normalise();
                lp.rot[size_t(t)] = local;
            }
            if (opt.translation != TranslationMode::None && s >= 0) {
                if (t == rootT) {
                    const Ogre::Vector3 d = Sp[size_t(s)] - Ps[size_t(s)];
                    const Ogre::Vector3 wp = Pt[size_t(t)] + (G * d) * k;
                    lp.pos[size_t(t)] = prInv * (wp - pp);
                } else if (opt.translation == TranslationMode::All) {
                    const int sp = source.parent[size_t(s)];
                    const Ogre::Quaternion spr = sp >= 0 ? Sw[size_t(sp)] : Ogre::Quaternion::IDENTITY;
                    const Ogre::Vector3 dLocal = frame.pos[size_t(s)] - srcRest.pos[size_t(s)];
                    const Ogre::Vector3 dWorld = G * (spr * dLocal) * k;
                    lp.pos[size_t(t)] = target.bindPos[size_t(t)] + prInv * dWorld;
                }
            }
            W[size_t(t)] = pr * lp.rot[size_t(t)];
            P[size_t(t)] = pp + pr * lp.pos[size_t(t)];
        }
        out.push_back(std::move(lp));
    }

    // Hemisphere continuity so keyframe interpolation never takes the long way.
    for (int t = 0; t < nt; ++t)
        for (size_t f = 1; f < out.size(); ++f)
            if (out[f - 1].rot[size_t(t)].Dot(out[f].rot[size_t(t)]) < 0.0f)
                out[f].rot[size_t(t)] = -out[f].rot[size_t(t)];

    if (report) *report = rep;
    return out;
}

// ===========================================================================
// Ogre adapter
// ===========================================================================

RigDesc describeSkeleton(Ogre::Skeleton* skel, std::vector<unsigned short>* handles)
{
    RigDesc r;
    if (handles) handles->clear();
    if (!skel) return r;
    const unsigned short n = skel->getNumBones();
    std::vector<std::pair<int, unsigned short>> order;   // (depth, handle)
    order.reserve(n);
    for (unsigned short h = 0; h < n; ++h) {
        int depth = 0;
        for (Ogre::Node* p = skel->getBone(h)->getParent(); p; p = p->getParent()) ++depth;
        order.push_back({depth, h});
    }
    std::stable_sort(order.begin(), order.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::map<unsigned short, int> indexOfHandle;
    for (const auto& [depth, h] : order) {
        Ogre::Bone* b = skel->getBone(h);
        indexOfHandle[h] = r.size();
        r.names.push_back(b->getName());
        int parent = -1;
        if (auto* pb = dynamic_cast<Ogre::Bone*>(b->getParent())) {
            auto it = indexOfHandle.find(pb->getHandle());
            if (it != indexOfHandle.end()) parent = it->second;
        }
        r.parent.push_back(parent);
        r.bindPos.push_back(b->getInitialPosition());
        r.bindRot.push_back(b->getInitialOrientation());
        if (handles) handles->push_back(h);
    }
    return r;
}

std::vector<std::string> boneNames(Ogre::Skeleton* skel)
{
    return describeSkeleton(skel).names;
}

bool sampleAnimation(Ogre::Skeleton* skel, const std::string& animName, int fps,
                     std::vector<float>& times, std::vector<LocalPose>& frames,
                     QString* error)
{
    times.clear();
    frames.clear();
    if (!skel || !skel->hasAnimation(animName)) {
        if (error) *error = QStringLiteral("animation '%1' not found on the source skeleton")
                                .arg(QString::fromStdString(animName));
        return false;
    }
    if (fps <= 0) fps = 30;
    Ogre::Animation* anim = skel->getAnimation(animName);
    std::vector<unsigned short> handles;
    const RigDesc rig = describeSkeleton(skel, &handles);
    const float len = anim->getLength();
    const int count = std::max(2, int(std::lround(len * float(fps))) + 1);

    // Snapshot the live pose (manual bones included — a pose-library apply
    // drives bones manually) and restore it afterwards, so sampling never
    // disturbs what the user is looking at.
    struct Saved { Ogre::Vector3 p; Ogre::Quaternion q; Ogre::Vector3 s; bool manual; };
    std::vector<Saved> saved;
    saved.reserve(handles.size());
    for (unsigned short h : handles) {
        Ogre::Bone* b = skel->getBone(h);
        saved.push_back({b->getPosition(), b->getOrientation(), b->getScale(), b->isManuallyControlled()});
    }

    for (int f = 0; f < count; ++f) {
        const float t = len * float(f) / float(count - 1);
        skel->reset(true);
        anim->apply(skel, t, 1.0f, 1.0f);
        LocalPose lp;
        lp.pos.reserve(handles.size());
        lp.rot.reserve(handles.size());
        for (unsigned short h : handles) {
            Ogre::Bone* b = skel->getBone(h);
            lp.pos.push_back(b->getPosition());
            lp.rot.push_back(b->getOrientation());
        }
        times.push_back(t);
        frames.push_back(std::move(lp));
    }

    skel->reset(true);
    for (size_t i = 0; i < handles.size(); ++i) {
        Ogre::Bone* b = skel->getBone(handles[i]);
        b->setManuallyControlled(saved[i].manual);
        b->setPosition(saved[i].p);
        b->setOrientation(saved[i].q);
        b->setScale(saved[i].s);
    }
    skel->_updateTransforms();
    (void)rig;
    return true;
}

std::string uniqueAnimationName(Ogre::Skeleton* skel, const std::string& base)
{
    if (!skel || !skel->hasAnimation(base)) return base;
    for (int i = 2; i < 10000; ++i) {
        const std::string n = base + "_" + std::to_string(i);
        if (!skel->hasAnimation(n)) return n;
    }
    return base + "_retarget";
}

Result retarget(Ogre::Skeleton* sourceSkel, const std::string& sourceAnim,
                Ogre::Skeleton* targetSkel, const std::string& newAnim,
                const BoneMap& map, const Options& options, int fps)
{
    Result res;
    if (!sourceSkel || !targetSkel) { res.error = QStringLiteral("missing skeleton"); return res; }
    if (newAnim.empty()) { res.error = QStringLiteral("the new animation needs a name"); return res; }
    if (targetSkel->hasAnimation(newAnim)) {
        res.error = QStringLiteral("the target already has an animation named '%1'")
                        .arg(QString::fromStdString(newAnim));
        return res;
    }

    std::vector<unsigned short> srcHandles, tgtHandles;
    const RigDesc src = describeSkeleton(sourceSkel, &srcHandles);
    const RigDesc tgt = describeSkeleton(targetSkel, &tgtHandles);
    const BoneMap resolved = resolveMap(map, src.names, tgt.names, &res.unresolvedPairs);

    std::vector<float> times;
    std::vector<LocalPose> frames;
    if (!sampleAnimation(sourceSkel, sourceAnim, fps, times, frames, &res.error)) return res;

    const std::vector<LocalPose> out =
        retargetFrames(src, frames, tgt, resolved, options, &res.report);
    if (out.empty()) { res.error = res.report.error; return res; }

    // Which target bones get a track: mapped ones (+ root translation).
    std::vector<char> animated(size_t(tgt.size()), 0);
    for (const auto& p : resolved.pairs) {
        const int t = tgt.indexOf(p.target);
        if (t >= 0) animated[size_t(t)] = 1;
    }

    const float length = times.back();
    Ogre::Animation* anim = targetSkel->createAnimation(newAnim, length > 0.0f ? length : 0.001f);
    anim->setInterpolationMode(Ogre::Animation::IM_LINEAR);
    for (int t = 0; t < tgt.size(); ++t) {
        if (!animated[size_t(t)]) continue;
        Ogre::Bone* bone = targetSkel->getBone(tgtHandles[size_t(t)]);
        Ogre::NodeAnimationTrack* track = anim->createNodeTrack(tgtHandles[size_t(t)], bone);
        const Ogre::Quaternion bindInv = tgt.bindRot[size_t(t)].Inverse();
        for (size_t f = 0; f < out.size(); ++f) {
            Ogre::TransformKeyFrame* kf = track->createNodeKeyFrame(times[f]);
            Ogre::Quaternion q = bindInv * out[f].rot[size_t(t)];
            q.normalise();
            kf->setRotation(q);
            kf->setTranslate(out[f].pos[size_t(t)] - tgt.bindPos[size_t(t)]);
            kf->setScale(Ogre::Vector3::UNIT_SCALE);
        }
    }

    if (std::getenv("QTMESH_RETARGET_DEBUG")) {
        // Verify the WRITTEN clip: play it on the target, play the source,
        // compare world bone directions (through G) at a few times.
        std::vector<float> tt; std::vector<LocalPose> tf, sf;
        QString e;
        sampleAnimation(targetSkel, newAnim, 4, tt, tf, &e);
        sampleAnimation(sourceSkel, sourceAnim, 4, tt, sf, &e);
        std::vector<Ogre::Quaternion> a, b; std::vector<Ogre::Vector3> pa, pb;
        // recover G the same way the core did
        RetargetReport r2; const Options o2 = options;
        const auto again = retargetFrames(src, sf, tgt, resolved, o2, &r2);
        for (size_t f = 0; f < tf.size() && f < again.size(); ++f) {
            forwardKinematics(tgt, tf[f], a, pa);
            forwardKinematics(tgt, again[f], b, pb);
            float worst = 0; std::string wb;
            for (int t = 0; t < tgt.size(); ++t) {
                const int p = tgt.parent[size_t(t)];
                if (p < 0 || !animated[size_t(t)]) continue;
                Ogre::Vector3 da = pa[size_t(t)] - pa[size_t(p)], db = pb[size_t(t)] - pb[size_t(p)];
                if (da.length() < 1e-6f || db.length() < 1e-6f) continue;
                da.normalise(); db.normalise();
                const float ang = std::acos(std::clamp(da.dotProduct(db), -1.0f, 1.0f)) * 57.2958f;
                if (ang > worst) { worst = ang; wb = tgt.names[size_t(t)]; }
            }
            std::fprintf(stderr, "[retarget] written clip vs core, frame %zu: worst %.2f deg (%s)\n",
                         f, worst, wb.c_str());
        }
    }

    res.ok = true;
    res.animation = newAnim;
    res.frames = int(out.size());
    res.length = length;
    return res;
}

} // namespace Retarget
