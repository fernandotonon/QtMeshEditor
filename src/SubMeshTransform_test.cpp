#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>
#include <OgreHardwareBufferManager.h>
#include <OgreSceneManager.h>

#include "Manager.h"
#include "SubMeshTransform.h"
#include "TestHelpers.h"

#include <cmath>

class SubMeshTransformTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre());
        createStandardOgreMaterials();
        ASSERT_TRUE(canLoadMeshFiles());
    }
    void TearDown() override {
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(20);
    }
    QApplication* app = nullptr;

    // Build a triangle entity. The TestHelpers triangle has vertices
    // {(0,0,0), (1,0,0), (0,1,0)} so the centroid is (1/3, 1/3, 0).
    Ogre::Entity* makeTriangle(const std::string& name) {
        auto mesh = createInMemoryTriangleMesh(name + "_mesh");
        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* entity = sceneMgr->createEntity(name, mesh);
        return entity;
    }
};

// -------------------- getSubMeshCenter --------------------

TEST_F(SubMeshTransformTest, GetSubMeshCenterReturnsCentroid)
{
    auto* entity = makeTriangle("centroid_e");
    const auto center = SubMeshTransform::getSubMeshCenter(entity, 0);
    EXPECT_NEAR(center.x, 1.0f / 3.0f, 1e-4f);
    EXPECT_NEAR(center.y, 1.0f / 3.0f, 1e-4f);
    EXPECT_NEAR(center.z, 0.0f, 1e-4f);
}

TEST_F(SubMeshTransformTest, GetSubMeshCenterNullEntityReturnsZero)
{
    const auto center = SubMeshTransform::getSubMeshCenter(nullptr, 0);
    EXPECT_EQ(center, Ogre::Vector3::ZERO);
}

TEST_F(SubMeshTransformTest, GetSubMeshCenterOutOfRangeIndexReturnsZero)
{
    auto* entity = makeTriangle("oor_e");
    const auto center = SubMeshTransform::getSubMeshCenter(entity, 42);
    EXPECT_EQ(center, Ogre::Vector3::ZERO);
}

// -------------------- readPositions --------------------

TEST_F(SubMeshTransformTest, ReadPositionsReturnsAllVertices)
{
    auto* entity = makeTriangle("read_e");
    const auto pos = SubMeshTransform::readPositions(entity, 0);
    ASSERT_EQ(pos.size(), 3u);
    EXPECT_EQ(pos[0], Ogre::Vector3(0, 0, 0));
    EXPECT_EQ(pos[1], Ogre::Vector3(1, 0, 0));
    EXPECT_EQ(pos[2], Ogre::Vector3(0, 1, 0));
}

TEST_F(SubMeshTransformTest, ReadPositionsNullEntityReturnsEmpty)
{
    const auto pos = SubMeshTransform::readPositions(nullptr, 0);
    EXPECT_TRUE(pos.empty());
}

TEST_F(SubMeshTransformTest, ReadPositionsOutOfRangeIndexReturnsEmpty)
{
    auto* entity = makeTriangle("read_oor");
    const auto pos = SubMeshTransform::readPositions(entity, 99);
    EXPECT_TRUE(pos.empty());
}

// -------------------- writePositions --------------------

TEST_F(SubMeshTransformTest, WritePositionsUpdatesBuffer)
{
    auto* entity = makeTriangle("write_e");
    std::vector<Ogre::Vector3> newPos{
        Ogre::Vector3(10, 20, 30),
        Ogre::Vector3(40, 50, 60),
        Ogre::Vector3(70, 80, 90),
    };
    SubMeshTransform::writePositions(entity, 0, newPos);
    const auto pos = SubMeshTransform::readPositions(entity, 0);
    ASSERT_EQ(pos.size(), 3u);
    EXPECT_EQ(pos[0], Ogre::Vector3(10, 20, 30));
    EXPECT_EQ(pos[1], Ogre::Vector3(40, 50, 60));
    EXPECT_EQ(pos[2], Ogre::Vector3(70, 80, 90));
}

TEST_F(SubMeshTransformTest, WritePositionsHandlesShortInput)
{
    // Fewer positions than vertex count — only first N updated, the rest untouched.
    auto* entity = makeTriangle("write_short_e");
    std::vector<Ogre::Vector3> shortIn{
        Ogre::Vector3(99, 99, 99),
    };
    SubMeshTransform::writePositions(entity, 0, shortIn);
    const auto pos = SubMeshTransform::readPositions(entity, 0);
    ASSERT_EQ(pos.size(), 3u);
    EXPECT_EQ(pos[0], Ogre::Vector3(99, 99, 99));
    EXPECT_EQ(pos[1], Ogre::Vector3(1, 0, 0));
    EXPECT_EQ(pos[2], Ogre::Vector3(0, 1, 0));
}

TEST_F(SubMeshTransformTest, WritePositionsRecalculatesBounds)
{
    auto* entity = makeTriangle("write_bounds_e");
    std::vector<Ogre::Vector3> bigPos{
        Ogre::Vector3(-5, -5, -5),
        Ogre::Vector3( 5,  5,  5),
        Ogre::Vector3( 0,  0,  0),
    };
    SubMeshTransform::writePositions(entity, 0, bigPos);
    const Ogre::AxisAlignedBox bb = entity->getMesh()->getBounds();
    EXPECT_NEAR(bb.getMinimum().x, -5.0f, 1e-4f);
    EXPECT_NEAR(bb.getMaximum().x,  5.0f, 1e-4f);
}

TEST_F(SubMeshTransformTest, WritePositionsNullEntityDoesNothing)
{
    // Just ensure no crash.
    SubMeshTransform::writePositions(nullptr, 0, {Ogre::Vector3::ZERO});
}

// -------------------- translateSubMesh --------------------

TEST_F(SubMeshTransformTest, TranslateMovesAllVertices)
{
    auto* entity = makeTriangle("trans_e");
    SubMeshTransform::translateSubMesh(entity, 0, Ogre::Vector3(5, 0, -2));
    const auto pos = SubMeshTransform::readPositions(entity, 0);
    ASSERT_EQ(pos.size(), 3u);
    EXPECT_EQ(pos[0], Ogre::Vector3(5, 0, -2));
    EXPECT_EQ(pos[1], Ogre::Vector3(6, 0, -2));
    EXPECT_EQ(pos[2], Ogre::Vector3(5, 1, -2));
}

TEST_F(SubMeshTransformTest, TranslateNullEntityIsNoop)
{
    SubMeshTransform::translateSubMesh(nullptr, 0, Ogre::Vector3(1, 1, 1));
    // No crash = success.
}

TEST_F(SubMeshTransformTest, TranslateOutOfRangeIsNoop)
{
    auto* entity = makeTriangle("trans_oor_e");
    SubMeshTransform::translateSubMesh(entity, 99, Ogre::Vector3(5, 5, 5));
    const auto pos = SubMeshTransform::readPositions(entity, 0);
    EXPECT_EQ(pos[1], Ogre::Vector3(1, 0, 0));
}

// -------------------- scaleSubMesh --------------------

TEST_F(SubMeshTransformTest, ScaleAroundCentroid)
{
    auto* entity = makeTriangle("scale_e");
    const Ogre::Vector3 centerBefore = SubMeshTransform::getSubMeshCenter(entity, 0);
    SubMeshTransform::scaleSubMesh(entity, 0, Ogre::Vector3(2, 2, 2));
    const Ogre::Vector3 centerAfter = SubMeshTransform::getSubMeshCenter(entity, 0);
    EXPECT_NEAR(centerAfter.x, centerBefore.x, 1e-4f);
    EXPECT_NEAR(centerAfter.y, centerBefore.y, 1e-4f);
    EXPECT_NEAR(centerAfter.z, centerBefore.z, 1e-4f);

    // Each vertex's distance to the centroid should have doubled.
    const auto pos = SubMeshTransform::readPositions(entity, 0);
    const auto orig = std::vector<Ogre::Vector3>{
        Ogre::Vector3(0, 0, 0), Ogre::Vector3(1, 0, 0), Ogre::Vector3(0, 1, 0)};
    for (size_t i = 0; i < 3; ++i) {
        const auto expected = centerBefore + (orig[i] - centerBefore) * Ogre::Vector3(2, 2, 2);
        EXPECT_NEAR(pos[i].x, expected.x, 1e-4f);
        EXPECT_NEAR(pos[i].y, expected.y, 1e-4f);
        EXPECT_NEAR(pos[i].z, expected.z, 1e-4f);
    }
}

TEST_F(SubMeshTransformTest, ScaleNullEntityIsNoop)
{
    SubMeshTransform::scaleSubMesh(nullptr, 0, Ogre::Vector3(2, 2, 2));
}

// -------------------- rotateSubMesh --------------------

TEST_F(SubMeshTransformTest, Rotate180DegreesAroundY)
{
    auto* entity = makeTriangle("rot_e");
    const Ogre::Vector3 centerBefore = SubMeshTransform::getSubMeshCenter(entity, 0);
    Ogre::Quaternion q(Ogre::Radian(Ogre::Math::PI), Ogre::Vector3::UNIT_Y);
    SubMeshTransform::rotateSubMesh(entity, 0, q);

    const Ogre::Vector3 centerAfter = SubMeshTransform::getSubMeshCenter(entity, 0);
    // Center is preserved under rotation around centroid.
    EXPECT_NEAR(centerAfter.x, centerBefore.x, 1e-4f);
    EXPECT_NEAR(centerAfter.y, centerBefore.y, 1e-4f);
    EXPECT_NEAR(centerAfter.z, centerBefore.z, 1e-4f);

    // The original (1,0,0)→(0,0,0) edge in X should now point in -X.
    const auto pos = SubMeshTransform::readPositions(entity, 0);
    // (1,0,0) reflected through centroid (1/3,1/3,0) along XZ → x' = 2*(1/3) - 1 = -1/3
    EXPECT_NEAR(pos[1].x, -1.0f / 3.0f, 1e-4f);
}

TEST_F(SubMeshTransformTest, RotationRotatesNormals)
{
    auto* entity = makeTriangle("rot_norm_e");
    // 90° about X: normal (0,0,1) should become (0,-1,0) (Ogre right-handed convention check
    // is what the function uses; we just check the magnitude is preserved and the direction
    // has changed from purely +Z).
    Ogre::Quaternion q(Ogre::Radian(Ogre::Math::HALF_PI), Ogre::Vector3::UNIT_X);
    SubMeshTransform::rotateSubMesh(entity, 0, q);

    // Read normals back.
    auto* mesh = entity->getMesh().get();
    Ogre::VertexData* vdata = mesh->sharedVertexData;
    const auto* normElem = vdata->vertexDeclaration->findElementBySemantic(Ogre::VES_NORMAL);
    ASSERT_NE(normElem, nullptr);
    auto nbuf = vdata->vertexBufferBinding->getBuffer(normElem->getSource());
    auto* vp = static_cast<unsigned char*>(nbuf->lock(Ogre::HardwareBuffer::HBL_READ_ONLY));
    Ogre::Real* n;
    normElem->baseVertexPointerToElement(vp, &n);
    Ogre::Vector3 norm(n[0], n[1], n[2]);
    nbuf->unlock();
    // Magnitude preserved
    EXPECT_NEAR(norm.length(), 1.0f, 1e-3f);
    // Not equal to original (0,0,1)
    EXPECT_GT((norm - Ogre::Vector3(0, 0, 1)).length(), 0.1f);
}

TEST_F(SubMeshTransformTest, RotateNullEntityIsNoop)
{
    SubMeshTransform::rotateSubMesh(nullptr, 0, Ogre::Quaternion::IDENTITY);
}

// -------------------- recalculateMeshBounds --------------------

TEST_F(SubMeshTransformTest, RecalculateMeshBoundsHandlesMultiSubmeshAndShared)
{
    auto mesh = createInMemoryMeshSharedVertsPlusLocalSubmesh("rcb_mesh");
    // Shared verts span x in [0..1], local submesh x in [10..11].
    SubMeshTransform::recalculateMeshBounds(mesh.get());
    const Ogre::AxisAlignedBox bb = mesh->getBounds();
    EXPECT_NEAR(bb.getMinimum().x, 0.0f, 1e-4f);
    EXPECT_NEAR(bb.getMaximum().x, 11.0f, 1e-4f);
}
