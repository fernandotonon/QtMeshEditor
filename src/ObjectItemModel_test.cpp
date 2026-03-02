#include <gtest/gtest.h>
#include <QApplication>
#include <QThread>
#include <QStandardItem>
#include "Manager.h"
#include "ObjectItemModel.h"
#include "SelectionSet.h"
#include "PrimitiveObject.h"
#include <OgreException.h>
#include "TestHelpers.h"

class ObjectItemModelTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();
    }

    void TearDown() override
    {
        if (app)
        {
            app->processEvents();
        }
    }

private:
    QApplication* app = nullptr;
};

// Test: Constructor creates model with correct header
TEST_F(ObjectItemModelTest, ConstructorCreatesModelWithCorrectHeader)
{
    ObjectItemModel model;

    // The constructor calls reloadSceneNode which sets header to "No Selection"
    QVariant headerData = model.headerData(0, Qt::Horizontal, Qt::DisplayRole);
    EXPECT_EQ(headerData.toString(), QString("No Selection"));
}

// Test: Constructor creates model with root item "Scene"
TEST_F(ObjectItemModelTest, ConstructorCreatesRootSceneItem)
{
    ObjectItemModel model;

    // The root item should exist and be named "Scene"
    QModelIndex rootIndex = model.getRootIndex();
    EXPECT_TRUE(rootIndex.isValid());

    QString rootText = model.data(rootIndex, Qt::DisplayRole).toString();
    EXPECT_EQ(rootText, QString("Scene"));
}

// Test: reloadSceneNode builds tree from scene graph
TEST_F(ObjectItemModelTest, ReloadSceneNodeBuildsTreeFromSceneGraph)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    // Create a primitive to add a node to the scene
    PrimitiveObject::createCube("TestCube");

    // Create the model - constructor calls reloadSceneNode which should pick up the existing node
    ObjectItemModel model;

    QModelIndex rootIndex = model.getRootIndex();
    EXPECT_TRUE(rootIndex.isValid());

    // The root item should have at least one child (the TestCube node)
    int childCount = model.rowCount(rootIndex);
    EXPECT_GE(childCount, 1);

    // Verify the child node text contains the name
    bool foundCube = false;
    for (int i = 0; i < childCount; ++i)
    {
        QModelIndex childIndex = model.index(i, 0, rootIndex);
        QString childText = model.data(childIndex, Qt::DisplayRole).toString();
        if (childText.contains("TestCube"))
        {
            foundCube = true;
            break;
        }
    }
    EXPECT_TRUE(foundCube);

    Manager::getSingleton()->destroySceneNode("TestCube");
}

// Test: reloadSceneNode rebuilds the tree (clears and repopulates)
TEST_F(ObjectItemModelTest, ReloadSceneNodeRebuildsTree)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    ObjectItemModel model;

    QModelIndex rootIndex = model.getRootIndex();
    EXPECT_TRUE(rootIndex.isValid());
    int initialChildCount = model.rowCount(rootIndex);

    // Add a node
    PrimitiveObject::createCube("ReloadTestCube");

    // Manually trigger reload (simulates what entityCreated signal does)
    // The signal is already connected, but since PrimitiveObject::createCube
    // also triggers entityCreated, let us check the model
    // After the signal, the model should have rebuilt
    QModelIndex newRootIndex = model.getRootIndex();
    EXPECT_TRUE(newRootIndex.isValid());
    int newChildCount = model.rowCount(newRootIndex);
    EXPECT_GT(newChildCount, initialChildCount);

    Manager::getSingleton()->destroySceneNode("ReloadTestCube");
}

// Test: newObjectNode adds nodes to the model
TEST_F(ObjectItemModelTest, NewObjectNodeAddsNodesToModel)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    ObjectItemModel model;

    QModelIndex rootIndex = model.getRootIndex();
    int initialChildCount = model.rowCount(rootIndex);

    // Creating a primitive triggers sceneNodeCreated signal, which calls newObjectNode
    PrimitiveObject::createCube("AddNodeTestCube");

    // After signal processing, the model root index may have changed due to reload
    // (entityCreated also triggers reloadSceneNode), so fetch the root again
    QModelIndex updatedRootIndex = model.getRootIndex();
    int updatedChildCount = model.rowCount(updatedRootIndex);

    EXPECT_GT(updatedChildCount, initialChildCount);

    // Verify the node is present in the tree
    bool foundNode = false;
    for (int i = 0; i < updatedChildCount; ++i)
    {
        QModelIndex childIndex = model.index(i, 0, updatedRootIndex);
        QString childText = model.data(childIndex, Qt::DisplayRole).toString();
        if (childText.contains("AddNodeTestCube"))
        {
            foundNode = true;
            break;
        }
    }
    EXPECT_TRUE(foundNode);

    Manager::getSingleton()->destroySceneNode("AddNodeTestCube");
}

// Test: objectNodeRemoved removes nodes from the model
TEST_F(ObjectItemModelTest, ObjectNodeRemovedRemovesNodes)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    // Create a primitive first
    PrimitiveObject::createCube("RemoveTestCube");

    ObjectItemModel model;

    QModelIndex rootIndex = model.getRootIndex();
    int childCountBefore = model.rowCount(rootIndex);
    EXPECT_GE(childCountBefore, 1);

    // Destroy the scene node - this triggers sceneNodeDestroyed signal
    // which calls objectNodeRemoved
    Manager::getSingleton()->destroySceneNode("RemoveTestCube");

    // After the signal, the model should be rebuilt (reloadSceneNode is triggered)
    QModelIndex newRootIndex = model.getRootIndex();
    int childCountAfter = model.rowCount(newRootIndex);

    EXPECT_LT(childCountAfter, childCountBefore);
}

// Test: getRootIndex returns valid index
TEST_F(ObjectItemModelTest, GetRootIndexReturnsValidIndex)
{
    ObjectItemModel model;

    QModelIndex rootIndex = model.getRootIndex();
    EXPECT_TRUE(rootIndex.isValid());

    // The root index should point to the "Scene" item
    QString rootText = model.data(rootIndex, Qt::DisplayRole).toString();
    EXPECT_EQ(rootText, QString("Scene"));
}

// Test: getRootIndex row and column are valid
TEST_F(ObjectItemModelTest, GetRootIndexHasValidRowAndColumn)
{
    ObjectItemModel model;

    QModelIndex rootIndex = model.getRootIndex();
    EXPECT_TRUE(rootIndex.isValid());
    EXPECT_EQ(rootIndex.row(), 0);
    EXPECT_EQ(rootIndex.column(), 0);
}

// Test: setHeaderText changes header
TEST_F(ObjectItemModelTest, SetHeaderTextChangesHeader)
{
    ObjectItemModel model;

    // Initially "No Selection"
    QVariant headerBefore = model.headerData(0, Qt::Horizontal, Qt::DisplayRole);
    EXPECT_EQ(headerBefore.toString(), QString("No Selection"));

    // Change header text
    model.setHeaderText("Custom Header");

    QVariant headerAfter = model.headerData(0, Qt::Horizontal, Qt::DisplayRole);
    EXPECT_EQ(headerAfter.toString(), QString("Custom Header"));
}

// Test: setHeaderText with empty string
TEST_F(ObjectItemModelTest, SetHeaderTextWithEmptyString)
{
    ObjectItemModel model;

    model.setHeaderText("");

    QVariant headerData = model.headerData(0, Qt::Horizontal, Qt::DisplayRole);
    EXPECT_EQ(headerData.toString(), QString(""));
}

// Test: Model stores node data in user roles
TEST_F(ObjectItemModelTest, ModelStoresNodeDataInUserRoles)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    PrimitiveObject::createCube("DataTestCube");

    ObjectItemModel model;

    QModelIndex rootIndex = model.getRootIndex();
    int childCount = model.rowCount(rootIndex);
    EXPECT_GE(childCount, 1);

    // Find the cube node and verify it stores node data
    for (int i = 0; i < childCount; ++i)
    {
        QModelIndex childIndex = model.index(i, 0, rootIndex);
        QString childText = model.data(childIndex, Qt::DisplayRole).toString();
        if (childText.contains("DataTestCube"))
        {
            // NODE_DATA is Qt::UserRole+1, the data should be a non-null pointer
            QVariant nodeData = model.data(childIndex, Qt::UserRole + 1);
            EXPECT_TRUE(nodeData.isValid());
            void* ptr = nodeData.value<void*>();
            EXPECT_NE(ptr, nullptr);
            break;
        }
    }

    Manager::getSingleton()->destroySceneNode("DataTestCube");
}

// Test: Entities are added as children of their scene node
TEST_F(ObjectItemModelTest, EntitiesAreChildrenOfSceneNode)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    PrimitiveObject::createCube("EntityTestCube");

    ObjectItemModel model;

    QModelIndex rootIndex = model.getRootIndex();
    int childCount = model.rowCount(rootIndex);
    EXPECT_GE(childCount, 1);

    // Find the cube node and check it has entity children
    for (int i = 0; i < childCount; ++i)
    {
        QModelIndex childIndex = model.index(i, 0, rootIndex);
        QString childText = model.data(childIndex, Qt::DisplayRole).toString();
        if (childText.contains("EntityTestCube"))
        {
            // The node should have at least one entity child (the mesh)
            int entityCount = model.rowCount(childIndex);
            EXPECT_GE(entityCount, 1);

            // The entity child should contain "(Mesh)" in its text
            bool foundMesh = false;
            for (int j = 0; j < entityCount; ++j)
            {
                QModelIndex entityIndex = model.index(j, 0, childIndex);
                QString entityText = model.data(entityIndex, Qt::DisplayRole).toString();
                if (entityText.contains("(Mesh)"))
                {
                    foundMesh = true;
                    break;
                }
            }
            EXPECT_TRUE(foundMesh);
            break;
        }
    }

    Manager::getSingleton()->destroySceneNode("EntityTestCube");
}

// Test: Multiple nodes can be tracked in the model
TEST_F(ObjectItemModelTest, MultipleNodesTracked)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    PrimitiveObject::createCube("MultiCube1");
    PrimitiveObject::createSphere("MultiSphere1");

    ObjectItemModel model;

    QModelIndex rootIndex = model.getRootIndex();
    int childCount = model.rowCount(rootIndex);
    EXPECT_GE(childCount, 2);

    bool foundCube = false;
    bool foundSphere = false;
    for (int i = 0; i < childCount; ++i)
    {
        QModelIndex childIndex = model.index(i, 0, rootIndex);
        QString childText = model.data(childIndex, Qt::DisplayRole).toString();
        if (childText.contains("MultiCube1"))
            foundCube = true;
        if (childText.contains("MultiSphere1"))
            foundSphere = true;
    }
    EXPECT_TRUE(foundCube);
    EXPECT_TRUE(foundSphere);

    Manager::getSingleton()->destroySceneNode("MultiCube1");
    Manager::getSingleton()->destroySceneNode("MultiSphere1");
}
