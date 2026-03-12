// Stub implementation of RTShaderHelper for the test binary.
// The real implementation (RTShaderHelper.cpp) includes OgreRTShaderSystem.h,
// whose framework loading interferes with bare Ogre::Root() in unit tests.
#include "RTShaderHelper.h"

void RTShaderHelper::initialize(Ogre::SceneManager*) {}
void RTShaderHelper::shutdown(Ogre::SceneManager*) {}
void RTShaderHelper::applyNormalMap(Ogre::MaterialPtr&, const std::string&) {}
