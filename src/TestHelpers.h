#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>

/**
 * Ensures that Ogre's MaterialManager has been initialised.
 *
 * In normal application startup this happens inside
 * Ogre::Root::oneTimePostWindowInit() -- which is only called after the
 * first RenderWindow is created.  Headless unit tests never create a
 * window, so the default material settings (and the built-in BaseWhite /
 * BaseWhiteNoLighting materials) are missing.  Without them every
 * Material::create() produces a material with an empty technique list and
 * any call to getTechnique(0) throws std::out_of_range.
 *
 * Call this helper once before creating or querying Ogre materials in
 * test fixtures.
 */
static inline void ensureMaterialManagerInitialised()
{
    if (!Ogre::MaterialManager::getSingleton().getDefaultSettings())
    {
        Ogre::MaterialManager::getSingleton().initialise();
    }
}

/**
 * Creates the BaseWhiteNoLighting and BaseWhite materials that many parts
 * of QtMeshEditor expect to exist.
 *
 * Automatically calls ensureMaterialManagerInitialised() first so that
 * newly created materials receive the default technique/pass.
 */
static inline void createStandardOgreMaterials()
{
    ensureMaterialManagerInitialised();

    Ogre::MaterialPtr baseWhiteMat = Ogre::MaterialManager::getSingleton().getByName(
        "BaseWhiteNoLighting", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!baseWhiteMat)
    {
        baseWhiteMat = Ogre::MaterialManager::getSingleton().create(
            "BaseWhiteNoLighting", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        baseWhiteMat->getTechnique(0)->getPass(0)->setDiffuse(1, 1, 1, 1);
        baseWhiteMat->getTechnique(0)->getPass(0)->setAmbient(1, 1, 1);
        baseWhiteMat->getTechnique(0)->getPass(0)->setSelfIllumination(1, 1, 1);
        baseWhiteMat->getTechnique(0)->setLightingEnabled(false);
    }

    Ogre::MaterialPtr baseWhiteMat2 = Ogre::MaterialManager::getSingleton().getByName(
        "BaseWhite", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!baseWhiteMat2)
    {
        baseWhiteMat2 = Ogre::MaterialManager::getSingleton().create(
            "BaseWhite", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        baseWhiteMat2->getTechnique(0)->getPass(0)->setDiffuse(1, 1, 1, 1);
        baseWhiteMat2->getTechnique(0)->getPass(0)->setAmbient(1, 1, 1);
    }
}

#endif // TEST_HELPERS_H
