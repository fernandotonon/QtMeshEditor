#include "SkeletonResolver.h"

#include "Manager.h"

#include <OgreEntity.h>
#include <OgreSkeletonInstance.h>

namespace SkeletonResolver {

Ogre::SkeletonInstance* resolve(const std::string& entityName)
{
    if (entityName.empty()) return nullptr;
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    for (Ogre::Entity* ent : mgr->getEntities()) {
        if (!ent) continue;
        if (ent->getMovableType() != "Entity") continue;
        if (ent->getName() != entityName) continue;
        if (!ent->hasSkeleton()) return nullptr;
        return ent->getSkeleton();
    }
    return nullptr;
}

std::string entityNameForSkeleton(Ogre::SkeletonInstance* skel)
{
    if (!skel) return {};
    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return {};
    for (Ogre::Entity* ent : mgr->getEntities()) {
        if (!ent) continue;
        if (ent->getMovableType() != "Entity") continue;
        if (ent->hasSkeleton() && ent->getSkeleton() == skel)
            return ent->getName();
    }
    return {};
}

} // namespace SkeletonResolver
