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

TEST_F(MeshValidatorTest, HasSelectionTrueWhenSceneNodeSelected)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto mesh = createValidUvMesh("MeshValidatorNodeSelMesh");
    auto* entity = createEntityFromMesh("MeshValidatorNodeSelNode", mesh);
    ASSERT_NE(entity, nullptr);

    auto* sel = SelectionSet::getSingleton();
    EXPECT_TRUE(sel->hasNodes());
    EXPECT_FALSE(sel->hasEntities());
    EXPECT_TRUE(validator->hasSelection());
}

TEST_F(MeshValidatorTest, HasSelectionTrueWhenSubEntitySelected)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto mesh = createValidUvMesh("MeshValidatorSubSelMesh");
    auto* entity = createEntityFromMesh("MeshValidatorSubSelNode", mesh);
    ASSERT_NE(entity, nullptr);

    ASSERT_GE(entity->getNumSubEntities(), 1u);
    SelectionSet::getSingleton()->selectOne(entity->getSubEntity(0));
    EXPECT_FALSE(SelectionSet::getSingleton()->hasEntities());
    EXPECT_TRUE(validator->hasSelection());

    validator->doValidate();
    EXPECT_TRUE(validator->validated());
    const QVariantList issues = validator->issues();
    // Phase 6: validation now emits a checklist (geometry/UVs/draws/memory)
    // instead of a single "No issues found." row.  At minimum the geometry
    // row must be present and be "ok" for this valid mesh.
    ASSERT_GE(issues.size(), 1);
    EXPECT_EQ(issues.first().toMap().value("type").toString(), QStringLiteral("ok"));
    EXPECT_TRUE(issues.first().toMap().value("description").toString().startsWith("Geometry:"));
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

TEST_F(MeshValidatorTest, DoValidateValidMeshReportsChecklist)
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

    // Phase 6: the validator now emits a per-dimension checklist for a valid
    // mesh — at minimum a Geometry-ok row, a UVs row, plus the new Draws/GPU
    // info rows from slices A+B. The user sees what was actually checked
    // instead of a bare "No issues found." line.
    const QVariantList issues = validator->issues();
    ASSERT_GE(issues.size(), 2);

    bool sawGeometryOk = false;
    bool sawUvsOk = false;
    bool sawDraws = false;
    bool sawCache = false;
    bool sawGpu = false;
    for (const QVariant& issueVariant : issues) {
        const QVariantMap issue = issueVariant.toMap();
        const QString type = issue.value("type").toString();
        const QString description = issue.value("description").toString();

        // No row may be an error or warning on a clean mesh.
        EXPECT_NE(type, QStringLiteral("error")) << description.toStdString();
        EXPECT_NE(type, QStringLiteral("warning")) << description.toStdString();

        if (description.startsWith("Geometry:")     && type == "ok") sawGeometryOk = true;
        if (description.startsWith("UVs:")          && type == "ok") sawUvsOk = true;
        if (description.startsWith("Draws:"))                        sawDraws = true;
        if (description.startsWith("Vertex cache:"))                 sawCache = true;
        if (description.startsWith("GPU:"))                          sawGpu = true;
    }
    EXPECT_TRUE(sawGeometryOk);
    EXPECT_TRUE(sawUvsOk);
    EXPECT_TRUE(sawDraws);
    EXPECT_TRUE(sawCache);
    EXPECT_TRUE(sawGpu);
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
        // Phase 6: rows now have "Geometry:" / "UVs:" prefixes.
        sawDegenerate |= description.contains("degenerate triangle");
        sawNonFinite |= description.contains("non-finite");
        sawExtremeUv |= description.contains("extreme values");
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
