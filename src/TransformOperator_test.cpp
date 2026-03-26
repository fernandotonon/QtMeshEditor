#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>

#include <Ogre.h>

#include "commands/TransformCommands.h"
#include "GlobalDefinitions.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "TransformOperator.h"
#include "UndoManager.h"

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
    EXPECT_EQ(ray.getOrigin(), Ogre::Vector3::ZERO);
    EXPECT_EQ(ray.getDirection(), Ogre::Vector3::ZERO);
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

TEST_F(TransformOperatorTests, OnSelectionChangedRestoresNodeInitialState)
{
    Ogre::SceneNode* node = createSelectedNode("InitialStateNode");
    ASSERT_NE(node, nullptr);

    node->setScale(Ogre::Vector3(2.0f, 2.0f, 2.0f));
    node->setOrientation(Ogre::Quaternion(Ogre::Degree(30), Ogre::Vector3::UNIT_Y));
    node->setInitialState();

    node->setScale(Ogre::Vector3(5.0f, 6.0f, 7.0f));
    node->setOrientation(Ogre::Quaternion(Ogre::Degree(80), Ogre::Vector3::UNIT_X));

    op->onSelectionChanged();

    EXPECT_EQ(node->getScale(), Ogre::Vector3(2.0f, 2.0f, 2.0f));
    EXPECT_EQ(node->getOrientation(), Ogre::Quaternion(Ogre::Degree(30), Ogre::Vector3::UNIT_Y));
}

TEST_F(TransformOperatorTests, OnSelectionChangedNormalizesSelectedEntityParentNode)
{
    Ogre::Entity* entity = createSelectedEntity("EntityStateNode", "EntityStateEntity", "EntityStateMesh");
    if (!entity) {
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";
    }

    Ogre::SceneNode* parentNode = entity->getParentSceneNode();
    parentNode->setScale(Ogre::Vector3(3.0f, 4.0f, 5.0f));
    parentNode->setOrientation(Ogre::Quaternion(Ogre::Degree(45), Ogre::Vector3::UNIT_Z));

    op->onSelectionChanged();

    EXPECT_EQ(parentNode->getScale(), Ogre::Vector3::UNIT_SCALE);
    EXPECT_EQ(parentNode->getOrientation(), Ogre::Quaternion::IDENTITY);
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
