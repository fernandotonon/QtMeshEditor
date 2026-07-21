#ifndef ANIMATIONMERGER_H
#define ANIMATIONMERGER_H

#include <Ogre.h>
#include <QString>
#include <QList>
#include <array>
#include <vector>

class AnimationMerger {
public:
    AnimationMerger() = delete;

    /// Check if two skeletons have compatible bone hierarchies (matched by name).
    static bool areSkeletonsCompatible(const Ogre::SkeletonPtr& a, const Ogre::SkeletonPtr& b);

    /// Register the coordinate-system up-axis for a named skeleton.
    /// 1 = Y-up (Mixamo/default), 2 = Z-up (Unreal Engine).
    /// Must be called by the importer immediately after loading so that
    /// mergeAnimations() can apply the correct coordinate transform.
    static void registerSkeletonUpAxis(const std::string& skeletonName, int upAxis);

    /// Rename an animation on a skeleton by cloning with a new name and removing the old.
    /// Ogre::Animation has no setName(), so this clone-and-remove pattern is the only way.
    static void renameAnimation(Ogre::Skeleton* skel,
                                const std::string& oldName,
                                const std::string& newName);

    /// Resample an animation to exactly N evenly-spaced keyframes.
    /// Uses interpolation to evaluate T/R/S at each sample point, producing
    /// a smooth curve with a uniform keyframe distribution.
    /// Returns the number of keyframes removed (negative if keyframes were added).
    static int resampleAnimation(Ogre::Skeleton* skel,
                                 const std::string& animName,
                                 int targetKeyframes);

    /// Decimate an animation by keeping every Nth keyframe (plus always the last).
    /// This is a lossy reduction that preserves only the original keyframe data
    /// at the kept indices — no interpolation is performed.
    /// Returns the number of keyframes removed.
    static int decimateAnimation(Ogre::Skeleton* skel,
                                 const std::string& animName,
                                 int step);

    /// Tolerances for redundant-keyframe detection. A keyframe is "redundant"
    /// when removing it leaves the lerp/slerp from its neighbors within tolerance
    /// of the original value. Defaults are the "Conservative" preset — the
    /// safest of the three named presets, near-lossless on meter-scale rigs
    /// (~0.1mm / 0.05° / 0.01% scale). Pick "balanced" or "aggressive" via
    /// tolerancesForPreset() / the CLI / Inspector dropdown when you want
    /// more aggressive reduction at the cost of perceptible drift.
    struct SimplifyTolerances {
        float translation = 1e-4f;     // world units (~0.1mm on meter-scale rigs)
        float rotationDeg = 0.05f;     // degrees of angular drift
        float scale       = 1e-4f;     // unitless multiplier delta
    };

    /// Map a preset name (case-insensitive: "conservative" / "balanced" /
    /// "aggressive") to the corresponding tolerance triple. Single source of
    /// truth shared by the CLI, MCP and Inspector — bumping a preset value in
    /// one place updates every surface. Unknown presets fall back to the
    /// "conservative" defaults (the SimplifyTolerances{} ctor) and `*outOk`
    /// is set to false so callers can surface a usage error.
    static SimplifyTolerances tolerancesForPreset(const std::string& preset,
                                                  bool* outOk = nullptr);

    /// Simplify an animation by removing keyframes that are within tolerance of
    /// the lerp/slerp interpolation between their immediate neighbors. First and
    /// last keyframes are always preserved. Returns the number of keyframes removed.
    static int simplifyAnimation(Ogre::Skeleton* skel,
                                 const std::string& animName,
                                 const SimplifyTolerances& tol);

    /// Overload using default tolerances.
    static int simplifyAnimation(Ogre::Skeleton* skel,
                                 const std::string& animName) {
        return simplifyAnimation(skel, animName, SimplifyTolerances{});
    }

    /// Count redundant keyframes across all tracks of an animation without modifying it.
    /// outOriginal is the total keyframes (sum across tracks); outRedundant is how many
    /// would be removed by simplifyAnimation() with the same tolerances.
    static void analyzeRedundantKeyframes(const Ogre::Animation* anim,
                                          const SimplifyTolerances& tol,
                                          int* outOriginal,
                                          int* outRedundant);

    /// Re-grid every node track in `animName` to a uniform `targetFps`
    /// keyframes-per-second layout. Walks each track's existing
    /// keyframes for evaluation, then replaces the interior with
    /// linearly-interpolated samples at clean 1/fps intervals. Used
    /// by the CLI / MCP / Inspector "Bake @ N FPS" actions for fast
    /// pipeline export at a known cadence. Returns the total number
    /// of keyframes in the animation after baking. No-op (returns 0)
    /// if the animation is missing or has no tracks.
    ///
    /// `targetFps` must be > 0. First and last keyframes of each track
    /// are preserved so the clip duration is unchanged.
    static int bakeAnimationAtFps(Ogre::Skeleton* skel,
                                  const std::string& animName,
                                  int targetFps);

    /// Outcome of inbetweenAnimation — how many keyframes were inserted and
    /// whether the ML model or the spline fallback produced them.
    struct InbetweenResult {
        bool ok = false;
        QString error;
        int keyframesInserted = 0;   // total new keyframes across all tracks
        int tracksAffected    = 0;
        bool usedModel        = false;   // true = RMIB model; false = spline
        QString fallbackReason;          // why the spline ran (if it did)
    };

    /// AI animation in-betweening (#409): for every node track of `animName`,
    /// find the keyframe pair bracketing the window [t0, t1] (the two adjacent
    /// keys whose times straddle the gap) and insert `gapFrames` intermediate
    /// keyframes between them, predicted by MotionInbetween (RMIB ONNX model
    /// when available, deterministic spline fallback otherwise).
    ///
    /// `t0`/`t1` are clip-time seconds; the nearest existing keyframe at or
    /// before t0 and at or after t1 on each track define the segment. Tracks
    /// with fewer than two keyframes, or no key straddling the window, are
    /// skipped. Existing keyframes are preserved; only interior keys are added.
    /// `forceFallback` forces the spline path (CLI `--no-model` / tests).
    ///
    /// The whole skeleton is packed into ONE MotionInbetween call per segment
    /// (channels = bones × 10 DoF) so the model sees the full pose, then the
    /// predicted per-frame poses are scattered back onto each track. Returns an
    /// InbetweenResult; `usedModel`/`fallbackReason` let the caller surface a
    /// "fell back to spline" note (acceptance criterion).
    static InbetweenResult inbetweenAnimation(Ogre::Skeleton* skel,
                                              const std::string& animName,
                                              float t0, float t1,
                                              int gapFrames,
                                              const QString& modelPath,
                                              bool forceFallback = false);

    /// Outcome of applyMotionClip — how the canonical clip mapped onto the rig.
    struct ApplyMotionResult {
        bool ok = false;
        QString error;
        int tracksWritten = 0;     // skeleton bones that mapped to a canonical joint
        int canonicalJoints = 0;   // distinct canonical roles resolved
        int frames = 0;
        float length = 0.0f;       // seconds
        bool refined = false;      // RMIB refine pass ran (smoothed the motion)
        bool usedModel = false;    // RMIB model (vs spline) used in the refine pass
    };

    /// Apply a TEMPLATE MOTION CLIP (#411) onto a skeleton as a new animation.
    /// `clipQuats` is [frame][canonicalJoint] unit quaternions on the 22-joint
    /// canonical CMU skeleton (MotionLibrary::Clip::quats). Each skeleton bone is
    /// mapped to a canonical joint by name via MotionInbetween::canonicalIndexForBone
    /// (the #409 retargeting), and that joint's rotation sequence is written as
    /// keyframes onto the bone's track at 1/fps spacing. Bones with no canonical
    /// role are left unanimated (keep their bind pose). Fails if too few roles
    /// resolve (not a humanoid rig). Rotation only — translation/scale untouched.
    /// `refineWithModel` optionally decimates to every `refineStride`-th
    /// keyframe and RMIB-in-betweens the gaps (#409 model). DEFAULT OFF: the
    /// template clips are real mocap and already temporally smooth (≈26× lower
    /// frame-to-frame jerk than a decimate+refill), so the refine only adds
    /// jitter at the sparse keyframe boundaries — measured, not assumed. Kept as
    /// an opt-in for callers that start from a noisier source. Best-effort.
    /// `worldFrame` selects how `clipQuats` is interpreted:
    ///   * true  (schema v3): each quat is the joint's WORLD-space orientation.
    ///     The retarget takes a clean world delta vs frame 0,
    ///     `dWorld(f) = clip(f)·clip(0)⁻¹`, and transports it into the target
    ///     bone's parent-world frame — basis-independent, so it carries the true
    ///     per-bone roll with NO arm-twist. This is the preferred path.
    ///   * false (schema v1/v2): each quat is the joint's LOCAL parent-relative
    ///     rotation; `cmuRestWorld` (optional, 22 world-rest quats) provides the
    ///     CMU↔target change-of-basis. Empty → parent-world transport only
    ///     (direction-correct, residual roll). Kept for back-compat.
    static ApplyMotionResult applyMotionClip(
        Ogre::Skeleton* skel,
        const std::string& animName,
        const std::vector<std::vector<std::array<float, 4>>>& clipQuats,
        int fps,
        bool worldFrame = false,
        const std::vector<std::array<float, 4>>& cmuRestWorld = {},
        bool refineWithModel = false,
        int refineStride = 8,
        bool yaw180 = false,
        const std::vector<std::array<float, 3>>& clipRestDir = {},
        // #837: tight per-role twist caps damp the from-scratch MODEL's noisy
        // roll (flailing arms / thrown-back head). Authored template + self-
        // parity clips carry legitimate large roll (arms up to 180°); capping
        // them collapses real motion (measured: mouse elbow 180°→124°, total
        // parity 5.1°→3.1° with caps off). So default = relaxed; the model
        // path passes modelClip=true to re-enable the tight caps.
        bool modelClip = false);

    /// One skeletal animation extracted onto the 22-joint canonical skeleton
    /// (#839, the REVERSE of applyMotionClip's world-frame path): per frame,
    /// per canonical role, the source bone's WORLD orientation — exactly the
    /// "frame":"world" convention the v3 motion library stores, so extracted
    /// clips ride the existing retarget unchanged (delta vs clip frame 0).
    struct CanonicalClip {
        QString animation;      ///< source animation name
        int frames = 0;         ///< sampled frame count at `fps`
        int resolvedRoles = 0;  ///< canonical roles matched on this rig (≤22)
        /// frames × 22 × [x,y,z,w]; unresolved roles hold identity.
        std::vector<std::vector<std::array<float, 4>>> quats;
        /// The SOURCE rig's REFERENCE world orientation per canonical role,
        /// measured at the clip's calmest ANIMATED frame (same conjugated
        /// frame as `quats` — never the bind/reset pose, which on many
        /// scraped rigs differs from the animation worlds by a constant
        /// global armature rotation). Enables the bind-referenced retarget
        /// onto the TARGET's bind — no pose from any other target animation
        /// is ever involved. 22 × [x,y,z,w]; identity for unresolved roles.
        std::vector<std::array<float, 4>> restWorld;
        /// Canonical-frame bone directions per role at the same reference
        /// frame (unit vectors, canonical topology: role → its canonical
        /// child joint; leaf roles use the incoming direction). Combined
        /// with restWorld this gives the source bone's constant LOCAL
        /// direction axis; the target computes its own bind directions the
        /// same way, so every retargeted bone POINTS where the source bone
        /// points each frame. 22 × [x,y,z]; zero for unresolved roles.
        std::vector<std::array<float, 3>> restDir;
    };

    /// Post-process a generated animation to widen (+) or tuck (−) the arm
    /// chains, à la Mixamo's "Character Arm-Space" (#854). Rescues arm-into-
    /// torso clipping / too-wide arms on rigs whose proportions differ from
    /// the source clip, without touching the retarget math.
    ///
    /// Mechanics: for each bone mapped to a shoulder role (canonical 7 right,
    /// 11 left) — plus a fractional share on the collars (6/10) — a swing of
    /// `degrees` about the torso's FORWARD axis (from the target bind frame,
    /// mirrored per side: + swings both arms away from the body) is injected
    /// into WORLD space and folded back into the bone's local keyframe deltas.
    /// Elbows/hands inherit through the hierarchy (keyframes are parent-
    /// relative), so only the shoulders/collars are rewritten. The full angle
    /// is split across duplicate role bones so multi-segment shoulders don't
    /// over-rotate.
    ///
    /// ABSOLUTE + IDEMPOTENT: `degrees` is the target angle, not a nudge. The
    /// last-applied angle is tracked PER SKELETON INSTANCE on bone[0]'s
    /// UserObjectBindings (key "qtme.armspace.<anim>") — isolated per entity,
    /// never a process-global, and NOT persisted to disk (export bakes the
    /// final keyframes). Each call reverts the stored angle before applying
    /// the new one (delta = new − stored), so `adjustArmSpace(20)` then
    /// `adjustArmSpace(10)` == `adjustArmSpace(10)` from the original, and
    /// `adjustArmSpace(0)` restores the clip bit-near-exactly. NB: the binding
    /// is in-memory only, so a fresh CLI process (which loads the baked clip)
    /// sees `stored == 0` — calling `adjustArmSpace(0)` there is a no-op.
    /// Use currentArmSpace() to read the tracked value; applyMotionClip clears
    /// it when it regenerates a clip, and migrateArmSpaceKey() moves it on
    /// rename. Returns false (no-op) if the animation is missing or no arm
    /// role resolves on the rig.
    static bool adjustArmSpace(Ogre::Skeleton* skel,
                               const std::string& animName,
                               float degrees);

    /// The arm-space angle currently applied to `animName` (0 if none) — the
    /// value adjustArmSpace last stored this session. Lets a UI seed its
    /// slider with the clip's real state instead of assuming 0.
    static float currentArmSpace(Ogre::Skeleton* skel,
                                 const std::string& animName);

    /// Move the tracked arm-space angle from oldAnim to newAnim on the given
    /// skeleton. Call from every animation-rename path so a renamed clip keeps
    /// its widen/tuck value (the keyframes carry over; the tracked angle must
    /// too, or the next slider drag mis-computes its delta). No-op if there was
    /// no tracked angle or the names match.
    static void migrateArmSpaceKey(Ogre::Skeleton* skel,
                                   const std::string& oldAnim,
                                   const std::string& newAnim);

    /// #837 quality post-pass: re-grid the animation to a SPARSE keyframe
    /// rate, then back to `targetFps` — a temporal low-pass that removes
    /// retarget jitter ("trembling") while preserving the silhouette and the
    /// clip length (both passes keep endpoints). Codifies the field-proven
    /// trick of baking sparse and re-baking at 30 FPS. Returns the final
    /// keyframe count (0 = animation missing / invalid fps).
    static int smoothBakeAnimation(Ogre::Skeleton* skel,
                                   const std::string& animName,
                                   int sparseFps = 12, int targetFps = 30);

    /// Outcome of pinFeet.
    struct FootPinResult {
        bool ok = false;
        QString error;
        int spans = 0;            ///< contact spans pinned (both feet)
        int keyframesAdjusted = 0;
    };

    /// #856 — foot-contact cleanup. Retargeted clips slide/float feet on rigs
    /// whose proportions differ from the source (the direction retarget
    /// transfers bone DIRECTIONS, not world foot positions). Per foot role,
    /// detect contact spans (foot near the clip's ground level AND nearly
    /// stationary horizontally — FootContact::detectContacts, canonical-frame,
    /// leg-length-scaled thresholds) and lock the foot's world position to its
    /// span-start position with an analytic two-bone hip–knee–foot IK
    /// (FootContact::solveKnee — keeps segment lengths and the pose's own
    /// bend plane), blending in/out over `blendFrames` at span edges so knees
    /// don't pop. Rewrites ONLY the thigh/shin/foot keyframes (foot keeps its
    /// original world orientation); everything else untouched. Pure track
    /// math — nothing is applied to the live skeleton.
    ///
    /// Effectively idempotent: a second run detects the already-planted spans
    /// and re-pins to the same targets (near-no-op). The application is
    /// recorded on bone[0]'s UserObjectBindings ("qtme.footpin.<anim>") so a
    /// UI can reflect state; applyMotionClip clears it on clip regeneration.
    /// Designed for generated clips (dense uniform keyframes); sparse
    /// authored clips get keyframe-rate detection (approximate).
    static FootPinResult pinFeet(Ogre::Skeleton* skel,
                                 const std::string& animName,
                                 int blendFrames = 3);

    /// Sample every (or one) skeletal animation of `entity` at `fps` and
    /// express each canonical joint's world orientation per frame. Bone→role
    /// mapping is MotionInbetween::canonicalIndexForBone — the same matcher
    /// the retarget uses, so extraction and application are consistent by
    /// construction. Animations whose rig resolves 0 roles are skipped.
    static std::vector<CanonicalClip> extractCanonicalClips(
        Ogre::Entity* entity,
        int fps = 30,
        const QString& onlyAnimation = {});

    /// True when the entity's mesh appears to FACE −Z (the retarget and the
    /// CMU clips assume +Z): detected from the foot region — toe mass extends
    /// forward of the ankle joints. Rigs WITH a harvested standing pose don't
    /// need this (the world-frame change-of-basis absorbs facing), but
    /// auto-rigged skeletons (UniRig / template, no prior animation) apply
    /// motion axes raw — a −Z-facing mesh walks BACKWARD unless the caller
    /// passes yaw180 = detectBackwardFacing(entity) to applyMotionClip.
    /// Conservative: returns false when feet/geometry are inconclusive.
    static bool detectBackwardFacing(Ogre::Entity* entity);

    /// Merge animations from sourceEntities into baseEntity's skeleton.
    /// Convenience wrapper; forwards an empty skeleton list to the 4-argument overload.
    static Ogre::Entity* mergeAnimations(
        Ogre::Entity* baseEntity,
        const QList<Ogre::Entity*>& sourceEntities,
        QString& errorMsg);

    /// Full overload: merges from entity sources AND standalone skeletons
    /// (e.g. from animation-only files that produce no mesh entity).
    /// Base entity's own animations are kept as-is. Source animations are named
    /// after their scene node / skeleton name.
    /// Returns the base entity on success, nullptr on error.
    static Ogre::Entity* mergeAnimations(
        Ogre::Entity* baseEntity,
        const QList<Ogre::Entity*>& sourceEntities,
        const QList<Ogre::SkeletonPtr>& sourceSkeletons,
        QString& errorMsg);
};

#endif // ANIMATIONMERGER_H
