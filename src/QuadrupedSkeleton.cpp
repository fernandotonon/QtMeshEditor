#include "QuadrupedSkeleton.h"

#include <QStringList>

#include <array>
#include <vector>

namespace {

struct JointDef { const char* name; int parent; };

// Index order matches the header's documented layout; parent indices encode
// the tree. Legs hang off the chest (front) and root (back), which is how a
// real quadruped rig is built — the front legs are carried by the shoulder
// girdle, not the pelvis.
constexpr std::array<JointDef, 18> kJoints{{
    {"root",          -1},
    {"spine",          0},
    {"chest",          1},
    {"neck",           2},
    {"head",           3},
    {"frontUpLeg.L",   2}, {"frontLowLeg.L",  5}, {"frontFoot.L",  6},
    {"frontUpLeg.R",   2}, {"frontLowLeg.R",  8}, {"frontFoot.R",  9},
    {"backUpLeg.L",    0}, {"backLowLeg.L",  11}, {"backFoot.L",  12},
    {"backUpLeg.R",    0}, {"backLowLeg.R",  14}, {"backFoot.R",  15},
    {"tail1",          0},
}};

// Normalise a bone name: lower-case, strip a rig prefix up to ':' (mixamorig:)
// and drop the "_end" leaf markers Blender/FBX exports append.
QString normalise(const QString& raw)
{
    QString n = raw.toLower().trimmed();
    const int colon = n.lastIndexOf(QLatin1Char(':'));
    if (colon >= 0) n = n.mid(colon + 1);
    if (n.endsWith(QLatin1String("_end"))) n.chop(4);
    return n;
}

// Side of a bone name: 'l', 'r', or 0. Checks the explicit suffixes first
// (".l"/"_l"/".r"/"_r") because a bare "l"/"r" substring search would match
// far too much ("shoulder" contains an r, "pelvis" an l).
char sideOf(const QString& n)
{
    if (n.endsWith(QLatin1String(".l")) || n.endsWith(QLatin1String("_l"))
        || n.startsWith(QLatin1String("l_")) || n.contains(QLatin1String("left")))
        return 'l';
    if (n.endsWith(QLatin1String(".r")) || n.endsWith(QLatin1String("_r"))
        || n.startsWith(QLatin1String("r_")) || n.contains(QLatin1String("right")))
        return 'r';
    return 0;
}

bool contains(const QString& n, std::initializer_list<const char*> keys)
{
    for (const char* k : keys)
        if (n.contains(QLatin1String(k))) return true;
    return false;
}

} // namespace

namespace CreatureSkeleton {
namespace QuadrupedSkeleton {

int jointCount() { return static_cast<int>(kJoints.size()); }

QString jointName(int i)
{
    if (i < 0 || i >= jointCount()) return {};
    return QString::fromLatin1(kJoints[static_cast<size_t>(i)].name);
}

int parentOf(int i)
{
    if (i < 0 || i >= jointCount()) return -1;
    return kJoints[static_cast<size_t>(i)].parent;
}

int indexForBone(const QString& boneName)
{
    const QString n = normalise(boneName);
    if (n.isEmpty()) return -1;

    // Helper/appendage bones that would otherwise hit a core role.
    if (contains(n, {"ear", "horn", "eye", "jaw", "tongue", "udder",
                     "ik", "pole", "twist", "roll", "camera", "prop"}))
        return -1;

    // Spine chain (no side). "body"/"torso"/"back" are the Quaternius spine
    // names; "hips"/"pelvis" the root. NB "back" is checked AFTER the
    // front/back LEG test below, because "BackUpLeg" also contains "back".
    if (contains(n, {"hips", "pelvis", "root"}) && !contains(n, {"leg", "foot"}))
        return 0;

    const char side = sideOf(n);
    const bool isFront = contains(n, {"front", "fore"});
    const bool isBack  = contains(n, {"back", "hind", "rear"});

    // ---- legs: the front/back distinction the humanoid mapper lacks -------
    if (side && (isFront || isBack) && contains(n, {"leg", "foot", "paw", "hoof"})) {
        const int base = isFront ? (side == 'l' ? 5 : 8)
                                 : (side == 'l' ? 11 : 14);
        // Order matters: "lowleg" and "upleg" both contain "leg", and
        // "foot" must win over a rig that spells it "footleg".
        if (contains(n, {"foot", "paw", "hoof", "ankle"})) return base + 2;
        if (contains(n, {"lowleg", "lowerleg", "shin", "cannon", "knee"}))
            return base + 1;
        if (contains(n, {"upleg", "upperleg", "thigh", "femur", "humerus"}))
            return base;
        // A bare "FrontLeg.L" with no segment word. The Quaternius rigs are
        // THREE-segment -- FrontLeg -> FrontUpLeg -> FrontLowLeg -- so this
        // is the TOPMOST limb bone (the shoulder/hip attachment), NOT the
        // middle one. It was mapped to the lower leg, where it lost the role
        // to the explicitly-named FrontLowLeg and so received no animation at
        // all; the canonical upper-leg role then drove FrontUpLeg, the SECOND
        // segment. The limb therefore swung from the wrong joint, which reads
        // as legs bunched under the body with a cramped stride even though
        // the per-joint angles match the source exactly.
        //
        // Map it to the UPPER leg so the chain lines up with the canonical
        // UpLeg -> LowLeg -> Foot; the explicit FrontUpLeg then loses that
        // role on specificity, which is correct -- it is the middle segment
        // and canonical LowLeg is where its motion belongs.
        return base;
    }

    // ---- spine / head (after legs, so "BackUpLeg" cannot reach "back") ----
    if (contains(n, {"tail"})) {
        // Only the first tail joint is canonical; later segments are
        // follow-through this skeleton does not model.
        if (n.contains(QLatin1String("tail1")) || n == QLatin1String("tail"))
            return 17;
        return -1;
    }
    if (contains(n, {"head", "skull"}))            return 4;
    if (contains(n, {"neck"}))                     return 3;
    if (contains(n, {"chest", "shoulders", "shoulder", "withers"})) return 2;
    if (contains(n, {"spine", "body", "torso", "back", "abdomen"})) return 1;
    return -1;
}

bool isPlausibleQuadruped(const bool* resolvedRoles)
{
    if (!resolvedRoles) return false;
    // Spine chain: root + chest are what every limb hangs from. Without them
    // there is nothing to anchor a retarget onto.
    if (!resolvedRoles[0] || !resolvedRoles[2]) return false;
    // At least 3 of 4 legs — a rig missing one leg is still retargetable
    // (amputee/stylised creatures exist), but 2 or fewer is a biped or a
    // mis-detection and must not take this path.
    int legs = 0;
    for (int base : {5, 8, 11, 14})
        if (resolvedRoles[base]) ++legs;
    return legs >= 3;
}


int roleSpecificity(const QString& boneName)
{
    const QString n = normalise(boneName);
    if (n.isEmpty()) return 0;
    // Named its own segment: trust it over any guess.
    if (contains(n, {"foot", "paw", "hoof", "ankle",
                     "lowleg", "lowerleg", "shin", "cannon", "knee",
                     "upleg", "upperleg", "thigh", "femur", "humerus",
                     "hips", "pelvis", "tail", "head", "skull", "neck",
                     "chest", "withers", "spine"}))
        return 2;
    // A side+front/back leg bone with NO segment word ("FrontLeg.L") is the
    // TOPMOST limb bone on the three-segment Quaternius rigs
    // (FrontLeg -> FrontUpLeg -> FrontLowLeg), and indexForBone maps it to
    // the canonical UPPER leg. It must BEAT the explicitly-named
    // "FrontUpLeg", which is really the middle segment: the canonical chain
    // is UpLeg -> LowLeg -> Foot, so the limb has to swing from its true
    // root or the stride comes out cramped (the legs bunch under the body
    // while the per-joint angles still match the source exactly).
    //
    // 3 is deliberately above the "names its own segment" tier: hierarchy
    // position is stronger evidence than a name here.
    const char side = sideOf(n);
    if (side && contains(n, {"front", "fore", "back", "hind", "rear"})
             && contains(n, {"leg"})
             && !contains(n, {"upleg", "upperleg", "lowleg", "lowerleg",
                              "thigh", "shin", "foot", "paw", "hoof"}))
        return 3;
    return 1;
}

} // namespace QuadrupedSkeleton

// ---- winged biped (dragon, bat) --------------------------------------------
namespace {

// Reference rig: Quaternius Monster Pack (CC0) — Dragon.fbx and Bat.fbx share
// an IDENTICAL bone structure, which is why one plan covers both:
//   Root, BodyRoot, Body, Neck, Head, Shoulder.{L,R}, Wing1..4.{L,R},
//   UpperLeg.{L,R}, LowerLeg.{L,R}, Feet.{L,R}, Tail1..4
//
// Wings are modelled to three segments (shoulder + two bones). Wing4 is the
// membrane tip — follow-through, like the later tail segments, so it is not
// canonicalised.
constexpr std::array<JointDef, 16> kWingedJoints{{
    {"root",        -1},
    {"spine",        0},
    {"chest",        1},
    {"neck",         2},
    {"head",         3},
    {"wingRoot.L",   2}, {"wing1.L", 5}, {"wing2.L", 6},
    {"wingRoot.R",   2}, {"wing1.R", 8}, {"wing2.R", 9},
    {"upLeg.L",      0}, {"lowLeg.L", 11},
    {"upLeg.R",      0}, {"lowLeg.R", 13},
    {"tail1",        0},
}};

} // namespace

namespace WingedBiped {

int jointCount() { return static_cast<int>(kWingedJoints.size()); }

QString jointName(int i)
{
    if (i < 0 || i >= jointCount()) return {};
    return QString::fromLatin1(kWingedJoints[static_cast<size_t>(i)].name);
}

int parentOf(int i)
{
    if (i < 0 || i >= jointCount()) return -1;
    return kWingedJoints[static_cast<size_t>(i)].parent;
}

int indexForBone(const QString& boneName)
{
    const QString n = normalise(boneName);
    if (n.isEmpty()) return -1;
    if (contains(n, {"eye", "jaw", "tongue", "nose", "face", "ik", "pole",
                     "twist", "roll", "camera", "prop"}))
        return -1;

    const char side = sideOf(n);

    // Wings first: "Shoulder.L" is the WING ROOT on these rigs, not a
    // humanoid clavicle, and Wing1..3 are its chain.
    if (side) {
        if (n.contains(QLatin1String("wing"))) {
            const int base = (side == 'l') ? 5 : 8;
            if (n.contains(QLatin1String("wing1"))) return base + 1;
            if (n.contains(QLatin1String("wing2"))) return base + 2;
            // wing3 folds into the last modelled segment; wing4 is the
            // membrane tip and carries no role.
            if (n.contains(QLatin1String("wing3"))) return base + 2;
            if (n.contains(QLatin1String("wing4"))) return -1;
            return base;
        }
        // "Shoulder.L" IS the wing root on the reference dragon/bat rigs —
        // but a Mixamo humanoid also has LeftShoulder/RightShoulder, and
        // accepting those made Animated_Woman and KnightCharacter detect as
        // winged bipeds (caught by auditing against every rig in the
        // corpus, not by the fixtures). The bone name alone cannot tell
        // them apart, so the distinction is made at the RIG level:
        // detectBodyPlan requires a real Wing* bone before this plan can
        // win, and a humanoid has none.
        if (contains(n, {"shoulder", "clavicle"}))
            return (side == 'l') ? 5 : 8;      // wing root
        if (contains(n, {"feet", "foot", "ankle", "claw"}))
            return (side == 'l') ? 12 : 14;    // folded into the lower leg
        if (contains(n, {"lowerleg", "lowleg", "shin"}))
            return (side == 'l') ? 12 : 14;
        if (contains(n, {"upperleg", "upleg", "thigh", "leg"}))
            return (side == 'l') ? 11 : 13;
    }

    if (contains(n, {"tail"})) {
        if (n.contains(QLatin1String("tail1")) || n == QLatin1String("tail"))
            return 15;
        return -1;
    }
    if (contains(n, {"head", "skull"}))  return 4;
    if (contains(n, {"neck"}))           return 3;
    if (contains(n, {"chest", "withers"})) return 2;
    // "BodyRoot" is the rig's own root offset; "Body" is the spine.
    if (n == QLatin1String("bodyroot") || contains(n, {"hips", "pelvis"}))
        return 0;
    if (n == QLatin1String("root"))      return 0;
    if (contains(n, {"spine", "body", "torso", "abdomen"})) return 1;
    return -1;
}

bool isPlausible(const bool* resolvedRoles)
{
    if (!resolvedRoles) return false;
    if (!resolvedRoles[0] && !resolvedRoles[1]) return false;  // no spine anchor
    // BOTH wing roots — that is what distinguishes this plan. Legs are
    // optional: a flying creature may have vestigial or absent leg bones.
    return resolvedRoles[5] && resolvedRoles[8];
}


int roleSpecificity(const QString& boneName)
{
    const QString n = normalise(boneName);
    if (n.isEmpty()) return 0;
    if (contains(n, {"wing1", "wing2", "wing3",
                     "feet", "foot", "ankle", "claw",
                     "lowerleg", "lowleg", "shin",
                     "upperleg", "upleg", "thigh",
                     "hips", "pelvis", "tail", "head", "skull", "neck",
                     "chest", "withers", "spine"}))
        return 2;
    const char side = sideOf(n);
    if (side && contains(n, {"leg"})
             && !contains(n, {"upperleg", "upleg", "thigh",
                              "lowerleg", "lowleg", "shin"}))
        return 0;
    return 1;
}

} // namespace WingedBiped

BodyPlan detectBodyPlan(const QStringList& boneNames, bool* ok)
{
    std::vector<bool> quad(static_cast<size_t>(QuadrupedSkeleton::jointCount()), false);
    std::vector<bool> wing(static_cast<size_t>(WingedBiped::jointCount()), false);
    for (const QString& b : boneNames) {
        const int q = QuadrupedSkeleton::indexForBone(b);
        if (q >= 0) quad[static_cast<size_t>(q)] = true;
        const int w = WingedBiped::indexForBone(b);
        if (w >= 0) wing[static_cast<size_t>(w)] = true;
    }
    // Wings are the stronger signal and are tested FIRST: a dragon's leg
    // bones also satisfy parts of the quadruped mapper, but nothing in a
    // horse rig produces wing roots, so this ordering cannot misfire the
    // other way.
    // A real Wing* bone is mandatory. WingedBiped::indexForBone maps
    // "Shoulder.L" to the wing root (correct for the dragon/bat rigs), but a
    // humanoid clavicle is spelled the same way — so the plan is only
    // admissible when the rig actually carries wing bones.
    bool hasWingBone = false;
    for (const QString& b : boneNames)
        if (b.contains(QLatin1String("wing"), Qt::CaseInsensitive)) {
            hasWingBone = true;
            break;
        }
    std::vector<char> wraw(wing.begin(), wing.end());
    if (hasWingBone
        && WingedBiped::isPlausible(reinterpret_cast<const bool*>(wraw.data()))) {
        if (ok) *ok = true;
        return BodyPlan::WingedBiped;
    }
    std::vector<char> qraw(quad.begin(), quad.end());
    if (QuadrupedSkeleton::isPlausibleQuadruped(
            reinterpret_cast<const bool*>(qraw.data()))) {
        if (ok) *ok = true;
        return BodyPlan::Quadruped;
    }
    if (ok) *ok = false;
    return BodyPlan::Quadruped;
}

} // namespace CreatureSkeleton
