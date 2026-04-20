#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>
#include <OgreRoot.h>
#include <OgreException.h>
#include <OgreStringConverter.h>
#include <QGuiApplication>
#include <QWidget>
#include <QFile>
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
 * Follows the same pattern used in CLIPipeline for headless CLI mode.
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

/**
 * Creates an in-memory triangle mesh with positions, normals, and UVs.
 *
 * The mesh has 3 vertices and 1 triangle. This is the simplest possible
 * mesh that can be used to create an Ogre::Entity without loading from disk.
 *
 * Returns a MeshPtr. Call canLoadMeshFiles() before using this — it needs
 * a GL context for hardware buffer creation.
 */
static inline Ogre::MeshPtr createInMemoryTriangleMesh(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;

    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float verts[] = {
        0,0,0,   0,0,1,  0.0f,0.0f,
        1,0,0,   0,0,1,  1.0f,0.0f,
        0,1,0,   0,0,1,  0.0f,1.0f,
    };
    vbuf->writeData(0, sizeof(verts), verts);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 3;

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;

    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,1,1,1));
    mesh->_setBoundingSphereRadius(2.0);
    mesh->load();

    return mesh;
}

/**
 * Creates an in-memory welded cube mesh (8 verts, 12 tris). Matches the
 * topology the primitive-cube post-process produces at runtime — useful
 * for end-to-end bevel tests that go through the full editor pipeline.
 */
static inline Ogre::MeshPtr createInMemoryWeldedCube(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    sub->useSharedVertices = false;
    sub->vertexData = new Ogre::VertexData();
    auto* decl = sub->vertexData->vertexDeclaration;

    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 8, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    // (pos x3, normal x3, uv x2) = 8 floats per vertex
    float verts[] = {
        -1,-1,-1,  0,0,-1,  0,0,   // v0
         1,-1,-1,  0,0,-1,  1,0,   // v1
        -1, 1,-1,  0,0,-1,  0,1,   // v2
         1, 1,-1,  0,0,-1,  1,1,   // v3
        -1, 1, 1,  0,1, 0,  0,0,   // v4
         1, 1, 1,  0,1, 0,  1,0,   // v5
        -1,-1, 1,  0,-1,0,  0,1,   // v6
         1,-1, 1,  0,-1,0,  1,1,   // v7
    };
    vbuf->writeData(0, sizeof(verts), verts);
    sub->vertexData->vertexBufferBinding->setBinding(0, vbuf);
    sub->vertexData->vertexCount = 8;

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 12 * 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {
        0,2,1,  1,2,3,       // back
        4,6,5,  5,6,7,       // front
        6,0,7,  7,0,1,       // bottom
        2,4,3,  3,4,5,       // top
        2,0,4,  4,0,6,       // left
        1,3,7,  7,3,5,       // right
    };
    ibuf->writeData(0, sizeof(idx), idx);
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 36;

    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,1,1,1));
    mesh->_setBoundingSphereRadius(2.0);
    mesh->load();

    return mesh;
}

/**
 * Creates an in-memory mesh with a skeleton (2 bones: root + child)
 * and bone assignments. Does NOT include animations.
 *
 * Returns a MeshPtr linked to its skeleton. The skeleton name is
 * "<name>_skel".
 */
static inline Ogre::MeshPtr createInMemorySkeletonMesh(const std::string& name)
{
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        name + "_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* rootBone = skel->createBone("Root", 0);
    rootBone->setPosition(Ogre::Vector3(0, 0, 0));

    auto* childBone = skel->createBone("Child", 1);
    childBone->setPosition(Ogre::Vector3(0, 1, 0));
    rootBone->addChild(childBone);

    skel->setBindingPose();

    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float verts[] = {0,0,0, 1,0,0, 0,1,0};
    vbuf->writeData(0, sizeof(verts), verts);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 3;

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;

    Ogre::VertexBoneAssignment vba;
    vba.boneIndex = 1;
    vba.weight = 1.0f;
    for (unsigned short v = 0; v < 3; ++v) {
        vba.vertexIndex = v;
        mesh->addBoneAssignment(vba);
    }

    mesh->_notifySkeleton(skel);
    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,2,2,2));
    mesh->_setBoundingSphereRadius(3.0);
    mesh->load();

    return mesh;
}

/**
 * Creates an in-memory mesh with a skeleton (2 bones: Root + Child),
 * bone assignments, and a "TestAnim" animation with 3 keyframes at
 * t=0.0, t=0.5, and t=1.0 seconds.
 *
 * The animation is on the Child bone (track handle = 1).
 *
 * Also creates an Ogre::Entity and attaches it to a new scene node
 * via Manager::addSceneNode(). The entity name is "<name>" and has
 * a valid SkeletonInstance with animation states.
 *
 * Returns the Entity, or nullptr on failure.
 */
static inline Ogre::Entity* createAnimatedTestEntity(const std::string& name)
{
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        name + "_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* rootBone = skel->createBone("Root", 0);
    rootBone->setPosition(Ogre::Vector3(0, 0, 0));

    auto* childBone = skel->createBone("Child", 1);
    childBone->setPosition(Ogre::Vector3(0, 1, 0));
    rootBone->addChild(childBone);

    skel->setBindingPose();

    // Create animation "TestAnim" with 3 keyframes
    auto* anim = skel->createAnimation("TestAnim", 1.0f);
    auto* track = anim->createNodeTrack(1);
    track->setAssociatedNode(childBone);

    auto* kf0 = track->createNodeKeyFrame(0.0f);
    kf0->setTranslate(Ogre::Vector3::ZERO);
    kf0->setRotation(Ogre::Quaternion::IDENTITY);
    kf0->setScale(Ogre::Vector3::UNIT_SCALE);

    auto* kf1 = track->createNodeKeyFrame(0.5f);
    kf1->setTranslate(Ogre::Vector3(0.5f, 0, 0));
    kf1->setRotation(Ogre::Quaternion(Ogre::Radian(Ogre::Degree(30)),
                                       Ogre::Vector3::UNIT_Y));
    kf1->setScale(Ogre::Vector3::UNIT_SCALE);

    auto* kf2 = track->createNodeKeyFrame(1.0f);
    kf2->setTranslate(Ogre::Vector3::ZERO);
    kf2->setRotation(Ogre::Quaternion::IDENTITY);
    kf2->setScale(Ogre::Vector3::UNIT_SCALE);

    // Create mesh with position and normal data (normals are required by
    // NormalVisualizer::buildOverlayForEntity to produce an overlay).
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name + "_mesh", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    size_t offset = 0;
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl->addElement(0, offset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);

    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    // position (x,y,z) + normal (nx,ny,nz) per vertex
    float verts[] = {
        0,0,0,  0,0,1,
        1,0,0,  0,0,1,
        0,1,0,  0,0,1,
    };
    vbuf->writeData(0, sizeof(verts), verts);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 3;

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;

    Ogre::VertexBoneAssignment vba;
    vba.boneIndex = 1;
    vba.weight = 1.0f;
    for (unsigned short v = 0; v < 3; ++v) {
        vba.vertexIndex = v;
        mesh->addBoneAssignment(vba);
    }

    mesh->_notifySkeleton(skel);
    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,2,2,2));
    mesh->_setBoundingSphereRadius(3.0);
    mesh->load();

    // Create entity via Manager
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode(name.c_str());
    auto* entity = sceneMgr->createEntity(name, mesh);
    node->attachObject(entity);

    return entity;
}

/**
 * Writes a test XML file with the given content.
 * Returns true if the file was written successfully.
 */
static inline bool writeTestXMLFile(const QString& path, const QByteArray& content)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (f.write(content) != content.size())
        return false;
    f.close();
    return true;
}

#endif // TEST_HELPERS_H
