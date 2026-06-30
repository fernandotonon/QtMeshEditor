#pragma once

#include <OgreRTShaderSystem.h>

namespace HdrIblRtss {

/// RTSS type string for the QtMeshEditor split-sum IBL sub-render state.
extern const Ogre::String SRS_QTME_HDR_IBL;

/// Per-pass user-binding keys (slice C).
extern const char* kPbrEnvIntensityKey;
extern const char* kPbrEnvTintKey;

void registerFactory();
void unregisterFactory();

float readEnvIntensity(const Ogre::Pass* pass);
Ogre::ColourValue readEnvTint(const Ogre::Pass* pass);

} // namespace HdrIblRtss
