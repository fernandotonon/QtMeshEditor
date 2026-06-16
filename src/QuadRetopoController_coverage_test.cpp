#include "QuadRetopoController.h"
#include "QuadRetopo.h"
#include "SelectionSet.h"
#include "Manager.h"
#include "TestHelpers.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QApplication>
#include <QSignalSpy>
#include <QVariantMap>

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMeshManager.h>
#include <OgreSubMesh.h>
#include <OgreHardwareBufferManager.h>

// Coverage tests for QuadRetopoController (issue #401) — the QML-facing
// singleton wrapper around QuadRetopo::retopologize. The pure-data
// algorithm is covered by QuadRetopo_test.cpp; here we drive the
// controller's singleton lifecycle, selection-state property, error
// paths, and the happy retopology path on a real selected entity.
//
// Distinct suite name (QuadRetopoControllerCoverageTest) and file name to
// avoid ODR / duplicate-registration clashes with QuadRetopo_test.cpp.

namespace {

// Builds an in-memory mesh of two coplanar right triangles sharing a
// diagonal (a unit square split into two tris). The triangle-pairing
// retopology pairs these into a single quad with default options, so
// the controller's happy path produces applied=true with one quad.
//
// The mesh uses a dedicated submesh vertex buffer (not shared) so the
// in-place EditableSubMesh rewrite has straightforward geometry.
Ogre::MeshPtr makeCoplanarQuadMesh(const std::string& name)
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
        decl->getVertexSize(0), 4, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    // pos(3) normal(3) uv(2) per vertex; all in the z=0 plane => coplanar
    float verts[] = {
        0,0,0,  0,0,1,  0,0,   // v0
        1,0,0,  0,0,1,  1,0,   // v1
        1,1,0,  0,0,1,  1,1,   // v2
        0,1,0,  0,0,1,  0,1,   // v3
    };
    vbuf->writeData(0, sizeof(verts), verts);
    sub->vertexData->vertexBufferBinding->setBinding(0, vbuf);
    sub->vertexData->vertexCount = 4;

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 6,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = { 0,1,2,  0,2,3 };  // two tris sharing diagonal 0-2
    ibuf->writeData(0, sizeof(idx), idx);
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 6;

    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,1,1,1));
    mesh->_setBoundingSphereRadius(2.0);
    mesh->load();
    return mesh;
}

class QuadRetopoControllerCoverageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        createStandardOgreMaterials();
        // Fresh controller per test so selection signal wiring + busy
        // state start clean.
        QuadRetopoController::kill();
    }

    void TearDown() override
    {
        QuadRetopoController::kill();
        if (auto* sel = SelectionSet::getSingletonPtr())
            sel->clearList();
    }

    // Build a coplanar-quad entity attached to a scene node and select it.
    Ogre::Entity* selectCoplanarEntity(const std::string& name)
    {
        auto mesh = makeCoplanarQuadMesh(name + "_mesh");
        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* node = Manager::getSingleton()->addSceneNode(name.c_str());
        auto* entity = sceneMgr->createEntity(name, mesh);
        node->attachObject(entity);
        SelectionSet::getSingleton()->selectOne(entity);
        return entity;
    }
};

// --- singleton lifecycle (lines 11-30) -----------------------------------

TEST_F(QuadRetopoControllerCoverageTest, InstanceReturnsStableSingleton)
{
    auto* a = QuadRetopoController::instance();
    auto* b = QuadRetopoController::instance();
    EXPECT_NE(a, nullptr);
    EXPECT_EQ(a, b);
}

TEST_F(QuadRetopoControllerCoverageTest, KillResetsSingletonSoNextInstanceIsFresh)
{
    auto* first = QuadRetopoController::instance();
    ASSERT_NE(first, nullptr);
    QuadRetopoController::kill();
    auto* second = QuadRetopoController::instance();
    EXPECT_NE(second, nullptr);
    // A fresh allocation after kill — observably not busy.
    EXPECT_FALSE(second->busy());
}

TEST_F(QuadRetopoControllerCoverageTest, KillIsIdempotent)
{
    QuadRetopoController::instance();
    QuadRetopoController::kill();
    EXPECT_NO_THROW(QuadRetopoController::kill());  // double-kill safe
    EXPECT_NE(QuadRetopoController::instance(), nullptr);
}

// --- qmlInstance ownership wrapper (lines 18-24) -------------------------

TEST_F(QuadRetopoControllerCoverageTest, QmlInstanceReturnsSameSingletonWithCppOwnership)
{
    auto* viaInstance = QuadRetopoController::instance();
    auto* viaQml = QuadRetopoController::qmlInstance(nullptr, nullptr);
    EXPECT_EQ(viaInstance, viaQml);
    // Ownership flag set to CppOwnership — the object must survive (not be
    // GC'd). We can't read the flag directly, but the pointer staying valid
    // and equal to instance() is the observable contract.
    EXPECT_EQ(viaQml, QuadRetopoController::instance());
}

// --- hasSelection (lines 38-43) ------------------------------------------

TEST_F(QuadRetopoControllerCoverageTest, HasSelectionFalseWhenNothingSelected)
{
    if (auto* sel = SelectionSet::getSingletonPtr())
        sel->clearList();
    auto* c = QuadRetopoController::instance();
    EXPECT_FALSE(c->hasSelection());
}

TEST_F(QuadRetopoControllerCoverageTest, HasSelectionTrueWhenEntitySelected)
{
    auto* c = QuadRetopoController::instance();
    selectCoplanarEntity("qrcc_hassel");
    EXPECT_TRUE(c->hasSelection());
}

TEST_F(QuadRetopoControllerCoverageTest, SelectionChangedSignalForwardedFromSelectionSet)
{
    auto* c = QuadRetopoController::instance();
    QSignalSpy spy(c, &QuadRetopoController::selectionChanged);
    ASSERT_TRUE(spy.isValid());
    selectCoplanarEntity("qrcc_signal");
    // selectOne emits SelectionSet::selectionChanged, which the controller
    // re-emits as its own selectionChanged.
    EXPECT_GE(spy.count(), 1);
    EXPECT_TRUE(c->hasSelection());
}

// --- retopologizeSelected: empty-selection error path (lines 60-67) ------

TEST_F(QuadRetopoControllerCoverageTest, RetopologizeSelectedEmptySelectionEmitsError)
{
    if (auto* sel = SelectionSet::getSingletonPtr())
        sel->clearList();
    auto* c = QuadRetopoController::instance();

    QSignalSpy errorSpy(c, &QuadRetopoController::error);
    QSignalSpy busySpy(c, &QuadRetopoController::busyChanged);
    ASSERT_TRUE(errorSpy.isValid());

    QVariantMap result = c->retopologizeSelected(-1, 30.0, 90.0, 8.0);

    EXPECT_FALSE(result["applied"].toBool());
    EXPECT_EQ(result["error"].toString(), QStringLiteral("No mesh selected."));
    EXPECT_GE(errorSpy.count(), 1);
    EXPECT_EQ(errorSpy.takeFirst().at(0).toString(),
              QStringLiteral("No mesh selected."));
    // Error path returns before flipping busy.
    EXPECT_EQ(busySpy.count(), 0);
    EXPECT_FALSE(c->busy());
}

// --- retopologizeSelected: happy path (lines 69-114) ---------------------

TEST_F(QuadRetopoControllerCoverageTest, RetopologizeSelectedHappyPathPairsTrianglesIntoQuad)
{
    auto* c = QuadRetopoController::instance();
    selectCoplanarEntity("qrcc_happy");

    QSignalSpy busySpy(c, &QuadRetopoController::busyChanged);
    QSignalSpy appliedSpy(c, &QuadRetopoController::retopoApplied);
    QSignalSpy errorSpy(c, &QuadRetopoController::error);
    ASSERT_TRUE(busySpy.isValid());
    ASSERT_TRUE(appliedSpy.isValid());

    QVariantMap result = c->retopologizeSelected(-1, 30.0, 90.0, 8.0);

    EXPECT_TRUE(result["applied"].toBool());
    EXPECT_GT(result["totalTrianglesBefore"].toInt(), 0);
    EXPECT_EQ(result["totalTrianglesBefore"].toInt(), 2);
    EXPECT_EQ(result["totalQuadsAfter"].toInt(), 1);
    EXPECT_EQ(result["totalFacesAfter"].toInt(), 1);
    EXPECT_EQ(result["totalTrianglesAfter"].toInt(), 0);
    EXPECT_TRUE(result.contains("meshName"));
    EXPECT_NEAR(result["quadDominance"].toDouble(), 1.0, 1e-6);

    // busy true then false => at least two busyChanged emissions.
    EXPECT_GE(busySpy.count(), 2);
    // Ends not busy.
    EXPECT_FALSE(c->busy());

    // Applied => retopoApplied fires, no error.
    EXPECT_GE(appliedSpy.count(), 1);
    EXPECT_EQ(errorSpy.count(), 0);

    // The report carried by retopoApplied mirrors the returned map.
    ASSERT_GE(appliedSpy.count(), 1);
    const QVariantMap emitted = appliedSpy.takeFirst().at(0).toMap();
    EXPECT_TRUE(emitted["applied"].toBool());
    EXPECT_EQ(emitted["totalQuadsAfter"].toInt(), 1);
}

TEST_F(QuadRetopoControllerCoverageTest, RetopologizeSelectedNoPairsKeepsTrianglesStillApplied)
{
    // Non-coplanar triangles: bend v3 up out of plane so the dihedral
    // exceeds the default 25° gate when we pass a strict maxAngle. No quad
    // is formed but the operation still "applies" (mesh preserved as tris).
    auto mesh = makeCoplanarQuadMesh("qrcc_nopair_mesh");
    // Rewrite v3 z to push it out of plane via the existing buffer.
    {
        auto* sub = mesh->getSubMesh(0);
        auto vbuf = sub->vertexData->vertexBufferBinding->getBuffer(0);
        float* p = static_cast<float*>(vbuf->lock(Ogre::HardwareBuffer::HBL_NORMAL));
        // v3 is the 4th vertex; stride = 8 floats; pos.z is index 2.
        p[3 * 8 + 2] = 1.0f;
        vbuf->unlock();
    }
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("qrcc_nopair");
    auto* entity = sceneMgr->createEntity("qrcc_nopair", mesh);
    node->attachObject(entity);
    SelectionSet::getSingleton()->selectOne(entity);

    auto* c = QuadRetopoController::instance();
    QSignalSpy busySpy(c, &QuadRetopoController::busyChanged);
    QSignalSpy appliedSpy(c, &QuadRetopoController::retopoApplied);

    // Strict coplanarity gate (10°) rejects the ~45° bend => no pairs.
    QVariantMap result = c->retopologizeSelected(-1, 10.0, 65.0, 6.0);

    EXPECT_TRUE(result["applied"].toBool());
    EXPECT_EQ(result["totalTrianglesBefore"].toInt(), 2);
    EXPECT_EQ(result["totalQuadsAfter"].toInt(), 0);
    EXPECT_EQ(result["totalTrianglesAfter"].toInt(), 2);
    EXPECT_NEAR(result["quadDominance"].toDouble(), 0.0, 1e-6);
    EXPECT_GE(busySpy.count(), 2);
    EXPECT_GE(appliedSpy.count(), 1);  // applied (preserved) still emits
}

} // namespace
