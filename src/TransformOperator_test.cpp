#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>

#include <Ogre.h>

#define private public
#define protected public
#include "TransformOperator.h"
#undef protected
#undef private

#include "commands/TransformCommands.h"
#include "GlobalDefinitions.h"
#include "Manager.h"
#include "RotationGizmo.h"
#include "SelectionBoxObject.h"
#include "SelectionSet.h"
#include "ScaleGizmo.h"
#include "TestHelpers.h"
#include "TranslationGizmo.h"
#include "UndoManager.h"

namespace {
void expectQuaternionNear(const Ogre::Quaternion& actual, const Ogre::Quaternion& expected)
{
    EXPECT_FLOAT_EQ(actual.w, expected.w);
    EXPECT_FLOAT_EQ(actual.x, expected.x);
    EXPECT_FLOAT_EQ(actual.y, expected.y);
    EXPECT_FLOAT_EQ(actual.z, expected.z);
}
}

Q_DECLARE_METATYPE(Ogre::Vector3)

class TransformOperatorTests : public ::testing::Test
{
protected:
    void SetUp() override
    {
        TransformOperator::kill();
        UndoManager::kill();
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }

        createStandardOgreMaterials();
        op = TransformOperator::getSingleton();
        ASSERT_NE(op, nullptr);
    }

    void TearDown() override
    {
        TransformOperator::kill();
        UndoManager::kill();
        Manager::kill();
        if (app)
            app->processEvents();
    }

    Ogre::SceneNode* createSelectedNode(const QString& name)
    {
        Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode(name);
        EXPECT_NE(node, nullptr);
        if (node)
            SelectionSet::getSingleton()->selectOne(node);
        return node;
    }

    Ogre::Entity* createSelectedEntity(const QString& nodeName,
                                       const QString& entityName,
                                       const std::string& meshName)
    {
        if (!canLoadMeshFiles())
            return nullptr;

        Ogre::MeshPtr mesh = createInMemoryTriangleMesh(meshName);
        EXPECT_NE(mesh, nullptr);

        Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode(nodeName);
        EXPECT_NE(node, nullptr);
        if (!mesh || !node)
            return nullptr;

        Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(entityName.toStdString(), mesh);
        EXPECT_NE(entity, nullptr);
        if (!entity)
            return nullptr;

        node->attachObject(entity);
        SelectionSet::getSingleton()->selectOne(entity);
        return entity;
    }

    QApplication* app = nullptr;
    TransformOperator* op = nullptr;
};

TEST(TransformOperatorTest, Swap)
{
    int x = 1;
    int y = 2;
    TransformOperator::swap(x, y);
    EXPECT_EQ(x, 2);
    EXPECT_EQ(y, 1);
}

TEST_F(TransformOperatorTests, TransformSpaceChangesOnlyWhenValueDiffers)
{
    QSignalSpy spy(op, &TransformOperator::transformSpaceChanged);
    ASSERT_TRUE(spy.isValid());

    EXPECT_EQ(op->getTransformSpace(), TransformOperator::SPACE_WORLD);

    op->setTransformSpace(TransformOperator::SPACE_LOCAL);
    op->setTransformSpace(TransformOperator::SPACE_LOCAL);
    op->toggleTransformSpace();

    EXPECT_EQ(op->getTransformSpace(), TransformOperator::SPACE_WORLD);
    EXPECT_EQ(spy.count(), 2);
}

TEST_F(TransformOperatorTests, TransformStateChangeWithoutSelectionUpdatesStateAndTracking)
{
    op->onTransformStateChange(TransformOperator::TS_TRANSLATE);

    EXPECT_FALSE(op->mTrackingEnable);
    EXPECT_EQ(op->mTransformState, TransformOperator::TS_TRANSLATE);
}

TEST_F(TransformOperatorTests, SelectionBoxColourRoundTrips)
{
    const Ogre::ColourValue colour(0.1f, 0.2f, 0.3f, 0.4f);
    op->setSelectionBoxColour(colour);

    const Ogre::ColourValue current = op->getSelectionBoxColour();
    EXPECT_FLOAT_EQ(current.r, colour.r);
    EXPECT_FLOAT_EQ(current.g, colour.g);
    EXPECT_FLOAT_EQ(current.b, colour.b);
    EXPECT_FLOAT_EQ(current.a, colour.a);
}

TEST_F(TransformOperatorTests, RayFromScreenPointWithoutActiveWidgetReturnsDefaultRay)
{
    const Ogre::Ray ray = op->rayFromScreenPoint(QPoint(25, 40));
    const Ogre::Ray defaultRay;
    EXPECT_EQ(ray.getOrigin(), defaultRay.getOrigin());
    EXPECT_EQ(ray.getDirection(), defaultRay.getDirection());
}

TEST_F(TransformOperatorTests, UpdateGizmoPositionForNodeSelectionEmitsCurrentValues)
{
    Ogre::SceneNode* node = createSelectedNode("SignalNode");
    ASSERT_NE(node, nullptr);
    node->setPosition(Ogre::Vector3(2.0f, 3.0f, 4.0f));
    node->setScale(Ogre::Vector3(1.5f, 2.5f, 3.5f));

    QSignalSpy positionSpy(op, &TransformOperator::selectedPositionChanged);
    QSignalSpy scaleSpy(op, &TransformOperator::selectedScaleChanged);
    ASSERT_TRUE(positionSpy.isValid());
    ASSERT_TRUE(scaleSpy.isValid());

    op->updateGizmoPosition();

    ASSERT_FALSE(positionSpy.isEmpty());
    ASSERT_FALSE(scaleSpy.isEmpty());
    EXPECT_EQ(qvariant_cast<Ogre::Vector3>(positionSpy.takeLast().at(0)), Ogre::Vector3(2.0f, 3.0f, 4.0f));
    EXPECT_EQ(qvariant_cast<Ogre::Vector3>(scaleSpy.takeLast().at(0)), Ogre::Vector3(1.5f, 2.5f, 3.5f));
}

TEST_F(TransformOperatorTests, SelectedNodeTransformsUpdateNodeState)
{
    Ogre::SceneNode* node = createSelectedNode("TransformNode");
    ASSERT_NE(node, nullptr);

    op->setSelectedPosition(Ogre::Vector3(5.0f, 6.0f, 7.0f));
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(5.0f, 6.0f, 7.0f));

    op->setSelectedScale(Ogre::Vector3(2.0f, 3.0f, 4.0f));
    EXPECT_EQ(node->getScale(), Ogre::Vector3(2.0f, 3.0f, 4.0f));

    op->setSelectedOrientation(Ogre::Vector3(15.0f, 25.0f, 35.0f));
    EXPECT_NE(node->getOrientation(), Ogre::Quaternion::IDENTITY);

    op->translateSelected(Ogre::Vector3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(6.0f, 8.0f, 10.0f));
}

TEST_F(TransformOperatorTests, EmptySelectionTransformMutatorsAreNoOps)
{
    EXPECT_NO_THROW(op->setSelectedPosition(Ogre::Vector3(1.0f, 2.0f, 3.0f)));
    EXPECT_NO_THROW(op->translateSelected(Ogre::Vector3(1.0f, 0.0f, 0.0f)));
    EXPECT_NO_THROW(op->setSelectedScale(Ogre::Vector3(2.0f, 2.0f, 2.0f)));
    EXPECT_NO_THROW(op->scaleSelected(Ogre::Vector3(1.1f, 1.1f, 1.1f)));
    EXPECT_NO_THROW(op->setSelectedOrientation(Ogre::Vector3(10.0f, 20.0f, 30.0f)));
    EXPECT_NO_THROW(op->rotateSelected(Ogre::Quaternion::IDENTITY));
    EXPECT_NO_THROW(op->rotateSelected(Ogre::Vector3(0.0f, 0.0f, 0.0f)));
}

TEST_F(TransformOperatorTests, OnSelectionChangedRestoresNodeInitialState)
{
    Ogre::SceneNode* node = createSelectedNode("InitialStateNode");
    ASSERT_NE(node, nullptr);

    // onSelectionChanged() restores a selected SceneNode to its explicitly saved initial state.
    node->setScale(Ogre::Vector3(2.0f, 2.0f, 2.0f));
    const Ogre::Quaternion expectedOrientation(Ogre::Degree(30), Ogre::Vector3::UNIT_Y);
    node->setOrientation(expectedOrientation);
    node->setInitialState();

    node->setScale(Ogre::Vector3(5.0f, 6.0f, 7.0f));
    node->setOrientation(Ogre::Quaternion(Ogre::Degree(80), Ogre::Vector3::UNIT_X));

    op->onSelectionChanged();

    EXPECT_EQ(node->getScale(), Ogre::Vector3(2.0f, 2.0f, 2.0f));
    expectQuaternionNear(node->getOrientation(), expectedOrientation);
}

TEST_F(TransformOperatorTests, OnSelectionChangedNormalizesSelectedEntityParentNode)
{
    Ogre::Entity* entity = createSelectedEntity("EntityStateNode", "EntityStateEntity", "EntityStateMesh");
    if (!entity) {
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";
    }

    Ogre::SceneNode* parentNode = entity->getParentSceneNode();
    // onSelectionChanged() treats an Entity selection differently: it normalizes the parent SceneNode
    // back to identity instead of restoring a previously saved SceneNode initial state.
    parentNode->setScale(Ogre::Vector3(3.0f, 4.0f, 5.0f));
    parentNode->setOrientation(Ogre::Quaternion(Ogre::Degree(45), Ogre::Vector3::UNIT_Z));

    op->onSelectionChanged();

    EXPECT_EQ(parentNode->getScale(), Ogre::Vector3::UNIT_SCALE);
    expectQuaternionNear(parentNode->getOrientation(), Ogre::Quaternion::IDENTITY);
}

TEST_F(TransformOperatorTests, OnSelectionChangedNormalizesSelectedSubEntityParentNode)
{
    Ogre::Entity* entity = createSelectedEntity("SubEntityStateNode", "SubEntityStateEntity", "SubEntityStateMesh");
    if (!entity) {
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";
    }

    Ogre::SubEntity* subEntity = entity->getSubEntity(0);
    ASSERT_NE(subEntity, nullptr);

    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(subEntity);

    Ogre::SceneNode* parentNode = entity->getParentSceneNode();
    parentNode->setScale(Ogre::Vector3(6.0f, 7.0f, 8.0f));
    parentNode->setOrientation(Ogre::Quaternion(Ogre::Degree(15), Ogre::Vector3::UNIT_X));

    op->onSelectionChanged();

    EXPECT_EQ(parentNode->getScale(), Ogre::Vector3::UNIT_SCALE);
    expectQuaternionNear(parentNode->getOrientation(), Ogre::Quaternion::IDENTITY);
}

TEST_F(TransformOperatorTests, EntityRotationVectorSetterTracksAbsoluteRotation)
{
    Ogre::Entity* entity = createSelectedEntity("EntityRotateNode", "EntityRotateEntity", "EntityRotateMesh");
    if (!entity) {
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";
    }

    op->rotateSelected(Ogre::Vector3(10.0f, 20.0f, 30.0f));
    EXPECT_EQ(SelectionSet::getSingleton()->getEntityRotation(entity), Ogre::Vector3(10.0f, 20.0f, 30.0f));

    op->rotateSelected(Ogre::Vector3(15.0f, 25.0f, 35.0f));
    EXPECT_EQ(SelectionSet::getSingleton()->getEntityRotation(entity), Ogre::Vector3(15.0f, 25.0f, 35.0f));
}

TEST_F(TransformOperatorTests, EntityScaleSetterTracksScaleFactor)
{
    Ogre::Entity* entity = createSelectedEntity("EntityScaleNode", "EntityScaleEntity", "EntityScaleMesh");
    if (!entity) {
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";
    }

    op->setSelectedScale(Ogre::Vector3(1.2f, 1.3f, 1.4f));

    EXPECT_EQ(SelectionSet::getSingleton()->getEntityScaleFactor(entity), Ogre::Vector3(1.2f, 1.3f, 1.4f));
}

TEST_F(TransformOperatorTests, RemoveSelectedWithEmptySelectionKeepsUndoHistory)
{
    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("HistoryNode");
    ASSERT_NE(node, nullptr);

    QList<Ogre::SceneNode*> nodes;
    nodes.append(node);
    UndoManager::getSingleton()->push(new TranslateCommand(nodes, Ogre::Vector3(1.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(UndoManager::getSingleton()->canUndo());

    SelectionSet::getSingleton()->clear();
    op->removeSelected();

    EXPECT_TRUE(UndoManager::getSingleton()->canUndo());
}

TEST_F(TransformOperatorTests, RemoveSelectedDestroysNodesAndClearsUndoHistory)
{
    Ogre::SceneNode* node = createSelectedNode("DeleteNode");
    ASSERT_NE(node, nullptr);

    QList<Ogre::SceneNode*> nodes;
    nodes.append(node);
    UndoManager::getSingleton()->push(new TranslateCommand(nodes, Ogre::Vector3(1.0f, 0.0f, 0.0f)));
    ASSERT_TRUE(UndoManager::getSingleton()->canUndo());

    op->removeSelected();

    EXPECT_FALSE(Manager::getSingleton()->getSceneMgr()->hasSceneNode("DeleteNode"));
    EXPECT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_FALSE(UndoManager::getSingleton()->canUndo());
}
