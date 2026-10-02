/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef ANIMATIONRETARGETER_H
#define ANIMATIONRETARGETER_H

// Animation retargeting between INCOMPATIBLE skeletons (#523, epic #517).
//
// AnimationMerger only merges clips between skeletons that share bone names
// and rest poses. This maps a clip from rig A onto rig B through an explicit
// source→target BONE MAP:
//
//   - Auto-mapping (`autoMap`): exact name (after stripping namespaces such as
//     `mixamorig:`), then the rig-independent humanoid role matcher the motion
//     pipeline already uses (MotionInbetween::canonicalIndexForBoneV2 — body +
//     fingers, Mixamo / Unreal / Unity / Biped / CMU spellings), then a fuzzy
//     token match with side normalisation and synonyms (thigh↔upleg,
//     calf↔leg, upperarm↔arm, clavicle↔shoulder, …). Each target bone is used
//     at most once.
//   - Named maps save/load as `.bonemap` JSON (schema `qtmesh-bonemap-v1`);
//     two are bundled (`mixamo_to_humanik`, `mixamo_to_unreal`).
//
// The ALGORITHM (`retargetFrames`) works on plain data so it is headless-
// testable, the Ogre adapter (`retarget`) only reads/writes skeletons:
//
//   1. Rest poses: target = its bind pose; source = its bind pose or the
//      clip's first frame (`SourceRest::FirstFrame`, for rigs whose bind pose
//      differs from the animation's frame by an armature rotation).
//   2. Global alignment G (source skeleton space → target skeleton space)
//      from the humanoid frame of each rest pose: up = hips→head, right =
//      left→right upper legs (or upper arms). Absorbs Z-up vs Y-up, ±Z facing
//      and armature rotations. Identity for non-humanoid maps.
//   3. Per mapped bone, the SOURCE WORLD rotation delta from its rest
//      `Δ = S_world(f) · S_rest_world⁻¹` is carried into the target frame
//      and applied to the target rest: `T_world = G·Δ·G⁻¹ · A · T_rest_world`.
//      `A` (optional, `alignDirections`, default on) rotates the target rest
//      bone direction onto the source rest direction, so an A-pose source
//      drives a T-pose target with the arms in the right place. World →
//      local top-down; unmapped target bones keep their bind rotation.
//   4. Translation (`TranslationMode`): None (rotation only — the target's
//      bone lengths exactly), Root (default: the root-most mapped bone's
//      world displacement, scaled by the rigs' height ratio, so the hips move
//      without sliding), All (every mapped bone's local translation delta,
//      scaled — for facial / finger detail).
//
// Output keyframes are relative to the target bind pose, i.e. an ordinary
// skeletal clip the timeline, dope sheet and exporters handle unchanged.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <OgreQuaternion.h>
#include <OgreVector.h>

#include <string>
#include <vector>

namespace Ogre { class Skeleton; class Entity; }

namespace Retarget {

// ---------------------------------------------------------------------------
// Pure data
// ---------------------------------------------------------------------------

/// A skeleton as plain data. Bones are ordered so every parent precedes its
/// children (`parent[i] < i`, -1 for roots); bind values are LOCAL.
struct RigDesc {
    std::vector<std::string> names;
    std::vector<int> parent;
    std::vector<Ogre::Vector3> bindPos;
    std::vector<Ogre::Quaternion> bindRot;
    int size() const { return int(names.size()); }
    int indexOf(const std::string& name) const;
};

/// One frame of LOCAL transforms, one entry per RigDesc bone.
struct LocalPose {
    std::vector<Ogre::Vector3> pos;
    std::vector<Ogre::Quaternion> rot;
};

struct BonePair {
    std::string source;
    std::string target;
};

/// A named source→target bone mapping.
struct BoneMap {
    QString name;
    std::vector<BonePair> pairs;

    /// `qtmesh-bonemap-v1` JSON.
    QByteArray toJson() const;
    static bool fromJson(const QByteArray& json, BoneMap* out, QString* error);
    bool save(const QString& path, QString* error) const;
    static bool load(const QString& path, BoneMap* out, QString* error);

    /// Target bone mapped from `source`, or "" if none.
    std::string targetFor(const std::string& source) const;
    /// Set (or clear, with an empty target) the mapping for `source`. A
    /// target already used by another source is released first, so the map
    /// stays one-to-one.
    void setPair(const std::string& source, const std::string& target);
};

/// Names of the bundled maps (`mixamo_to_humanik`, `mixamo_to_unity` — the
/// same HumanIK names —, `mixamo_to_unreal`).
QStringList bundledBoneMapNames();
/// Fill `out` with a bundled map. False for an unknown name.
bool bundledBoneMap(const QString& name, BoneMap* out);

/// Strip namespaces (`mixamorig:`, `Armature|`) and common rig prefixes
/// (`Bip01 `, `DEF-`, `ORG-`), lowercase, and drop separators.
QString normalizeBoneName(const QString& name);

/// Resolve `wanted` against `names`: exact, then case-insensitive, then
/// equal after `normalizeBoneName`. Returns the matching entry or "".
std::string resolveBoneName(const std::vector<std::string>& names,
                            const std::string& wanted);

/// Rebind a map written for other spellings of the same rigs (a bundled
/// `mixamorig:Hips` map applied to a `mixamorig1:Hips` rig) to the actual
/// bone names. Pairs whose source or target cannot be resolved are dropped
/// and listed in `unresolved` (`source → target`).
BoneMap resolveMap(const BoneMap& map,
                   const std::vector<std::string>& sourceNames,
                   const std::vector<std::string>& targetNames,
                   QStringList* unresolved = nullptr);

struct AutoMapReport {
    BoneMap map;
    int byExactName = 0;
    int byRole = 0;
    int byFuzzy = 0;
    std::vector<std::string> unmappedSource;
    std::vector<std::string> unmappedTarget;
};

/// Build a map from bone names alone (see the file comment for the passes).
AutoMapReport autoMap(const std::vector<std::string>& sourceNames,
                      const std::vector<std::string>& targetNames);

enum class TranslationMode { None, Root, All };
enum class SourceRest { Bind, FirstFrame };

QString translationModeId(TranslationMode m);              // none|root|all
bool translationModeFromId(const QString& id, TranslationMode* out);
QString sourceRestId(SourceRest r);                        // bind|first-frame
bool sourceRestFromId(const QString& id, SourceRest* out);

struct Options {
    TranslationMode translation = TranslationMode::Root;
    bool alignDirections = true;
    SourceRest sourceRest = SourceRest::Bind;
};

struct RetargetReport {
    int mappedBones = 0;          ///< map pairs resolved to bones in both rigs
    bool globalAlignment = false; ///< humanoid frame found → G applied
    float heightScale = 1.0f;     ///< target/source rest height (translations)
    std::string rootTarget;       ///< target bone carrying root translation
    QString error;                ///< set when the call failed
};

/// The retarget core. Returns one LocalPose per source frame for EVERY target
/// bone (unmapped bones keep their bind transform). Empty + `report->error`
/// on bad input (no frames, size mismatch, no resolvable pair).
std::vector<LocalPose> retargetFrames(const RigDesc& source,
                                      const std::vector<LocalPose>& sourceFrames,
                                      const RigDesc& target,
                                      const BoneMap& map,
                                      const Options& options,
                                      RetargetReport* report = nullptr);

/// Forward kinematics helper (also used by tests): world rotations and
/// positions of `pose` on `rig`.
void forwardKinematics(const RigDesc& rig, const LocalPose& pose,
                       std::vector<Ogre::Quaternion>& worldRot,
                       std::vector<Ogre::Vector3>& worldPos);

/// The bind pose of `rig` as a LocalPose.
LocalPose bindPose(const RigDesc& rig);

// ---------------------------------------------------------------------------
// Ogre adapter
// ---------------------------------------------------------------------------

/// Read `skel` into a RigDesc (parents first). `handles` (optional) receives
/// the Ogre bone handle of each RigDesc index.
RigDesc describeSkeleton(Ogre::Skeleton* skel,
                         std::vector<unsigned short>* handles = nullptr);

/// Bone names of `skel`, in RigDesc order.
std::vector<std::string> boneNames(Ogre::Skeleton* skel);

/// Sample skeletal animation `animName` on `skel` at `fps` (both ends
/// included). Leaves the skeleton reset to its bind pose.
bool sampleAnimation(Ogre::Skeleton* skel, const std::string& animName, int fps,
                     std::vector<float>& times, std::vector<LocalPose>& frames,
                     QString* error = nullptr);

struct Result {
    bool ok = false;
    QString error;
    std::string animation;      ///< the created clip
    int frames = 0;
    float length = 0.0f;
    RetargetReport report;
    QStringList unresolvedPairs;
};

/// Retarget `sourceAnim` from `sourceSkel` onto `targetSkel` as a NEW skeletal
/// animation `newAnim` (refused if it already exists). The map is resolved
/// against both rigs first (`resolveMap`). The caller refreshes the target
/// entity's animation states (`Entity::refreshAvailableAnimationState`).
Result retarget(Ogre::Skeleton* sourceSkel, const std::string& sourceAnim,
                Ogre::Skeleton* targetSkel, const std::string& newAnim,
                const BoneMap& map, const Options& options, int fps = 30);

/// `base` when free on `skel`, else `base_2`, `base_3`, …
std::string uniqueAnimationName(Ogre::Skeleton* skel, const std::string& base);

} // namespace Retarget

#endif // ANIMATIONRETARGETER_H
