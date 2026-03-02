#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreException.h>
#include <OgreStringConverter.h>
#include <QGuiApplication>
#include <QWidget>
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
 * Creates a hidden 1x1 QWidget and uses its native window handle to
 * create an Ogre RenderWindow named "TestHidden".  This provides the
 * GL context that Ogre needs for hardware buffer operations (creating
 * entities, loading meshes, etc.) without requiring a visible window.
 *
 * Follows the same pattern used in main.cpp for merge-animations CLI.
 *
 * Returns true if a render window already exists or was created
 * successfully, false on failure.
 */
static inline bool createTestRenderWindow()
{
    auto* root = Ogre::Root::getSingletonPtr();
    if (!root || !root->getRenderSystem())
        return false;

    // Already have a render window
    try {
        if (root->getRenderTarget("TestHidden"))
            return true;
    } catch (...) {
        // getRenderTarget throws if not found in some Ogre versions
    }

    static QWidget* hiddenWidget = nullptr;
    if (!hiddenWidget) {
        hiddenWidget = new QWidget();
        hiddenWidget->setAttribute(Qt::WA_DontShowOnScreen);
        hiddenWidget->resize(1, 1);
        hiddenWidget->show();
    }
    try {
        Ogre::NameValuePairList params;
        params["externalWindowHandle"] = Ogre::StringConverter::toString(
            static_cast<unsigned long>(hiddenWidget->winId()));
#ifdef Q_OS_MACOS
        params["macAPI"] = "cocoa";
        params["macAPICocoaUseNSView"] = "true";
#endif
        root->createRenderWindow("TestHidden", 1, 1, false, &params);
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * Safely initializes Ogre via Manager::getSingleton() and creates a
 * hidden render window for GL context.
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
    // Already initialized — ensure render window exists
    if (Manager::getSingletonPtr()) {
        createTestRenderWindow();
        return true;
    }

    try {
        Manager::getSingleton();
        createTestRenderWindow();
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
 * A GL context is available when either a RenderWindow has been created
 * (via createTestRenderWindow() or OgreWidget) or Ogre auto-created one.
 *
 * Tests that create entities or meshes should call this and GTEST_SKIP()
 * if false.
 */
static inline bool canLoadMeshFiles()
{
    auto* root = Ogre::Root::getSingletonPtr();
    if (!root || !root->getRenderSystem())
        return false;
    // Check for our test render window
    try {
        if (root->getRenderTarget("TestHidden"))
            return true;
    } catch (...) {}
    // Check for auto-created window (legacy path)
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
    if (root->getAutoCreatedWindow())
        return true;
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    return false;
}

#endif // TEST_HELPERS_H
