#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreException.h>
#include <QGuiApplication>
#include "Manager.h"

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
 * Creates the BaseWhiteNoLighting, BaseWhite, and GUI_Material materials
 * that many parts of QtMeshEditor expect to exist.
 *
 * Automatically calls ensureMaterialManagerInitialised() first so that
 * newly created materials receive the default technique/pass.
 *
 * In normal app startup, GUI_Material is created by Manager::loadResources()
 * which is called from MainWindow.  Tests never create a MainWindow, so
 * we create it here.
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

    // GUI_Material — used by ViewportGrid, gizmos, and SelectionBoxObject
    Ogre::MaterialPtr guiMat = Ogre::MaterialManager::getSingleton().getByName(
        "GUI_Material", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!guiMat)
    {
        guiMat = Ogre::MaterialManager::getSingleton().create(
            "GUI_Material", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        guiMat->getTechnique(0)->setLightingEnabled(false);
        guiMat->getTechnique(0)->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        guiMat->getTechnique(0)->setDepthCheckEnabled(false);
    }
}

/**
 * Safely initializes Ogre via Manager::getSingleton().
 *
 * Catches Ogre::Exception (e.g., no render system found when plugins
 * are missing) and returns false instead of letting the exception
 * propagate and crash the test.
 *
 * Note: On macOS, if plugins ARE found but GL context creation fails,
 * the crash is a SIGSEGV (not a C++ exception) and cannot be caught
 * here. On Linux CI with Xvfb, GL context creation succeeds.
 *
 * Returns true if Ogre initialized successfully, false otherwise.
 * Test fixtures should call this and GTEST_SKIP() on false.
 */
static inline bool tryInitOgre()
{
    // Already initialized — nothing to do
    if (Manager::getSingletonPtr())
        return true;

    try {
        Manager::getSingleton();
        return true;
    } catch (const Ogre::Exception&) {
        return false;
    } catch (...) {
        return false;
    }
}

/**
 * Returns true if the environment supports creating entities from meshes.
 *
 * Creating entities requires Ogre hardware buffers which need a GL context.
 * In headless CI (Xvfb + Mesa), Manager::getSingleton() initialises Ogre
 * with `mRoot->initialise(false)` — no RenderWindow is created, so no GL
 * context exists.  Any attempt to create an Entity, ManualObject, or
 * realize a procedural mesh will SIGSEGV (not a C++ exception).
 *
 * Tests that create entities or meshes should call this and GTEST_SKIP()
 * if false.
 */
static inline bool canLoadMeshFiles()
{
    // Offscreen Qt platform typically has no real GPU context
    if (QGuiApplication::platformName() == "offscreen")
        return false;
    // No render system means Ogre can't load materials
    if (!Ogre::Root::getSingletonPtr() || !Ogre::Root::getSingleton().getRenderSystem())
        return false;
    // Without a RenderWindow there is no GL context — entity creation
    // will crash with SIGSEGV (can't be caught by try/catch).
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    if (!Ogre::Root::getSingleton().getAutoCreatedWindow())
        return false;
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    return true;
}

#endif // TEST_HELPERS_H
