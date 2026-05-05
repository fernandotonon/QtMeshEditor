#ifndef SKELETON_RESOLVER_H
#define SKELETON_RESOLVER_H

#include <string>

namespace Ogre {
    class SkeletonInstance;
    class Entity;
}

/**
 * Resolves a SkeletonInstance from an entity name at apply-time.
 *
 * Used by undo commands that need a skeleton handle but cannot safely
 * hold a raw pointer across the command's lifetime: entities can be
 * deleted, reloaded, or have their skeletons rebuilt before undo/redo
 * runs. Storing the entity's name and re-resolving on each apply avoids
 * dangling-pointer use-after-free.
 *
 * Returns nullptr when the entity is gone or has no skeleton — callers
 * should treat that as "command no-op" rather than a hard error.
 */
namespace SkeletonResolver {

/// Resolves the SkeletonInstance for an entity by name. Walks the scene
/// via Manager::getEntities() to find the entity, then returns its
/// skeleton. nullptr if entity / skeleton is gone.
Ogre::SkeletonInstance* resolve(const std::string& entityName);

/// Convenience: get the entity's name from a live skeleton instance,
/// for callers that capture the command at push time. Returns "" when
/// no entity in the scene owns this skeleton.
std::string entityNameForSkeleton(Ogre::SkeletonInstance* skel);

} // namespace SkeletonResolver

#endif // SKELETON_RESOLVER_H
