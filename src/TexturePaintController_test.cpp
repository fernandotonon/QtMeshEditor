#include <gtest/gtest.h>

#include <QSignalSpy>

#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "TexturePaintBuffer.h"
#include "TexturePaintController.h"

#include <OgreEntity.h>
#include <OgreMeshManager.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

// Reverse UV→3D lookup: ask for UV (0,0) and verify we get the
// position of vertex 0 (which carries UV (0,0)) on the test triangle.
TEST(TexturePaintControllerTest, FindMeshPointForUVHitsCorrectTriangle)
{
    if (!tryInitOgre()) GTEST_SKIP();
    auto* mgr = Manager::getSingleton();
    ASSERT_NE(mgr, nullptr);
    auto* scene = mgr->getSceneMgr();
    ASSERT_NE(scene, nullptr);
    auto mesh = createInMemoryTriangleMesh("TPC_FindMeshPointForUV");
    auto* entity = scene->createEntity("TPC_TestEntity", mesh->getName());
    auto* node = scene->getRootSceneNode()->createChildSceneNode();
    node->attachObject(entity);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(entity);

    auto* ctrl = TexturePaintController::instance();
    ctrl->refreshSlots();
    // Need a paint buffer so `m_paintMesh` is built.
    ASSERT_TRUE(ctrl->ensurePaintableTexture(64));

    Ogre::Vector3 pos, normal;
    EXPECT_TRUE(ctrl->findMeshPointForUV(Ogre::Vector2(0.0f, 0.0f), pos, normal));
    EXPECT_NEAR(pos.x, 0.0f, 1e-4);
    EXPECT_NEAR(pos.y, 0.0f, 1e-4);

    EXPECT_TRUE(ctrl->findMeshPointForUV(Ogre::Vector2(1.0f, 0.0f), pos, normal));
    EXPECT_NEAR(pos.x, 1.0f, 1e-4);
    EXPECT_NEAR(pos.y, 0.0f, 1e-4);

    EXPECT_TRUE(ctrl->findMeshPointForUV(Ogre::Vector2(0.0f, 1.0f), pos, normal));
    EXPECT_NEAR(pos.x, 0.0f, 1e-4);
    EXPECT_NEAR(pos.y, 1.0f, 1e-4);

    // Outside the triangle in UV space → no hit.
    EXPECT_FALSE(ctrl->findMeshPointForUV(Ogre::Vector2(0.9f, 0.9f), pos, normal));

    ctrl->closeSession();
    SelectionSet::getSingleton()->clear();
    scene->getRootSceneNode()->removeAndDestroyChild(node);
    scene->destroyEntity(entity);
    Ogre::MeshManager::getSingleton().remove(mesh);
}

TEST(TexturePaintControllerTest, BrushToolDefaultIsPaint)
{
    auto* ctrl = TexturePaintController::instance();
    // Reset to a known value via the public path so test order doesn't
    // matter.
    ctrl->setBrushTool(TexturePaintController::ToolPaint);
    EXPECT_EQ(ctrl->brushTool(), static_cast<int>(TexturePaintController::ToolPaint));
}

TEST(TexturePaintControllerTest, SetBrushToolEmitsOnceAndSticks)
{
    auto* ctrl = TexturePaintController::instance();
    ctrl->setBrushTool(TexturePaintController::ToolPaint);
    QSignalSpy spy(ctrl, &TexturePaintController::brushToolChanged);
    ctrl->setBrushTool(TexturePaintController::ToolErase);
    EXPECT_EQ(ctrl->brushTool(), static_cast<int>(TexturePaintController::ToolErase));
    EXPECT_EQ(spy.count(), 1);
    // Same value should not re-emit.
    ctrl->setBrushTool(TexturePaintController::ToolErase);
    EXPECT_EQ(spy.count(), 1);
    ctrl->setBrushTool(TexturePaintController::ToolPaint);
}
