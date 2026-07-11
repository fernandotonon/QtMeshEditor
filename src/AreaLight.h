#pragma once

#include "LightManager.h"

#include <QString>

namespace AreaLight
{

inline constexpr const char* kProxyLightTag = "area_light_proxy";
inline constexpr const char* kProxyOwnerKey = "area_light_owner";

bool isProxyLight(const Ogre::Light* light);
QString ownerLightName(const Ogre::Light* light);

/// Rebuild child point-light proxies for an area-shaped user light.
void syncProxies(const LightHandle& owner, const LightSnapshot& snapshot);

/// Remove every proxy owned by @p ownerLightName.
void removeProxies(const QString& ownerLightName);

} // namespace AreaLight
