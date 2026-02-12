#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <OgreException.h>
#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>
#include "Manager.h"
#include "MeshTransform.h"
#include "PrimitiveObject.h"

static void createOGREMaterials()
{
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

// Helper to read vertex positions from a mesh for verification
static std::vector<Ogre::Vector3> getVertexPositions(Ogre::Mesh* mesh)
{
    std::vector<Ogre::Vector3> positions;
    bool added_shared = false;

    for (int i = 0; i < mesh->getNumSubMeshes(); ++i)
    {
        Ogre::SubMesh* submesh = mesh->getSubMesh(i);
        Ogre::VertexData* vertex_data = submesh->useSharedVertices ? mesh->sharedVertexData : submesh->vertexData;

        if ((!submesh->useSharedVertices) || (submesh->useSharedVertices && !added_shared))
        {
            if (submesh->useSharedVertices)
                added_shared = true;

            const Ogre::VertexElement* posElem =
                vertex_data->vertexDeclaration->findElementBySemantic(Ogre::VES_POSITION);
            Ogre::HardwareVertexBufferSharedPtr vbuf =
                vertex_data->vertexBufferBinding->getBuffer(posElem->getSource());

            unsigned char* vertex = static_cast<unsigned char*>(
                vbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
            Ogre::Real* pReal;

            for (size_t j = 0; j < vertex_data->vertexCount; ++j, vertex += vbuf->getVertexSize())
            {
                posElem->baseVertexPointerToElement(vertex, &pReal);
                Ogre::Vector3 pos;
                pos.x = pReal[0];
                pos.y = pReal[1];
                pos.z = pReal[2];
                positions.push_back(pos);
            }
            vbuf->unlock();
        }
    }
    return positions;
}

class MeshTransformTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        try {
            Manager::getSingleton();
        } catch (const Ogre::Exception& e) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
        }
        createOGREMaterials();
    }
    void TearDown() override {
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(50);
    }
};

// ------------------------------------------------------------------
// scaleMesh tests
// ------------------------------------------------------------------

TEST_F(MeshTransformTest, ScaleMeshUniformlyDoubles)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("ScaleUniformCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    // Record original vertex positions
    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Scale uniformly by 2x
    MeshTransform::scaleMesh(entity, Ogre::Vector3(2.0f, 2.0f, 2.0f));

    // Read back and verify each vertex was scaled
    std::vector<Ogre::Vector3> scaledPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), scaledPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(scaledPositions[i].x, originalPositions[i].x * 2.0f, 1e-4f)
            << "Vertex " << i << " x mismatch";
        EXPECT_NEAR(scaledPositions[i].y, originalPositions[i].y * 2.0f, 1e-4f)
            << "Vertex " << i << " y mismatch";
        EXPECT_NEAR(scaledPositions[i].z, originalPositions[i].z * 2.0f, 1e-4f)
            << "Vertex " << i << " z mismatch";
    }

    // Verify bounding box also scaled
    Ogre::AxisAlignedBox bounds = mesh->getBounds();
    EXPECT_NEAR(bounds.getMaximum().x, 2.0f, 0.1f);
    EXPECT_NEAR(bounds.getMaximum().y, 2.0f, 0.1f);
    EXPECT_NEAR(bounds.getMaximum().z, 2.0f, 0.1f);
    EXPECT_NEAR(bounds.getMinimum().x, -2.0f, 0.1f);
    EXPECT_NEAR(bounds.getMinimum().y, -2.0f, 0.1f);
    EXPECT_NEAR(bounds.getMinimum().z, -2.0f, 0.1f);

    Manager::getSingleton()->destroySceneNode("ScaleUniformCube");
}

TEST_F(MeshTransformTest, ScaleMeshNonUniform)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("ScaleNonUniformCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    // Record original vertex positions
    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Scale non-uniformly: 3x in X, 1x in Y, 0.5x in Z
    MeshTransform::scaleMesh(entity, Ogre::Vector3(3.0f, 1.0f, 0.5f));

    std::vector<Ogre::Vector3> scaledPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), scaledPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(scaledPositions[i].x, originalPositions[i].x * 3.0f, 1e-4f)
            << "Vertex " << i << " x mismatch";
        EXPECT_NEAR(scaledPositions[i].y, originalPositions[i].y * 1.0f, 1e-4f)
            << "Vertex " << i << " y mismatch";
        EXPECT_NEAR(scaledPositions[i].z, originalPositions[i].z * 0.5f, 1e-4f)
            << "Vertex " << i << " z mismatch";
    }

    Manager::getSingleton()->destroySceneNode("ScaleNonUniformCube");
}

TEST_F(MeshTransformTest, ScaleMeshByIdentityPreservesVertices)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("ScaleIdentityCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Scale by identity (1,1,1) should not change anything
    MeshTransform::scaleMesh(entity, Ogre::Vector3(1.0f, 1.0f, 1.0f));

    std::vector<Ogre::Vector3> afterPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), afterPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(afterPositions[i].x, originalPositions[i].x, 1e-5f);
        EXPECT_NEAR(afterPositions[i].y, originalPositions[i].y, 1e-5f);
        EXPECT_NEAR(afterPositions[i].z, originalPositions[i].z, 1e-5f);
    }

    Manager::getSingleton()->destroySceneNode("ScaleIdentityCube");
}

TEST_F(MeshTransformTest, ScaleMeshByZeroCollapsesVertices)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("ScaleZeroCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    // Scale by zero in all axes -- all vertices should collapse to origin
    MeshTransform::scaleMesh(entity, Ogre::Vector3(0.0f, 0.0f, 0.0f));

    std::vector<Ogre::Vector3> positions = getVertexPositions(mesh);
    ASSERT_FALSE(positions.empty());

    for (size_t i = 0; i < positions.size(); ++i)
    {
        EXPECT_NEAR(positions[i].x, 0.0f, 1e-5f) << "Vertex " << i << " x should be 0";
        EXPECT_NEAR(positions[i].y, 0.0f, 1e-5f) << "Vertex " << i << " y should be 0";
        EXPECT_NEAR(positions[i].z, 0.0f, 1e-5f) << "Vertex " << i << " z should be 0";
    }

    Manager::getSingleton()->destroySceneNode("ScaleZeroCube");
}

TEST_F(MeshTransformTest, ScaleMeshNegativeFlipsVertices)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("ScaleNegCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Scale by -1 should negate all coordinates
    MeshTransform::scaleMesh(entity, Ogre::Vector3(-1.0f, -1.0f, -1.0f));

    std::vector<Ogre::Vector3> negPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), negPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(negPositions[i].x, -originalPositions[i].x, 1e-4f);
        EXPECT_NEAR(negPositions[i].y, -originalPositions[i].y, 1e-4f);
        EXPECT_NEAR(negPositions[i].z, -originalPositions[i].z, 1e-4f);
    }

    Manager::getSingleton()->destroySceneNode("ScaleNegCube");
}

TEST_F(MeshTransformTest, ScaleMeshOverloadWithMeshPtr)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("ScaleMeshPtrCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Use the Mesh* overload directly (bypasses skeleton transform)
    MeshTransform::scaleMesh(mesh, Ogre::Vector3(2.0f, 2.0f, 2.0f));

    std::vector<Ogre::Vector3> scaledPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), scaledPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(scaledPositions[i].x, originalPositions[i].x * 2.0f, 1e-4f);
        EXPECT_NEAR(scaledPositions[i].y, originalPositions[i].y * 2.0f, 1e-4f);
        EXPECT_NEAR(scaledPositions[i].z, originalPositions[i].z * 2.0f, 1e-4f);
    }

    Manager::getSingleton()->destroySceneNode("ScaleMeshPtrCube");
}

TEST_F(MeshTransformTest, ScaleMeshConsecutivelyComposes)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("ScaleCompCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Scale by 2x then by 3x should be equivalent to 6x
    MeshTransform::scaleMesh(entity, Ogre::Vector3(2.0f, 2.0f, 2.0f));
    MeshTransform::scaleMesh(entity, Ogre::Vector3(3.0f, 3.0f, 3.0f));

    std::vector<Ogre::Vector3> finalPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), finalPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(finalPositions[i].x, originalPositions[i].x * 6.0f, 1e-3f)
            << "Vertex " << i << " x mismatch after 2x then 3x scale";
        EXPECT_NEAR(finalPositions[i].y, originalPositions[i].y * 6.0f, 1e-3f)
            << "Vertex " << i << " y mismatch";
        EXPECT_NEAR(finalPositions[i].z, originalPositions[i].z * 6.0f, 1e-3f)
            << "Vertex " << i << " z mismatch";
    }

    Manager::getSingleton()->destroySceneNode("ScaleCompCube");
}

// ------------------------------------------------------------------
// translateMesh tests
// ------------------------------------------------------------------

TEST_F(MeshTransformTest, TranslateMeshMovesVertices)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("TranslateCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    Ogre::Vector3 translation(5.0f, -3.0f, 7.0f);
    MeshTransform::translateMesh(entity, translation);

    std::vector<Ogre::Vector3> movedPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), movedPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(movedPositions[i].x, originalPositions[i].x + translation.x, 1e-4f)
            << "Vertex " << i << " x mismatch";
        EXPECT_NEAR(movedPositions[i].y, originalPositions[i].y + translation.y, 1e-4f)
            << "Vertex " << i << " y mismatch";
        EXPECT_NEAR(movedPositions[i].z, originalPositions[i].z + translation.z, 1e-4f)
            << "Vertex " << i << " z mismatch";
    }

    Manager::getSingleton()->destroySceneNode("TranslateCube");
}

TEST_F(MeshTransformTest, TranslateMeshByZeroPreservesVertices)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("TranslateZeroCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    MeshTransform::translateMesh(entity, Ogre::Vector3::ZERO);

    std::vector<Ogre::Vector3> afterPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), afterPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(afterPositions[i].x, originalPositions[i].x, 1e-5f);
        EXPECT_NEAR(afterPositions[i].y, originalPositions[i].y, 1e-5f);
        EXPECT_NEAR(afterPositions[i].z, originalPositions[i].z, 1e-5f);
    }

    Manager::getSingleton()->destroySceneNode("TranslateZeroCube");
}

TEST_F(MeshTransformTest, TranslateMeshUpdatesBounds)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("TranslateBoundsCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    Ogre::AxisAlignedBox originalBounds = mesh->getBounds();
    Ogre::Vector3 originalCenter = originalBounds.getCenter();

    Ogre::Vector3 translation(10.0f, 20.0f, 30.0f);
    MeshTransform::translateMesh(entity, translation);

    Ogre::AxisAlignedBox newBounds = mesh->getBounds();
    Ogre::Vector3 newCenter = newBounds.getCenter();

    // The center should have moved by the translation amount
    EXPECT_NEAR(newCenter.x, originalCenter.x + translation.x, 0.2f);
    EXPECT_NEAR(newCenter.y, originalCenter.y + translation.y, 0.2f);
    EXPECT_NEAR(newCenter.z, originalCenter.z + translation.z, 0.2f);

    // The bounding box size should remain the same
    Ogre::Vector3 originalSize = originalBounds.getSize();
    Ogre::Vector3 newSize = newBounds.getSize();
    EXPECT_NEAR(newSize.x, originalSize.x, 0.1f);
    EXPECT_NEAR(newSize.y, originalSize.y, 0.1f);
    EXPECT_NEAR(newSize.z, originalSize.z, 0.1f);

    Manager::getSingleton()->destroySceneNode("TranslateBoundsCube");
}

TEST_F(MeshTransformTest, TranslateMeshConsecutivelyAccumulates)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("TranslateAccumCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Translate twice: (1,2,3) + (4,5,6) = (5,7,9)
    MeshTransform::translateMesh(entity, Ogre::Vector3(1.0f, 2.0f, 3.0f));
    MeshTransform::translateMesh(entity, Ogre::Vector3(4.0f, 5.0f, 6.0f));

    std::vector<Ogre::Vector3> finalPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), finalPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(finalPositions[i].x, originalPositions[i].x + 5.0f, 1e-4f);
        EXPECT_NEAR(finalPositions[i].y, originalPositions[i].y + 7.0f, 1e-4f);
        EXPECT_NEAR(finalPositions[i].z, originalPositions[i].z + 9.0f, 1e-4f);
    }

    Manager::getSingleton()->destroySceneNode("TranslateAccumCube");
}

TEST_F(MeshTransformTest, TranslateMeshNegativeDirection)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("TranslateNegCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    Ogre::Vector3 translation(-10.0f, -20.0f, -30.0f);
    MeshTransform::translateMesh(entity, translation);

    std::vector<Ogre::Vector3> movedPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), movedPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(movedPositions[i].x, originalPositions[i].x + translation.x, 1e-4f);
        EXPECT_NEAR(movedPositions[i].y, originalPositions[i].y + translation.y, 1e-4f);
        EXPECT_NEAR(movedPositions[i].z, originalPositions[i].z + translation.z, 1e-4f);
    }

    Manager::getSingleton()->destroySceneNode("TranslateNegCube");
}

// ------------------------------------------------------------------
// rotateMesh tests
// ------------------------------------------------------------------

TEST_F(MeshTransformTest, RotateMeshAroundX)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("RotateXCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Rotate 90 degrees around X axis (the code uses _rotate.x with UNIT_Y)
    // In the implementation: _rotate.x != 0 => Quaternion(Degree(_rotate.x), UNIT_Y)
    MeshTransform::rotateMesh(entity, Ogre::Vector3(90.0f, 0.0f, 0.0f));

    std::vector<Ogre::Vector3> rotatedPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), rotatedPositions.size());

    // After rotation, vertices should have changed but vertex count should remain
    // A 90-degree rotation around Y axis should swap X and Z components (with sign change)
    Ogre::Vector3 center = mesh->getBounds().getCenter();

    // The bounding box should still contain roughly the same extents (cube is symmetric)
    Ogre::AxisAlignedBox bounds = mesh->getBounds();
    Ogre::Vector3 size = bounds.getSize();
    // For a cube, rotation should not significantly change the bounding box size
    // (cube is symmetric, so rotating 90 degrees gives same box)
    EXPECT_GT(size.x, 0.0f);
    EXPECT_GT(size.y, 0.0f);
    EXPECT_GT(size.z, 0.0f);

    Manager::getSingleton()->destroySceneNode("RotateXCube");
}

TEST_F(MeshTransformTest, RotateMeshAroundY)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("RotateYCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Rotate 90 degrees around Y axis (the code uses _rotate.y with UNIT_Z)
    MeshTransform::rotateMesh(entity, Ogre::Vector3(0.0f, 90.0f, 0.0f));

    std::vector<Ogre::Vector3> rotatedPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), rotatedPositions.size());

    // Verify rotation happened - at least some vertices should differ from original
    bool anyDifferent = false;
    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        if (std::abs(rotatedPositions[i].x - originalPositions[i].x) > 1e-4f ||
            std::abs(rotatedPositions[i].y - originalPositions[i].y) > 1e-4f ||
            std::abs(rotatedPositions[i].z - originalPositions[i].z) > 1e-4f)
        {
            anyDifferent = true;
            break;
        }
    }
    EXPECT_TRUE(anyDifferent) << "Rotation should have changed at least some vertex positions";

    Manager::getSingleton()->destroySceneNode("RotateYCube");
}

TEST_F(MeshTransformTest, RotateMeshAroundZ)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("RotateZCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Rotate 90 degrees around Z axis (the code uses _rotate.z with UNIT_X)
    MeshTransform::rotateMesh(entity, Ogre::Vector3(0.0f, 0.0f, 90.0f));

    std::vector<Ogre::Vector3> rotatedPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), rotatedPositions.size());

    // Verify rotation happened
    bool anyDifferent = false;
    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        if (std::abs(rotatedPositions[i].x - originalPositions[i].x) > 1e-4f ||
            std::abs(rotatedPositions[i].y - originalPositions[i].y) > 1e-4f ||
            std::abs(rotatedPositions[i].z - originalPositions[i].z) > 1e-4f)
        {
            anyDifferent = true;
            break;
        }
    }
    EXPECT_TRUE(anyDifferent) << "Rotation should have changed at least some vertex positions";

    Manager::getSingleton()->destroySceneNode("RotateZCube");
}

TEST_F(MeshTransformTest, RotateMeshByZeroPreservesVertices)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("RotateZeroCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Rotate by zero in all axes -- the else branch: rpos = pos - Center + Center = pos
    MeshTransform::rotateMesh(entity, Ogre::Vector3(0.0f, 0.0f, 0.0f));

    std::vector<Ogre::Vector3> afterPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), afterPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(afterPositions[i].x, originalPositions[i].x, 1e-4f);
        EXPECT_NEAR(afterPositions[i].y, originalPositions[i].y, 1e-4f);
        EXPECT_NEAR(afterPositions[i].z, originalPositions[i].z, 1e-4f);
    }

    Manager::getSingleton()->destroySceneNode("RotateZeroCube");
}

TEST_F(MeshTransformTest, RotateMesh360DegreesReturnsToOriginal)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("Rotate360Cube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Rotate 360 degrees around the X-mapped axis (UNIT_Y) in four 90-degree steps
    for (int step = 0; step < 4; ++step)
    {
        MeshTransform::rotateMesh(entity, Ogre::Vector3(90.0f, 0.0f, 0.0f));
    }

    std::vector<Ogre::Vector3> afterPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), afterPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(afterPositions[i].x, originalPositions[i].x, 0.01f)
            << "Vertex " << i << " x should return to original after 360 rotation";
        EXPECT_NEAR(afterPositions[i].y, originalPositions[i].y, 0.01f)
            << "Vertex " << i << " y should return to original after 360 rotation";
        EXPECT_NEAR(afterPositions[i].z, originalPositions[i].z, 0.01f)
            << "Vertex " << i << " z should return to original after 360 rotation";
    }

    Manager::getSingleton()->destroySceneNode("Rotate360Cube");
}

TEST_F(MeshTransformTest, RotateMeshUpdatesBounds)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("RotateBoundsCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    // A cube of size 2 centered at origin: bounds should be (-1,-1,-1) to (1,1,1)
    Ogre::AxisAlignedBox originalBounds = mesh->getBounds();

    // Rotate 45 degrees around UNIT_Y (via _rotate.x)
    MeshTransform::rotateMesh(entity, Ogre::Vector3(45.0f, 0.0f, 0.0f));

    Ogre::AxisAlignedBox rotatedBounds = mesh->getBounds();

    // After 45-degree rotation of a cube, the bounding box should be valid (not infinite/null)
    EXPECT_FALSE(rotatedBounds.isNull());
    EXPECT_FALSE(rotatedBounds.isInfinite());

    // The bounding box should have non-zero size
    Ogre::Vector3 size = rotatedBounds.getSize();
    EXPECT_GT(size.x, 0.0f);
    EXPECT_GT(size.y, 0.0f);
    EXPECT_GT(size.z, 0.0f);

    Manager::getSingleton()->destroySceneNode("RotateBoundsCube");
}

// ------------------------------------------------------------------
// Combined transform tests
// ------------------------------------------------------------------

TEST_F(MeshTransformTest, TranslateThenScaleMesh)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("TransScaleCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Translate by (5,5,5) then scale by 2x
    MeshTransform::translateMesh(entity, Ogre::Vector3(5.0f, 5.0f, 5.0f));
    MeshTransform::scaleMesh(entity, Ogre::Vector3(2.0f, 2.0f, 2.0f));

    std::vector<Ogre::Vector3> finalPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), finalPositions.size());

    // Expected: first translate then scale -> (original + 5) * 2
    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(finalPositions[i].x, (originalPositions[i].x + 5.0f) * 2.0f, 1e-3f);
        EXPECT_NEAR(finalPositions[i].y, (originalPositions[i].y + 5.0f) * 2.0f, 1e-3f);
        EXPECT_NEAR(finalPositions[i].z, (originalPositions[i].z + 5.0f) * 2.0f, 1e-3f);
    }

    Manager::getSingleton()->destroySceneNode("TransScaleCube");
}

TEST_F(MeshTransformTest, TransformWorksWithSphere)
{
    Ogre::SceneNode* node = PrimitiveObject::createSphere("TransformSphere");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Scale sphere by 3x
    MeshTransform::scaleMesh(entity, Ogre::Vector3(3.0f, 3.0f, 3.0f));

    std::vector<Ogre::Vector3> scaledPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), scaledPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(scaledPositions[i].x, originalPositions[i].x * 3.0f, 1e-3f);
        EXPECT_NEAR(scaledPositions[i].y, originalPositions[i].y * 3.0f, 1e-3f);
        EXPECT_NEAR(scaledPositions[i].z, originalPositions[i].z * 3.0f, 1e-3f);
    }

    Manager::getSingleton()->destroySceneNode("TransformSphere");
}

TEST_F(MeshTransformTest, TransformWorksWithPlane)
{
    Ogre::SceneNode* node = PrimitiveObject::createPlane("TransformPlane");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    std::vector<Ogre::Vector3> originalPositions = getVertexPositions(mesh);
    ASSERT_FALSE(originalPositions.empty());

    // Translate plane
    Ogre::Vector3 translation(1.0f, 2.0f, 3.0f);
    MeshTransform::translateMesh(entity, translation);

    std::vector<Ogre::Vector3> movedPositions = getVertexPositions(mesh);
    ASSERT_EQ(originalPositions.size(), movedPositions.size());

    for (size_t i = 0; i < originalPositions.size(); ++i)
    {
        EXPECT_NEAR(movedPositions[i].x, originalPositions[i].x + translation.x, 1e-4f);
        EXPECT_NEAR(movedPositions[i].y, originalPositions[i].y + translation.y, 1e-4f);
        EXPECT_NEAR(movedPositions[i].z, originalPositions[i].z + translation.z, 1e-4f);
    }

    Manager::getSingleton()->destroySceneNode("TransformPlane");
}

// ------------------------------------------------------------------
// Vertex count preservation tests
// ------------------------------------------------------------------

TEST_F(MeshTransformTest, ScalePreservesVertexCount)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("CountScaleCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    size_t originalCount = getVertexPositions(mesh).size();
    ASSERT_GT(originalCount, 0u);

    MeshTransform::scaleMesh(entity, Ogre::Vector3(5.0f, 5.0f, 5.0f));

    size_t afterCount = getVertexPositions(mesh).size();
    EXPECT_EQ(originalCount, afterCount);

    Manager::getSingleton()->destroySceneNode("CountScaleCube");
}

TEST_F(MeshTransformTest, TranslatePreservesVertexCount)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("CountTransCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    size_t originalCount = getVertexPositions(mesh).size();
    ASSERT_GT(originalCount, 0u);

    MeshTransform::translateMesh(entity, Ogre::Vector3(100.0f, 200.0f, 300.0f));

    size_t afterCount = getVertexPositions(mesh).size();
    EXPECT_EQ(originalCount, afterCount);

    Manager::getSingleton()->destroySceneNode("CountTransCube");
}

TEST_F(MeshTransformTest, RotatePreservesVertexCount)
{
    Ogre::SceneNode* node = PrimitiveObject::createCube("CountRotCube");
    ASSERT_NE(node, nullptr);
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    Ogre::Mesh* mesh = entity->getMesh().get();

    size_t originalCount = getVertexPositions(mesh).size();
    ASSERT_GT(originalCount, 0u);

    MeshTransform::rotateMesh(entity, Ogre::Vector3(45.0f, 0.0f, 0.0f));

    size_t afterCount = getVertexPositions(mesh).size();
    EXPECT_EQ(originalCount, afterCount);

    Manager::getSingleton()->destroySceneNode("CountRotCube");
}
