#include "MotionInbetween.h"

#include "ModelDownloader.h"
#include "OnnxRuntimeSettings.h"

#include <QByteArray>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {
// Hosted alongside the other QtMeshEditor ONNX models (#404/#408) — the RMIB
// export lands under the inbetween/ subdir of the same HF models repo.
constexpr const char* kDefaultModelBaseUrl =
    "https://huggingface.co/fernandotonon/QtMeshEditor-models/resolve/main/inbetween/";
constexpr const char* kBaseUrlSettingsKey = "ai/inbetweenModelBaseUrl";
} // namespace

#ifdef ENABLE_ONNX
#include <onnxruntime_cxx_api.h>
#include <array>
#include <unordered_map>
#include "AppStorage.h"
#endif

// Out-of-line ctor (same idiom as AutoRig::Options / UniRigPredictor::Options):
// keeps the `{}` default arg on predict()/interpolateSpline() from forcing
// aggregate init of the nested struct while MotionInbetween is incomplete.
MotionInbetween::Options::Options() = default;

// ---------------------------------------------------------------------------
// Canonical skeleton (must match scripts/export-rmib-onnx.py's joint order)
// ---------------------------------------------------------------------------
namespace {
// The 22 CMU core-body joints, in the exact order the model was trained on.
const char* const kCanonJoints[] = {
    "hip", "abdomen", "chest", "neck", "neck1", "head",
    "rcollar", "rshoulder", "relbow", "rhand",
    "lcollar", "lshoulder", "lelbow", "lhand",
    "rbuttock", "rhip", "rknee", "rfoot",
    "lbuttock", "lhip", "lknee", "lfoot",
};
constexpr int kCanonCount = 22;

// 3ds Max Biped side token: names are space-delimited like "Bip001 L UpperArm"
// / "Bip001 R Thigh", so the side is a standalone single-letter TOKEN ('l'/'r')
// that vanishes once spaces are stripped (leaving e.g. "bip001lupperarm", whose
// sideOf() can't tell 'l' apart from the "bip001" prefix). Detect it from the
// raw space-split tokens BEFORE normalisation and fold it into a "left"/"right"
// word so the rest of the matcher works unchanged.
char bipedSideToken(const QString& raw)
{
    const QStringList toks = raw.split(QRegularExpression("[ _\\-.:]"),
                                       Qt::SkipEmptyParts);
    for (const QString& t : toks) {
        const QString tl = t.toLower();
        if (tl == "l") return 'l';
        if (tl == "r") return 'r';
    }
    return 0;
}

// normalise: lowercase, strip a leading "mixamorig[N]:" / "bip001 " style prefix,
// drop separators, fold side tokens so "LeftArm"/"L_Arm"/"arm.l" all compare.
QString normaliseBoneName(const QString& raw)
{
    QString s = raw.toLower();
    int colon = s.lastIndexOf(':');           // mixamorig:LeftArm → LeftArm
    if (colon >= 0) s = s.mid(colon + 1);
    // Fold a 3ds Max Biped standalone side token into a "left"/"right" word so
    // it survives separator stripping (and drop the "bipNNN" armature prefix).
    const char bipedSide = bipedSideToken(raw);
    s.remove(' ').remove('_').remove('-').remove('.');
    s.remove(QRegularExpression("^bip\\d+"));      // "bip001pelvis" → "pelvis"
    if (bipedSide == 'l') s.prepend("left");
    else if (bipedSide == 'r') s.prepend("right");
    return s;
}

// Detect side from a normalised name. Returns 'l', 'r', or 0.
// Word-style ("left"/"right") wins over single-letter affixes (leading/trailing
// 'l'/'r'), which cover CMU `lshoulder` / DCC `arml`.
char sideOf(const QString& n)
{
    if (n.contains("left"))  return 'l';
    if (n.contains("right")) return 'r';
    if (n.startsWith('l') && n.size() > 1) return 'l';
    if (n.startsWith('r') && n.size() > 1) return 'r';
    if (n.endsWith('l')) return 'l';
    if (n.endsWith('r')) return 'r';
    return 0;
}
} // namespace

int MotionInbetween::canonicalJointCount() { return kCanonCount; }

namespace {
// hip abdomen chest neck neck1 head | rcollar rshoulder relbow rhand |
// lcollar lshoulder lelbow lhand | rbuttock rhip rknee rfoot |
// lbuttock lhip lknee lfoot
constexpr int kCanonParent[22] = {
    -1, 0, 1, 2, 3, 4,
     2, 6, 7, 8,
     2, 10, 11, 12,
     0, 14, 15, 16,
     0, 18, 19, 20};
constexpr int kCanonChild[22] = {
     1, 2, 3, 4, 5, -1,
     7, 8, 9, -1,
    11, 12, 13, -1,
    15, 16, 17, -1,
    19, 20, 21, -1};
} // namespace

int MotionInbetween::canonicalParentOf(int i)
{
    return (i >= 0 && i < kCanonCount) ? kCanonParent[i] : -1;
}

int MotionInbetween::canonicalChildOf(int i)
{
    return (i >= 0 && i < kCanonCount) ? kCanonChild[i] : -1;
}

QString MotionInbetween::canonicalJointName(int i)
{
    if (i < 0 || i >= kCanonCount) return {};
    return QString::fromLatin1(kCanonJoints[i]);
}

int MotionInbetween::canonicalIndexForBone(const QString& boneName)
{
    const QString n = normaliseBoneName(boneName);
    if (n.isEmpty()) return -1;
    const char side = sideOf(n);
    auto has = [&](std::initializer_list<const char*> cores) {
        for (const char* c : cores) if (n.contains(QLatin1String(c))) return true;
        return false;
    };
    // Early reject of non-core appendages BEFORE role matching: finger/toe/digit
    // bones often contain a core substring ("LeftHandIndex1" contains "hand",
    // "RightToeBase" a leg-ish name) and would otherwise mis-map onto a core
    // role. Face / twist / helper bones too.
    if (has({"finger", "thumb", "index", "middle", "ring", "pinky", "pinkie",
             "toe", "toebase", "ball", "metacarp", "digit",
             "twist", "roll", "jaw", "eye", "tongue", "ik", "pole",
             "camera", "prop", "weapon"}))
        return -1;

    // Center / spine chain (no side). Spine naming differs: CMU has
    // abdomen→chest; Mixamo has Spine→Spine1→Spine2(→Spine3). Map the LOWER
    // spine (spine/spine1/abdomen/lowerback) to abdomen, and the UPPER spine
    // (chest/spine2/spine3/upperchest) to chest.
    if (has({"hips", "pelvis"}) || n == "hip")          return 0;  // hip
    if (has({"chest", "spine2", "spine3", "upperchest"})) return 2; // chest (upper spine)
    if (has({"spine", "abdomen", "lowerback"}) && side == 0
        && !has({"head", "neck"}))                       return 1;  // abdomen (lower spine)
    if (has({"neck1", "neck2"}))                         return 4;  // neck1
    if (has({"neck"}))                                   return 3;  // neck
    if (has({"head"}) && !has({"headtop", "end"}))       return 5;  // head

    // Limbs — disambiguated by side. NOTE the arm naming conflict between
    // conventions: Mixamo's "Shoulder" is the CLAVICLE (its upper arm is "Arm");
    // CMU's "shoulder" (rshoulder/lshoulder) is the UPPER ARM. We resolve it so
    // both map correctly: collar/clavicle/shoulder → the collar role; arm/
    // upperarm (not fore/lower/hand) → the upper-arm role. CMU's own
    // "rshoulder"/"lshoulder" names hit the upper-arm branch via the exact-name
    // checks below so they keep their original index.
    if (side == 'r') {
        if (n == "rshoulder")                            return 7;   // CMU upper arm
        if (has({"collar", "clavicle", "shoulder"}))     return 6;   // rcollar (incl. Mixamo "Shoulder")
        if (has({"upperarm", "arm"}) && !has({"fore","lower","hand"})) return 7;  // rshoulder (upper arm)
        if (has({"elbow", "forearm", "lowerarm"}))       return 8;   // relbow
        if (has({"hand", "wrist"}))                      return 9;   // rhand
        if (has({"buttock"}))                            return 14;  // rbuttock
        if (has({"upleg", "upperleg", "thigh", "hip", "femur"})) return 15;  // rhip
        if (has({"knee", "leg", "shin", "calf"}) && !has({"upleg","thigh"})) return 16; // rknee
        if (has({"foot", "ankle"}))                      return 17;  // rfoot
    } else if (side == 'l') {
        if (n == "lshoulder")                            return 11;  // CMU upper arm
        if (has({"collar", "clavicle", "shoulder"}))     return 10;  // lcollar (incl. Mixamo "Shoulder")
        if (has({"upperarm", "arm"}) && !has({"fore","lower","hand"})) return 11; // lshoulder (upper arm)
        if (has({"elbow", "forearm", "lowerarm"}))       return 12;  // lelbow
        if (has({"hand", "wrist"}))                      return 13;  // lhand
        if (has({"buttock"}))                            return 18;  // lbuttock
        if (has({"upleg", "upperleg", "thigh", "hip", "femur"})) return 19;  // lhip
        if (has({"knee", "leg", "shin", "calf"}) && !has({"upleg","thigh"})) return 20; // lknee
        if (has({"foot", "ankle"}))                      return 21;  // lfoot
    }
    return -1;
}

// ---- Canonical skeleton V2 (52 = 22 body + 30 finger) ----------------------
namespace {
constexpr int kFingerSegV2 = 3;                 // segments kept per finger
constexpr int kFingerCountV2 = 5;               // thumb..pinky
constexpr int kFingerJointsV2 = 2 * kFingerCountV2 * kFingerSegV2;   // 30
constexpr int kCanonCountV2 = kCanonCount + kFingerJointsV2;         // 52

// V2 finger joint index (22 + fingerSlot). side 0=right/1=left.
inline int fingerSlotV2(int side, int finger, int segment)
{
    if (side < 0 || side > 1 || finger < 0 || finger >= kFingerCountV2
        || segment < 0 || segment >= kFingerSegV2)
        return -1;
    return kCanonCount + (side * kFingerCountV2 + finger) * kFingerSegV2
           + segment;
}
// Human-readable V2 finger joint name, e.g. "rthumb0", "lindex2".
QString fingerJointNameV2(int idx)
{
    const int f = idx - kCanonCount;
    const int side = f / (kFingerCountV2 * kFingerSegV2);
    const int rem = f - side * kFingerCountV2 * kFingerSegV2;
    const int finger = rem / kFingerSegV2, seg = rem % kFingerSegV2;
    static const char* kF[5] = {"thumb", "index", "middle", "ring", "pinky"};
    return QString::fromLatin1(side == 0 ? "r" : "l")
           + QLatin1String(kF[finger]) + QString::number(seg);
}
} // namespace

int MotionInbetween::canonicalJointCountV2() { return kCanonCountV2; }

int MotionInbetween::fingerJointIndexV2(int side, int finger, int segment)
{
    return fingerSlotV2(side, finger, segment);
}

QString MotionInbetween::canonicalJointNameV2(int i)
{
    if (i < 0 || i >= kCanonCountV2) return {};
    if (i < kCanonCount) return QString::fromLatin1(kCanonJoints[i]);
    return fingerJointNameV2(i);
}

int MotionInbetween::canonicalParentOfV2(int i)
{
    if (i < 0 || i >= kCanonCountV2) return -1;
    if (i < kCanonCount) return kCanonParent[i];   // body: same as V1
    // finger joint: seg0 → hand (rhand=9 / lhand=13); deeper → previous segment
    const int f = i - kCanonCount;
    const int side = f / (kFingerCountV2 * kFingerSegV2);
    const int rem = f - side * kFingerCountV2 * kFingerSegV2;
    const int seg = rem % kFingerSegV2;
    if (seg == 0) return (side == 0) ? 9 : 13;     // rhand / lhand
    return i - 1;                                   // previous finger segment
}

int MotionInbetween::canonicalChildOfV2(int i)
{
    if (i < 0 || i >= kCanonCountV2) return -1;
    // Body hands now HAVE finger children, but the child used for bone-direction
    // is unchanged for the body (leave hands as leaves for the direction calc —
    // fingers carry their own directions). Return V1 body child for 0..21.
    if (i < kCanonCount) return kCanonChild[i];
    const int f = i - kCanonCount;
    const int seg = (f % (kFingerCountV2 * kFingerSegV2)) % kFingerSegV2;
    if (seg + 1 < kFingerSegV2) return i + 1;       // next segment
    return -1;                                       // fingertip: leaf
}

int MotionInbetween::canonicalIndexForBoneV2(const QString& boneName)
{
    // Body bones resolve exactly as V1 (0..21).
    const int body = canonicalIndexForBone(boneName);
    if (body >= 0) return body;
    // Finger bones → 22..51 via the finger role (drop segments beyond V2's 3).
    const FingerRole fr = fingerRoleForBone(boneName);
    if (fr.valid() && fr.segment < kFingerSegV2)
        return fingerSlotV2(fr.side, fr.finger, fr.segment);
    return -1;
}

MotionInbetween::FingerRole MotionInbetween::fingerRoleForBone(
    const QString& boneName)
{
    FingerRole r;
    const char side = bipedSideToken(boneName);   // Biped "L"/"R" token
    QString n = boneName.toLower();
    const int colon = n.lastIndexOf(':');
    if (colon >= 0) n = n.mid(colon + 1);
    n.remove(' ').remove('_').remove('-').remove('.');
    // Side: word or single-letter affix, falling back to the Biped token.
    if (n.contains("left")) r.side = 1;
    else if (n.contains("right")) r.side = 0;
    else if (side == 'l') r.side = 1;
    else if (side == 'r') r.side = 0;
    else if (n.startsWith('l')) r.side = 1;
    else if (n.startsWith('r')) r.side = 0;

    // Separator-PRESERVING variant for boundary checks: `n` strips '_'/' '/…,
    // so in `n` a finger word is always letter-preceded ("lefthandring1") and
    // a plain substring test can't tell "L_Ring2" from "SpringBone_L_01".
    QString nsep = boneName.toLower();
    {
        const int c2 = nsep.lastIndexOf(':');
        if (c2 >= 0) nsep = nsep.mid(c2 + 1);
    }

    // Named (Mixamo) convention: Thumb/Index/Middle/Ring/Pinky + trailing seg.
    // Guard against substring hits on NON-finger bones ("ring" in
    // "SpringBone_L_01"/"EarRing", "index" in "IndexHelper"): accept only when
    // the word starts at a boundary in the separator-preserving name (start or
    // non-letter before it), or when the name carries hand context (Mixamo
    // "LeftHandRing1" fuses the words with no separator).
    static const char* kNamed[5] =
        {"thumb", "index", "middle", "ring", "pinky"};
    const bool handContext = n.contains("hand");
    for (int fi = 0; fi < 5; ++fi) {
        const QString key = QLatin1String(kNamed[fi]);
        const int at = n.indexOf(key);
        if (at < 0) continue;
        if (!handContext) {
            // A fused side word directly before it is also fine ("LeftRing1").
            const bool sideFused =
                (at >= 4 && n.mid(at - 4, 4) == QLatin1String("left")) ||
                (at >= 5 && n.mid(at - 5, 5) == QLatin1String("right"));
            const int atSep = nsep.indexOf(key);
            if (!sideFused &&
                (atSep < 0 || (atSep > 0 && nsep.at(atSep - 1).isLetter())))
                continue;   // mid-word hit (spring/string/earring) — not a finger
        }
        r.finger = fi;
        // segment = trailing number after the finger word (1-based → 0-based).
        const QString tail = n.mid(at + key.size());
        int seg = 0; bool got = false;
        for (const QChar& ch : tail)
            if (ch.isDigit()) { seg = ch.digitValue(); got = true; break; }
        r.segment = got ? std::max(0, seg - 1) : 0;
        return r;
    }
    // "pinkie" spelling.
    if (n.contains("pinkie")) {
        r.finger = 4;
        const int at = n.indexOf("pinkie");
        const QString tail = n.mid(at + 6);
        int seg = 0; bool got = false;
        for (const QChar& ch : tail)
            if (ch.isDigit()) { seg = ch.digitValue(); got = true; break; }
        r.segment = got ? std::max(0, seg - 1) : 0;
        return r;
    }

    // Numeric (3ds Max Biped) convention: "finger<F>[<S>]" where F=0..4 is the
    // finger (0=thumb … 4=pinky) and an OPTIONAL trailing digit is the segment
    // (Finger0 = seg0, Finger01 = seg1, Finger02 = seg2). "fingerNub" ignored.
    const int fpos = n.indexOf("finger");
    if (fpos >= 0) {
        const QString tail = n.mid(fpos + 6);      // after "finger"
        if (tail.contains("nub")) return {};       // helper tip, skip
        // digits after "finger": first = finger id, rest = segment.
        QString digits;
        for (const QChar& ch : tail) {
            if (ch.isDigit()) digits += ch;
            else break;
        }
        if (!digits.isEmpty()) {
            const int fid = digits.at(0).digitValue();
            if (fid >= 0 && fid <= 4) {
                r.finger = fid;
                r.segment = (digits.size() >= 2)
                    ? std::max(0, digits.at(1).digitValue()) : 0;
                return r;
            }
        }
    }
    return {};   // not a finger
}

bool MotionInbetween::isModelBackendAvailable()
{
#ifdef ENABLE_ONNX
    return true;
#else
    return false;
#endif
}

QString MotionInbetween::modelPath()
{
    return QDir(AppStorage::aiModelsRoot()).filePath(QStringLiteral("inbetween/rmib.onnx"));
}

bool MotionInbetween::modelPresent()
{
    return QFileInfo::exists(modelPath());
}

QString MotionInbetween::ensureModelBlocking()
{
#ifndef ENABLE_ONNX
    // A non-ONNX build can't run the model — don't touch disk/network. The
    // spline fallback is always used in this configuration.
    return {};
#else
    const QString dest = modelPath();
    if (QFileInfo::exists(dest)) return dest;

    // Offline guard (tests/CI set this so first-run never hits the network).
    if (!qEnvironmentVariableIsEmpty("QTMESH_INBETWEEN_NO_DOWNLOAD"))
        return {};

    // Base URL: QSettings override > env override > default HF repo.
    QString base;
    {
        QSettings s;
        base = s.value(QString::fromLatin1(kBaseUrlSettingsKey)).toString();
        if (base.isEmpty()) {
            const QByteArray env = qgetenv("QTMESH_INBETWEEN_MODEL_BASE_URL");
            base = env.isEmpty() ? QString::fromLatin1(kDefaultModelBaseUrl)
                                 : QString::fromUtf8(env);
        }
    }
    if (base.isEmpty()) return {};
    if (!base.endsWith('/')) base += '/';

    auto* dl = ModelDownloader::instance();
    if (!dl) return {};

    QDir().mkpath(QFileInfo(dest).absolutePath());
    const QString url = base + QStringLiteral("rmib.onnx");
    const QString label = QStringLiteral("RMIB in-betweening model");

    QEventLoop loop;
    bool ok = false, timedOut = false;
    auto onDone = QObject::connect(dl, &ModelDownloader::downloadCompleted, &loop,
        [&](const QString& name, const QString&) {
            if (name == label) { ok = true; loop.quit(); }
        });
    auto onErr = QObject::connect(dl, &ModelDownloader::downloadError, &loop,
        [&](const QString& name, const QString&) {
            if (name == label) { ok = false; loop.quit(); }
        });
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop,
                     [&]() { timedOut = true; loop.quit(); });
    timeout.start(300000);  // 5 min — RMIB is ~10 MB but allow slow links

    dl->startDownload(url, dest, label);
    loop.exec();

    QObject::disconnect(onDone);
    QObject::disconnect(onErr);
    if (timedOut && dl) dl->cancelDownload();

    return (ok && !timedOut && QFileInfo::exists(dest)) ? dest : QString();
#endif
}

// ---------------------------------------------------------------------------
// Pure-data helpers
// ---------------------------------------------------------------------------

std::array<float, 4> MotionInbetween::slerpQuat(const std::array<float, 4>& a,
                                                const std::array<float, 4>& b,
                                                float t)
{
    // a, b are [x,y,z,w]. Shortest-arc: flip b if the dot is negative so we
    // interpolate over the short way round.
    double ax = a[0], ay = a[1], az = a[2], aw = a[3];
    double bx = b[0], by = b[1], bz = b[2], bw = b[3];
    double dot = ax*bx + ay*by + az*bz + aw*bw;
    if (dot < 0.0) { bx = -bx; by = -by; bz = -bz; bw = -bw; dot = -dot; }

    double s0, s1;
    if (dot > 0.9995) {
        // Nearly parallel — fall back to normalised lerp to avoid div-by-~0.
        s0 = 1.0 - t;
        s1 = t;
    } else {
        const double theta0 = std::acos(std::clamp(dot, -1.0, 1.0));
        const double sinTheta0 = std::sin(theta0);
        const double theta = theta0 * t;
        s1 = std::sin(theta) / sinTheta0;
        s0 = std::cos(theta) - dot * s1;
    }
    double rx = s0*ax + s1*bx;
    double ry = s0*ay + s1*by;
    double rz = s0*az + s1*bz;
    double rw = s0*aw + s1*bw;
    const double n = std::sqrt(rx*rx + ry*ry + rz*rz + rw*rw);
    if (n > 1e-12) { rx/=n; ry/=n; rz/=n; rw/=n; }
    else { rx=0; ry=0; rz=0; rw=1; }
    return { static_cast<float>(rx), static_cast<float>(ry),
             static_cast<float>(rz), static_cast<float>(rw) };
}

float MotionInbetween::hermite(float p0, float p1, float m0, float m1, float t)
{
    // Cubic Hermite basis.
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2*t3 - 3*t2 + 1;
    const float h10 = t3 - 2*t2 + t;
    const float h01 = -2*t3 + 3*t2;
    const float h11 = t3 - t2;
    return h00*p0 + h10*m0 + h01*p1 + h11*m1;
}

// ---------------------------------------------------------------------------
// Spline fallback (always available)
// ---------------------------------------------------------------------------

MotionInbetween::Result MotionInbetween::interpolateSpline(
    const Pose& start, const Pose& end, const std::vector<Channel>& layout,
    const Options& opts, const Pose* preStart, const Pose* postEnd)
{
    Result r;
    const size_t C = layout.size();
    if (start.size() != C || end.size() != C) {
        r.error = QStringLiteral(
            "MotionInbetween: pose/layout channel-count mismatch (start=%1 end=%2 layout=%3)")
            .arg(start.size()).arg(end.size()).arg(C);
        return r;
    }
    const int gap = std::max(0, opts.gapFrames);
    r.frames.reserve(static_cast<size_t>(gap));

    // For scalar channels we use Catmull-Rom tangents: m = (p_{next} - p_{prev})/2.
    // Endpoints' outer neighbours come from preStart/postEnd when provided, else
    // we use the secant (start→end) so the curve is at least C1 at the segment
    // ends and never overshoots more than a Catmull-Rom would.
    auto neighbourOk = [&](const Pose* p) {
        return p != nullptr && p->size() == C;
    };
    const bool havePre  = neighbourOk(preStart);
    const bool havePost = neighbourOk(postEnd);

    for (int f = 0; f < gap; ++f) {
        // Parametric position in (0,1), excluding the endpoints themselves.
        const float t = static_cast<float>(f + 1) / static_cast<float>(gap + 1);
        Pose pose(C, 0.0f);

        for (size_t c = 0; c < C; ++c) {
            if (layout[c] == Channel::QuatCont) {
                continue;  // written by its QuatStart below
            }
            if (layout[c] == Channel::QuatStart) {
                // Pull the 4-wide block [c..c+3] and slerp it as a unit.
                std::array<float,4> qa{ start[c], (c+1<C?start[c+1]:0.f),
                                        (c+2<C?start[c+2]:0.f), (c+3<C?start[c+3]:1.f) };
                std::array<float,4> qb{ end[c], (c+1<C?end[c+1]:0.f),
                                        (c+2<C?end[c+2]:0.f), (c+3<C?end[c+3]:1.f) };
                const std::array<float,4> q = slerpQuat(qa, qb, t);
                pose[c] = q[0];
                if (c+1 < C) pose[c+1] = q[1];
                if (c+2 < C) pose[c+2] = q[2];
                if (c+3 < C) pose[c+3] = q[3];
                continue;
            }
            // Scalar: cubic-Hermite with Catmull-Rom tangents.
            const float p0 = start[c];
            const float p1 = end[c];
            const float prev = havePre  ? (*preStart)[c] : p0;  // secant if absent
            const float next = havePost ? (*postEnd)[c]  : p1;
            const float m0 = 0.5f * (p1 - prev);   // (next_of_p0 - prev_of_p0)/2
            const float m1 = 0.5f * (next - p0);   // (next_of_p1 - prev_of_p1)/2
            pose[c] = hermite(p0, p1, m0, m1, t);
        }
        r.frames.push_back(std::move(pose));
    }

    r.ok = true;
    r.usedModel = false;
    return r;
}

// ---------------------------------------------------------------------------
// ONNX RMIB path
// ---------------------------------------------------------------------------

#ifndef ENABLE_ONNX

MotionInbetween::Result MotionInbetween::predict(
    const Pose& start, const Pose& end, const std::vector<Channel>& layout,
    const QString& /*modelPath*/, const Options& opts,
    const Pose* preStart, const Pose* postEnd)
{
    // No ONNX in this build — always the deterministic spline.
    Result r = interpolateSpline(start, end, layout, opts, preStart, postEnd);
    if (r.ok) {
        r.fallbackReason = QStringLiteral(
            "AI in-betweening needs an ONNX-enabled build (rebuild with "
            "-DENABLE_ONNX) — used the spline fallback.");
    }
    return r;
}

#else // ENABLE_ONNX

MotionInbetween::Result MotionInbetween::predict(
    const Pose& start, const Pose& end, const std::vector<Channel>& layout,
    const QString& modelPath, const Options& opts,
    const Pose* preStart, const Pose* postEnd)
{
    auto fallback = [&](const QString& why) -> Result {
        Result r = interpolateSpline(start, end, layout, opts, preStart, postEnd);
        if (r.ok) r.fallbackReason = why;
        return r;
    };

    if (opts.forceFallback)
        return fallback(QStringLiteral("Spline fallback forced by request."));
    // RMIB is trained +Y-up. Rather than feed it a wrong-convention pose (which
    // would silently produce garbage), defer a non-Y up axis to the axis-
    // agnostic spline fallback. (The fallback interpolates channels
    // independently, so it's correct for any up axis.)
    if (opts.upAxis != 1)
        return fallback(QStringLiteral(
            "RMIB requires +Y-up; up axis %1 used the spline fallback.")
            .arg(opts.upAxis));
    if (modelPath.isEmpty() || !QFileInfo::exists(modelPath))
        return fallback(QStringLiteral(
            "RMIB model not found at %1 — used the spline fallback.").arg(modelPath));

    const int gap = std::max(0, opts.gapFrames);
    const size_t C = layout.size();
    if (start.size() != C || end.size() != C)
        return fallback(QStringLiteral(
            "Pose/layout channel mismatch — used the spline fallback."));

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "qtmesh_inbetween");
        Ort::SessionOptions so;
        OnnxRuntimeSettings::configureSessionOptions(so);
        so.SetIntraOpNumThreads(1);
#ifdef _WIN32
        const std::wstring wpath = modelPath.toStdWString();
        Ort::Session session(env, wpath.c_str(), so);
#else
        const std::string p = modelPath.toStdString();
        Ort::Session session(env, p.c_str(), so);
#endif
        Ort::AllocatorWithDefaultOptions alloc;

        // RMIB I/O contract (kept deliberately simple + discovered at runtime so
        // a re-export with different names still works): the model takes a
        // [1, 2, C] tensor (start pose, end pose) plus a scalar gap count, and
        // emits a [1, gap, C] tensor of intermediate poses. If the discovered
        // input rank / channel count doesn't match our layout, fall back —
        // RMIB is skeleton-specific and a mismatch means an incompatible rig.
        const size_t inCount = session.GetInputCount();
        if (inCount < 1)
            return fallback(QStringLiteral(
                "RMIB model exposes no inputs — used the spline fallback."));

        Ort::TypeInfo inInfo = session.GetInputTypeInfo(0);
        const auto inShape = inInfo.GetTensorTypeAndShapeInfo().GetShape();
        // Expect [.., 2, C] or [.., C]; the last dim must equal our channel count
        // for the skeleton to be compatible with this model.
        if (!inShape.empty() && inShape.back() > 0 &&
            static_cast<size_t>(inShape.back()) != C) {
            return fallback(QStringLiteral(
                "Skeleton incompatible with the RMIB model (model expects %1 "
                "channels, rig has %2) — used the spline fallback.")
                .arg(inShape.back()).arg(C));
        }

        // Pack [1, 2, C]: start then end.
        std::vector<float> input;
        input.reserve(2 * C);
        input.insert(input.end(), start.begin(), start.end());
        input.insert(input.end(), end.begin(), end.end());
        const std::array<int64_t, 3> shape = { 1, 2, static_cast<int64_t>(C) };

        Ort::MemoryInfo memInfo =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memInfo, input.data(), input.size(), shape.data(), shape.size());

        // Optional second input: a scalar gap count (some exports take it).
        std::vector<int64_t> gapData{ gap };
        const std::array<int64_t, 1> gapShape = { 1 };

        // Hold the allocated name strings for the lifetime of the Run() call.
        // Ort::AllocatedStringPtr is a move-only unique_ptr, so collect via
        // reserve()+push_back rather than default-constructing slots.
        std::vector<Ort::AllocatedStringPtr> inNameHolders;
        inNameHolders.reserve(2);
        inNameHolders.push_back(session.GetInputNameAllocated(0, alloc));
        std::vector<const char*> inNames{ inNameHolders.back().get() };
        std::vector<Ort::Value> inputs;
        inputs.push_back(std::move(inputTensor));

        if (inCount >= 2) {
            inNameHolders.push_back(session.GetInputNameAllocated(1, alloc));
            inNames.push_back(inNameHolders.back().get());
            inputs.push_back(Ort::Value::CreateTensor<int64_t>(
                memInfo, gapData.data(), gapData.size(),
                gapShape.data(), gapShape.size()));
        }

        const size_t outCount = session.GetOutputCount();
        std::vector<Ort::AllocatedStringPtr> outHolders;
        std::vector<const char*> outNames;
        for (size_t i = 0; i < outCount; ++i) {
            outHolders.push_back(session.GetOutputNameAllocated(i, alloc));
            outNames.push_back(outHolders.back().get());
        }

        std::vector<Ort::Value> outputs = session.Run(
            Ort::RunOptions{nullptr}, inNames.data(), inputs.data(), inputs.size(),
            outNames.data(), outNames.size());

        if (outputs.empty() || !outputs[0].IsTensor())
            return fallback(QStringLiteral(
                "RMIB model produced no usable output — used the spline fallback."));

        auto outTI = outputs[0].GetTensorTypeAndShapeInfo();
        const size_t elems = outTI.GetElementCount();
        const auto outShape = outTI.GetShape();
        // Validate the SHAPE, not just the element count: the last dim must be
        // our channel count and the frame dim must cover `gap`. A re-exported
        // model that returns the same float count in a different layout (e.g.
        // [1, C, gap] instead of [1, gap, C]) would otherwise be sliced wrong —
        // defer it to the spline rather than emit scrambled poses.
        if (outShape.empty() || outShape.back() <= 0
            || static_cast<size_t>(outShape.back()) != C)
            return fallback(QStringLiteral(
                "RMIB output last-dim %1 != channel count %2 — used the spline fallback.")
                .arg(outShape.empty() ? -1 : outShape.back()).arg(C));
        // Total framed rows available = elems / C; must be >= gap.
        if (elems < static_cast<size_t>(gap) * C)
            return fallback(QStringLiteral(
                "RMIB output too small (%1 < %2) — used the spline fallback.")
                .arg(elems).arg(static_cast<size_t>(gap) * C));

        const float* d = outputs[0].GetTensorData<float>();
        Result r;
        r.frames.reserve(static_cast<size_t>(gap));
        for (int f = 0; f < gap; ++f) {
            Pose pose(d + static_cast<size_t>(f) * C,
                      d + static_cast<size_t>(f + 1) * C);
            r.frames.push_back(std::move(pose));
        }
        r.ok = true;
        r.usedModel = true;
        return r;
    } catch (const Ort::Exception& e) {
        return fallback(QStringLiteral(
            "RMIB inference failed (%1) — used the spline fallback.")
            .arg(QString::fromUtf8(e.what())));
    } catch (const std::exception& e) {
        return fallback(QStringLiteral(
            "RMIB error (%1) — used the spline fallback.")
            .arg(QString::fromUtf8(e.what())));
    }
}

#endif // ENABLE_ONNX
