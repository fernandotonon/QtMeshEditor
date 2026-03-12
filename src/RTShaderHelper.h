#pragma once

#include <Ogre.h>

namespace RTShaderHelper {
    void initialize(Ogre::SceneManager* sceneMgr);
    void shutdown(Ogre::SceneManager* sceneMgr);
    void applyNormalMap(Ogre::MaterialPtr& mat, const std::string& normalMapTexName);
}
