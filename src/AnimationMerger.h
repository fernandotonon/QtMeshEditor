#ifndef ANIMATIONMERGER_H
#define ANIMATIONMERGER_H

#include <Ogre.h>
#include <QString>
#include <QList>

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
