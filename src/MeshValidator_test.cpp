#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>
#include <limits>

#define private public
#include "MeshValidator.h"
#undef private

#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

static constexpr unsigned long kSingletonSettleTimeMs = 30;

static Ogre::Entity* createEntityFromMesh(const std::string& nodeName, const Ogre::MeshPtr& mesh)
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

static Ogre::MeshPtr createDegenerateUvMesh(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;

    // Put positions and UVs in separate streams so MeshValidator can lock both
    // simultaneously (it locks position first and then UVs).
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    decl->addElement(1, 0, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

    auto posBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3), 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    auto uvBuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT2), 3,
        Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    float positions[] = {
        0.f, 0.f, 0.f,
        1.f, 0.f, 0.f,
        2.f, 0.f, 0.f, // colinear vertex => degenerate triangle
    };
    float uvs[] = {
        nan, nan, // non-finite UV
        20.f, 0.f, // out-of-range UV
        0.f, 0.f,
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

    mesh->_setBounds(Ogre::AxisAlignedBox(-1, -1, -1, 2, 1, 1));
    mesh->_setBoundingSphereRadius(2.0f);
    mesh->load();

    return mesh;
}

static Ogre::MeshPtr createValidUvMesh(const std::string& name)
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

class MeshValidatorTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override
    {
        MeshValidator::kill();
        SelectionSet::kill();
        Manager::kill();
        // Give QObject teardown from previous tests a short settle window.
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

TEST_F(MeshValidatorTest, InstanceAndKillLifecycle)
{
    auto* a = MeshValidator::instance();
    auto* b = MeshValidator::instance();
    EXPECT_EQ(a, b);

    // Mutate state so we can verify the fresh singleton starts with defaults.
    a->m_pendingValidate = true;
    a->m_validated = true;

    MeshValidator::kill();
    auto* c = MeshValidator::instance();
    EXPECT_NE(c, nullptr);
    EXPECT_FALSE(c->m_pendingValidate);
    EXPECT_FALSE(c->m_validated);

    validator = c;
}

TEST_F(MeshValidatorTest, HasSelectionReflectsSelectionSet)
{
    EXPECT_FALSE(validator->hasSelection());

    ASSERT_TRUE(canLoadMeshFiles());

    auto mesh = createValidUvMesh("MeshValidatorHasSelectionMesh");
    auto* entity = createEntityFromMesh("MeshValidatorHasSelectionNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);
    EXPECT_TRUE(validator->hasSelection());
}

TEST_F(MeshValidatorTest, ValidateWithoutSelectionDoesNotEnterPending)
{
    QSignalSpy validatingSpy(validator, &MeshValidator::validatingChanged);

    validator->validate();

    EXPECT_FALSE(validator->validating());
    EXPECT_FALSE(validator->validated());
    EXPECT_TRUE(validator->issues().isEmpty());
    EXPECT_EQ(validatingSpy.count(), 0);
}

TEST_F(MeshValidatorTest, DoValidateWithoutSelectionKeepsStateUnvalidated)
{
    validator->doValidate();

    EXPECT_FALSE(validator->validated());
    EXPECT_TRUE(validator->issues().isEmpty());
    EXPECT_FALSE(validator->hasFixableIssues());
}

TEST_F(MeshValidatorTest, DoValidateValidMeshReturnsOkIssue)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto mesh = createValidUvMesh("MeshValidatorValidMesh");
    auto* entity = createEntityFromMesh("MeshValidatorValidNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);
    validator->doValidate();

    EXPECT_TRUE(validator->validated());
    EXPECT_FALSE(validator->validating());
    EXPECT_FALSE(validator->hasFixableIssues());

    const QVariantList issues = validator->issues();
    ASSERT_EQ(issues.size(), 1);

    const QVariantMap issue = issues.first().toMap();
    EXPECT_EQ(issue.value("type").toString(), QStringLiteral("ok"));
    EXPECT_EQ(issue.value("description").toString(), QStringLiteral("No issues found."));
    EXPECT_EQ(issue.value("count").toInt(), 0);
    EXPECT_FALSE(issue.value("fixable").toBool());
}

TEST_F(MeshValidatorTest, DoValidateDetectsDegeneratesAndUvProblems)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto mesh = createDegenerateUvMesh("MeshValidatorBadMesh");
    auto* entity = createEntityFromMesh("MeshValidatorBadNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);
    validator->doValidate();

    EXPECT_TRUE(validator->validated());
    EXPECT_TRUE(validator->hasFixableIssues());

    bool sawDegenerate = false;
    bool sawNonFinite = false;
    bool sawExtremeUv = false;
    for (const QVariant& issueVariant : validator->issues()) {
        const QVariantMap issue = issueVariant.toMap();
        const QString description = issue.value("description").toString();
        sawDegenerate |= description.contains("degenerate triangle");
        sawNonFinite |= description.contains("non-finite UV");
        sawExtremeUv |= description.contains("extreme UV values");
    }

    EXPECT_TRUE(sawDegenerate);
    EXPECT_TRUE(sawNonFinite);
    EXPECT_TRUE(sawExtremeUv);
}

TEST_F(MeshValidatorTest, ValidateDefersAndFrameStartedRunsValidation)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto mesh = createValidUvMesh("MeshValidatorFrameMesh");
    auto* entity = createEntityFromMesh("MeshValidatorFrameNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);

    QSignalSpy validatingSpy(validator, &MeshValidator::validatingChanged);
    validator->validate();

    EXPECT_TRUE(validator->validating());
    EXPECT_TRUE(validator->m_pendingValidate);

    Ogre::FrameEvent evt;
    evt.timeSinceLastEvent = 0.016f;
    evt.timeSinceLastFrame = 0.016f;
    EXPECT_TRUE(validator->frameStarted(evt));

    EXPECT_FALSE(validator->validating());
    EXPECT_FALSE(validator->m_pendingValidate);
    EXPECT_TRUE(validator->validated());
    EXPECT_GE(validatingSpy.count(), 2);
}

TEST_F(MeshValidatorTest, SelectionChangeClearsIssuesAndCancelsPendingValidation)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto mesh = createValidUvMesh("MeshValidatorSelectionClearMesh");
    auto* entity = createEntityFromMesh("MeshValidatorSelectionClearNode", mesh);
    ASSERT_NE(entity, nullptr);

    SelectionSet::getSingleton()->selectOne(entity);
    validator->validate();
    ASSERT_TRUE(validator->validating());

    SelectionSet::getSingleton()->clear();

    EXPECT_FALSE(validator->validating());
    EXPECT_FALSE(validator->validated());
    EXPECT_TRUE(validator->issues().isEmpty());
}

TEST_F(MeshValidatorTest, FixAllWithoutSelectionEmitsError)
{
    QSignalSpy errorSpy(validator, &MeshValidator::error);

    validator->fixAll();

    ASSERT_EQ(errorSpy.count(), 1);
    EXPECT_TRUE(errorSpy.takeFirst().first().toString().contains("No mesh selected"));
}
