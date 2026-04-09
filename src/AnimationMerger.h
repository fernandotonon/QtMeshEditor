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
