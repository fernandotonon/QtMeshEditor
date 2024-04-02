#include <gtest/gtest.h>
#include "Manager.h"
#include <QMap>
#include "SelectionSet.h"

TEST(SelectionSetTests, AppendSceneNode)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->append(sceneNode);

    EXPECT_EQ(selectionSet->getNodesCount(), 1);
    EXPECT_TRUE(selectionSet->contains(sceneNode));

    auto sceneNode2 = Manager::getSingleton()->addSceneNode("test2");
    selectionSet->append(sceneNode);
    selectionSet->append(sceneNode2);

    EXPECT_EQ(selectionSet->getNodesCount(), 2);
    EXPECT_TRUE(selectionSet->contains(sceneNode));
    EXPECT_TRUE(selectionSet->contains(sceneNode2));

    Manager::getSingleton()->destroySceneNode(sceneNode);
    Manager::getSingleton()->destroySceneNode(sceneNode2);
}

TEST(SelectionSetTests, RemoveSceneNode)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->append(sceneNode);
    bool removed = selectionSet->removeOne(sceneNode);

    EXPECT_TRUE(removed);
    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_FALSE(selectionSet->contains(sceneNode));

    Manager::getSingleton()->destroySceneNode(sceneNode);
    SelectionSet::kill();
}

TEST(SelectionSetTests, SelectSceneNode)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->selectOne(sceneNode);

    EXPECT_EQ(selectionSet->getNodesCount(), 1);
    EXPECT_TRUE(selectionSet->contains(sceneNode));

    Manager::getSingleton()->destroySceneNode(sceneNode);
}

TEST(SelectionSetTests, Clear)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->append(sceneNode);
    selectionSet->clear();

    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_FALSE(selectionSet->contains(sceneNode));

    Manager::getSingleton()->destroySceneNode(sceneNode);
}

TEST(SelectionSetTests, ClearList)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->append(sceneNode);
    selectionSet->clearList();

    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_FALSE(selectionSet->contains(sceneNode));
    Manager::getSingleton()->destroySceneNode(sceneNode);
}

TEST(SelectionSetTests, GetSelectionNodesCenterEmpty)
{
    SelectionSet::getSingleton()->clear();
    auto center = SelectionSet::getSingleton()->getSelectionNodesCenter();

    EXPECT_EQ(center.x, 0.0f);
    EXPECT_EQ(center.y, 0.0f);
    EXPECT_EQ(center.z, 0.0f);
}

TEST(SelectionSetTests, GetSelectionNodesCenter)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");
    auto sceneNode2 = Manager::getSingleton()->addSceneNode("test2");

    selectionSet->selectOne(sceneNode);
    selectionSet->append(sceneNode2);

    sceneNode2->translate(1.0,2.0,3.0);

    auto center = selectionSet->getSelectionNodesCenter();

    EXPECT_EQ(center.x, 0.5f);
    EXPECT_EQ(center.y, 1.0f);
    EXPECT_EQ(center.z, 1.5f);

    Manager::getSingleton()->destroySceneNode(sceneNode);
    Manager::getSingleton()->destroySceneNode(sceneNode2);
}
