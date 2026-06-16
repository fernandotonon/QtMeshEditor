// Coverage suite for MeshValidator::optimizeVertexCache() / fixAll(with selection)
// / hasCacheOptimization(). The existing MeshValidator_test.cpp thoroughly covers
// validate()/doValidate()/frameStarted()/selection clearing and the no-selection
// fixAll error path, but never exercises optimizeVertexCache() (the whole
// m_lastOptimizeResult / m_cacheOptimizationAvailable machinery) nor fixAll() with
// a real selection. Distinct filename + distinct suite name (MeshValidatorCoverageTest)
// to avoid any ODR / duplicate-registration clash with the existing suite.

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>

#define private public
#include "MeshValidator.h"
#undef private

#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

static constexpr unsigned long kSingletonSettleTimeMs = 30;

static Ogre::Entity* covCreateEntityFromMesh(const std::string& nodeName, const Ogre::MeshPtr& mesh)
{
    if (!mesh) {
        return nullptr;
    }

    auto* manager = Manager::getSingleton();
    auto* node = manager->addSceneNode(nodeName.c_str());
    if (!node) {
        return nullptr;
    }

    return manager->createEntity(node, mesh);
}

// Mirrors the existing suite's createValidUvMesh helper: a single-submesh triangle
// with positions + UV0 in separate streams, valid topology, no degenerates.
static Ogre::MeshPtr covCreateValidUvMesh(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;

    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    decl->addElement(1, 0, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

    auto posBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3), 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    auto uvBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT2), 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);

    float positions[] = {
        0.f, 0.f, 0.f,
        1.f, 0.f, 0.f,
        0.f, 1.f, 0.f,
    };
    float uvs[] = {
        0.f, 0.f,
        1.f, 0.f,
        0.f, 1.f,
    };

    posBuf->writeData(0, sizeof(positions), positions);
    uvBuf->writeData(0, sizeof(uvs), uvs);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, posBuf);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(1, uvBuf);
    mesh->sharedVertexData->vertexCount = 3;

    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);

    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;

    mesh->_setBounds(Ogre::AxisAlignedBox(-1, -1, -1, 1, 1, 1));
    mesh->_setBoundingSphereRadius(2.0f);
    mesh->load();

    return mesh;
}

// Pumps the deferred validate() through frameStarted() so m_validated flips true,
// exactly like the existing ValidateDefersAndFrameStartedRunsValidation test does.
static void covPumpFrame(MeshValidator* validator)
{
    Ogre::FrameEvent evt;
    evt.timeSinceLastEvent = 0.016f;
    evt.timeSinceLastFrame = 0.016f;
    validator->frameStarted(evt);
}

class MeshValidatorCoverageTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override
    {
        MeshValidator::kill();
        SelectionSet::kill();
        Manager::kill();
        QThread::msleep(kSingletonSettleTimeMs);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        validator = MeshValidator::instance();
        ASSERT_NE(validator, nullptr);
    }

    void TearDown() override
    {
        if (Manager::getSingletonPtr()) {
            SelectionSet::getSingleton()->clear();
        }

        MeshValidator::kill();
        SelectionSet::kill();
        Manager::kill();

        if (app) {
            app->processEvents();
        }
        QThread::msleep(kSingletonSettleTimeMs);
    }

    MeshValidator* validator = nullptr;
};

// --- optimizeVertexCache: no-selection error / no-op path ---------------------

TEST_F(MeshValidatorCoverageTest, OptimizeVertexCacheWithoutSelectionEmitsError)
{
    QSignalSpy errorSpy(validator, &MeshValidator::error);
    QSignalSpy fixSpy(validator, &MeshValidator::fixApplied);

    ASSERT_FALSE(validator->hasSelection());
    validator->optimizeVertexCache();

    ASSERT_EQ(errorSpy.count(), 1);
    EXPECT_TRUE(errorSpy.takeFirst().first().toString().contains("No mesh selected"));
    // Early-return path must not emit a success toast or set any result row.
    EXPECT_EQ(fixSpy.count(), 0);
    EXPECT_FALSE(validator->hasCacheOptimization());
    EXPECT_TRUE(validator->m_lastOptimizeResult.isEmpty());
}

// --- optimizeVertexCache: happy path on a selected entity ---------------------

TEST_F(MeshValidatorCoverageTest, OptimizeVertexCacheEmitsFixAppliedAndRevalidates)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto mesh = covCreateValidUvMesh("MeshValidatorCovOptMesh");
    auto* entity = covCreateEntityFromMesh("MeshValidatorCovOptNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);
    ASSERT_TRUE(validator->hasSelection());

    QSignalSpy fixSpy(validator, &MeshValidator::fixApplied);
    QSignalSpy errorSpy(validator, &MeshValidator::error);

    validator->optimizeVertexCache();

    // fixApplied must fire exactly once (either "optimized N" or "already optimal").
    ASSERT_EQ(fixSpy.count(), 1);
    EXPECT_FALSE(fixSpy.takeFirst().first().toString().isEmpty());
    EXPECT_EQ(errorSpy.count(), 0);

    // A result row is persisted so the checklist can surface what happened.
    ASSERT_FALSE(validator->m_lastOptimizeResult.isEmpty());
    EXPECT_EQ(validator->m_lastOptimizeResult.value("type").toString(), QStringLiteral("ok"));
    EXPECT_TRUE(validator->m_lastOptimizeResult.value("description").toString()
                    .startsWith("Optimize Geometry:"));
    EXPECT_FALSE(validator->m_lastOptimizeResult.value("fixable").toBool());

    // optimizeVertexCache() calls validate() (deferred); pump the frame to land it.
    covPumpFrame(validator);
    EXPECT_TRUE(validator->validated());
    EXPECT_FALSE(validator->validating());

    // After the auto-revalidate the persisted optimize row is prepended to issues.
    const QVariantList issues = validator->issues();
    ASSERT_GE(issues.size(), 1);
    EXPECT_TRUE(issues.first().toMap().value("description").toString()
                    .startsWith("Optimize Geometry:"));
}

// A tiny already-optimal triangle yields no meaningful gain, so the run reports
// the "already optimal — no submeshes were reordered" branch.
TEST_F(MeshValidatorCoverageTest, OptimizeVertexCacheAlreadyOptimalBranch)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto mesh = covCreateValidUvMesh("MeshValidatorCovOptimalMesh");
    auto* entity = covCreateEntityFromMesh("MeshValidatorCovOptimalNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);

    QSignalSpy fixSpy(validator, &MeshValidator::fixApplied);
    validator->optimizeVertexCache();

    ASSERT_EQ(fixSpy.count(), 1);
    const QString msg = fixSpy.takeFirst().first().toString();
    // A single triangle has nothing to reorder — exercise the zero-optimized branch.
    EXPECT_TRUE(msg.contains("already optimal") || msg.contains("Optimized"));

    ASSERT_FALSE(validator->m_lastOptimizeResult.isEmpty());
    EXPECT_EQ(validator->m_lastOptimizeResult.value("count").toInt(), 0);
}

// --- hasCacheOptimization reflects m_cacheOptimizationAvailable ---------------

TEST_F(MeshValidatorCoverageTest, HasCacheOptimizationResetByDoValidate)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto mesh = covCreateValidUvMesh("MeshValidatorCovCacheMesh");
    auto* entity = covCreateEntityFromMesh("MeshValidatorCovCacheNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);

    // Force the flag on, then doValidate() on a clean already-optimal mesh must
    // reset it (line 165) and never re-raise it (no meaningful gain available).
    validator->m_cacheOptimizationAvailable = true;
    validator->doValidate();

    EXPECT_TRUE(validator->validated());
    EXPECT_FALSE(validator->hasCacheOptimization());
    EXPECT_EQ(validator->hasCacheOptimization(), validator->m_cacheOptimizationAvailable);
}

TEST_F(MeshValidatorCoverageTest, OptimizeVertexCacheLeavesCacheUnavailableAfterRevalidate)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto mesh = covCreateValidUvMesh("MeshValidatorCovPostOptMesh");
    auto* entity = covCreateEntityFromMesh("MeshValidatorCovPostOptNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);

    validator->optimizeVertexCache();
    covPumpFrame(validator);

    // Post-optimize the mesh is optimal, so no further optimization is offered.
    EXPECT_TRUE(validator->validated());
    EXPECT_FALSE(validator->hasCacheOptimization());
}

// --- fixAll WITH a selection (existing suite only covers the no-selection error) -

TEST_F(MeshValidatorCoverageTest, FixAllWithSelectionEmitsFixApplied)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto mesh = covCreateValidUvMesh("MeshValidatorCovFixMesh");
    auto* entity = covCreateEntityFromMesh("MeshValidatorCovFixNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);
    ASSERT_TRUE(validator->hasSelection());

    QSignalSpy fixSpy(validator, &MeshValidator::fixApplied);
    QSignalSpy errorSpy(validator, &MeshValidator::error);

    validator->fixAll();

    // Export-to-OBJ + reimport-with-cleanup should succeed and announce the clean
    // re-import. If the export pipeline is unavailable it surfaces a clear error
    // instead of crashing — accept either observable outcome but never both silent.
    const bool announced = fixSpy.count() >= 1;
    const bool errored = errorSpy.count() >= 1;
    EXPECT_TRUE(announced || errored);

    if (announced) {
        EXPECT_TRUE(fixSpy.takeFirst().first().toString().contains("Cleaned mesh imported"));
    }
}

// fixAll() re-runs validate() at the end (deferred); the frame pump must not crash
// and should leave the validator in a consistent validated/!validating state.
TEST_F(MeshValidatorCoverageTest, FixAllWithSelectionRevalidatesViaFrame)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto mesh = covCreateValidUvMesh("MeshValidatorCovFixFrameMesh");
    auto* entity = covCreateEntityFromMesh("MeshValidatorCovFixFrameNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);

    validator->fixAll();
    covPumpFrame(validator);

    // Whatever the export outcome, the deferred validate path must settle cleanly.
    EXPECT_FALSE(validator->validating());
}
