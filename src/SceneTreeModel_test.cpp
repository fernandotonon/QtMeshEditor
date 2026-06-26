#include <gtest/gtest.h>
#include "SceneTreeModel.h"
#include "Manager.h"
#include "SelectionSet.h"
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>
#include "TestHelpers.h"

class SceneTreeModelTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    SceneTreeModel* model = nullptr;
    struct EntityFixtureData {
        Ogre::SceneNode* node = nullptr;
        Ogre::Entity* entity = nullptr;
        Ogre::SubEntity* subEntity = nullptr;
    };

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";

        createStandardOgreMaterials();
        model = new SceneTreeModel();
    }

    void TearDown() override {
        delete model;
        model = nullptr;
        if (app) app->processEvents();
    }

    EntityFixtureData createEntityHierarchy(const QString& nodeName,
                                           const QString& entityName,
                                           const QString& meshName)
    {
        EntityFixtureData data;
        auto mesh = createInMemoryTriangleMesh(meshName.toStdString());
        EXPECT_NE(mesh, nullptr);

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        data.node = Manager::getSingleton()->addSceneNode(nodeName);
        EXPECT_NE(data.node, nullptr);

        if (!mesh || !data.node)
            return data;

        data.entity = sceneMgr->createEntity(entityName.toStdString(), mesh);
        EXPECT_NE(data.entity, nullptr);

        if (!data.entity)
            return data;

        data.node->attachObject(data.entity);
        if (data.entity->getNumSubEntities() > 0)
            data.subEntity = data.entity->getSubEntity(0);
        return data;
    }
};

TEST_F(SceneTreeModelTests, RoleNames) {
    auto roles = model->roleNames();
    EXPECT_TRUE(roles.contains(SceneTreeModel::NameRole));
    EXPECT_TRUE(roles.contains(SceneTreeModel::TypeRole));
    EXPECT_TRUE(roles.contains(SceneTreeModel::TypeLabelRole));
    EXPECT_TRUE(roles.contains(SceneTreeModel::SelectedRole));
    EXPECT_TRUE(roles.contains(SceneTreeModel::MaterialNameRole));
}

TEST_F(SceneTreeModelTests, InvalidIndex) {
    QModelIndex invalid;
    EXPECT_EQ(model->data(invalid), QVariant());
    EXPECT_EQ(model->parent(invalid), QModelIndex());
}

TEST_F(SceneTreeModelTests, AvailableMaterials) {
    // availableMaterials() should return a list (possibly empty in test env)
    QStringList mats = model->availableMaterials();
    // Just verify it returns without crash; the list may be empty
    // if no materials besides internal ones are loaded
    EXPECT_GE(mats.size(), 0);
}

TEST_F(SceneTreeModelTests, SelectItemInvalidIndex) {
    // selectItem with an out-of-range row should not crash
    QModelIndex root = model->rootIndex();
    EXPECT_NO_THROW(model->selectItem(-1, root, false));
    EXPECT_NO_THROW(model->selectItem(99999, root, false));
}

TEST_F(SceneTreeModelTests, SetMaterialInvalidIndex) {
    // setMaterial with an out-of-range row should not crash
    QModelIndex root = model->rootIndex();
    EXPECT_NO_THROW(model->setMaterial(-1, root, "BaseWhite"));
    EXPECT_NO_THROW(model->setMaterial(99999, root, "BaseWhite"));
}

TEST_F(SceneTreeModelTests, MaterialNameInvalidIndex) {
    // materialName with an out-of-range row should return empty
    QModelIndex root = model->rootIndex();
    QString name = model->materialName(-1, root);
    EXPECT_TRUE(name.isEmpty());

    name = model->materialName(99999, root);
    EXPECT_TRUE(name.isEmpty());
}

TEST_F(SceneTreeModelTests, IsSelectedInvalidIndex) {
    QModelIndex root = model->rootIndex();
    // isSelected with invalid row should return false
    EXPECT_FALSE(model->isSelected(-1, root));
    EXPECT_FALSE(model->isSelected(99999, root));
}

TEST_F(SceneTreeModelTests, RootIndexIsInvalid) {
    // rootIndex() returns an invalid QModelIndex (root of the tree)
    QModelIndex root = model->rootIndex();
    EXPECT_FALSE(root.isValid());
}

TEST_F(SceneTreeModelTests, UpdateSelection) {
    QSignalSpy spy(model, &SceneTreeModel::selectionUpdated);
    model->updateSelection();
    EXPECT_EQ(spy.count(), 1);
}

TEST(SceneTreeItemTests, ChildParentRowAndTypeLabelsWork)
{
    SceneTreeItem root("Scene", SceneTreeItem::Root, nullptr);
    auto* node = new SceneTreeItem("NodeA", SceneTreeItem::Node, nullptr, &root);
    auto* entity = new SceneTreeItem("EntityA", SceneTreeItem::Entity, nullptr, node);
    auto* subEntity = new SceneTreeItem("0", SceneTreeItem::SubEntity, nullptr, entity);

    root.appendChild(node);
    node->appendChild(entity);
    entity->appendChild(subEntity);

    EXPECT_EQ(root.childCount(), 1);
    EXPECT_EQ(root.child(0), node);
    EXPECT_EQ(node->parentItem(), &root);
    EXPECT_EQ(node->row(), 0);
    EXPECT_EQ(entity->parentItem(), node);
    EXPECT_EQ(entity->row(), 0);
    EXPECT_EQ(subEntity->parentItem(), entity);
    EXPECT_EQ(subEntity->row(), 0);

    EXPECT_EQ(root.typeLabel(), QString("Scene"));
    EXPECT_EQ(node->typeLabel(), QString("Node"));
    EXPECT_EQ(entity->typeLabel(), QString("Mesh"));
    EXPECT_EQ(subEntity->typeLabel(), QString("Submesh"));
}

TEST_F(SceneTreeModelTests, RebuildCreatesNodeEntityAndSubEntityHierarchy)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    const auto data = createEntityHierarchy("HierarchyNode", "HierarchyEntity", "HierarchyMesh");
    ASSERT_NE(data.node, nullptr);
    ASSERT_NE(data.entity, nullptr);
    ASSERT_NE(data.subEntity, nullptr);
    data.subEntity->setMaterialName("BaseWhiteNoLighting");

    model->rebuild();

    QModelIndex root = model->rootIndex();
    ASSERT_GE(model->rowCount(root), 1);

    QModelIndex nodeIndex;
    for (int row = 0; row < model->rowCount(root); ++row) {
        QModelIndex idx = model->index(row, 0, root);
        if (model->data(idx, SceneTreeModel::NameRole).toString() == "HierarchyNode") {
            nodeIndex = idx;
            break;
        }
    }

    ASSERT_TRUE(nodeIndex.isValid());
    EXPECT_EQ(model->data(nodeIndex, SceneTreeModel::TypeRole).toInt(), SceneTreeItem::Node);
    EXPECT_EQ(model->data(nodeIndex, SceneTreeModel::TypeLabelRole).toString(), QString("Node"));
    EXPECT_EQ(model->parent(nodeIndex), QModelIndex());

    ASSERT_EQ(model->rowCount(nodeIndex), 1);
    QModelIndex entityIndex = model->index(0, 0, nodeIndex);
    ASSERT_TRUE(entityIndex.isValid());
    EXPECT_EQ(model->data(entityIndex, SceneTreeModel::NameRole).toString(), QString("HierarchyEntity"));
    EXPECT_EQ(model->data(entityIndex, SceneTreeModel::TypeRole).toInt(), SceneTreeItem::Entity);
    EXPECT_EQ(model->data(entityIndex, SceneTreeModel::TypeLabelRole).toString(), QString("Mesh"));
    EXPECT_EQ(model->parent(entityIndex), nodeIndex);
    EXPECT_EQ(model->data(entityIndex, SceneTreeModel::MaterialNameRole).toString(), QString("BaseWhiteNoLighting"));

    ASSERT_EQ(model->rowCount(entityIndex), 1);
    QModelIndex subEntityIndex = model->index(0, 0, entityIndex);
    ASSERT_TRUE(subEntityIndex.isValid());
    EXPECT_EQ(model->data(subEntityIndex, SceneTreeModel::NameRole).toString(), QString("0"));
    EXPECT_EQ(model->data(subEntityIndex, SceneTreeModel::TypeRole).toInt(), SceneTreeItem::SubEntity);
    EXPECT_EQ(model->data(subEntityIndex, SceneTreeModel::TypeLabelRole).toString(), QString("Submesh"));
    EXPECT_EQ(model->parent(subEntityIndex), entityIndex);
    EXPECT_EQ(model->data(subEntityIndex, SceneTreeModel::MaterialNameRole).toString(), QString("BaseWhiteNoLighting"));
}

TEST_F(SceneTreeModelTests, SelectItemSupportsNodeEntityAndSubEntitySelection)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    const auto data = createEntityHierarchy("SelectHierarchyNode", "SelectHierarchyEntity", "SelectHierarchyMesh");
    ASSERT_NE(data.node, nullptr);
    ASSERT_NE(data.entity, nullptr);
    ASSERT_NE(data.subEntity, nullptr);

    model->rebuild();

    QModelIndex root = model->rootIndex();
    QModelIndex nodeIndex;
    for (int row = 0; row < model->rowCount(root); ++row) {
        QModelIndex idx = model->index(row, 0, root);
        if (model->data(idx, SceneTreeModel::NameRole).toString() == "SelectHierarchyNode") {
            nodeIndex = idx;
            break;
        }
    }

    ASSERT_TRUE(nodeIndex.isValid());
    QModelIndex entityIndex = model->index(0, 0, nodeIndex);
    QModelIndex subEntityIndex = model->index(0, 0, entityIndex);
    ASSERT_TRUE(entityIndex.isValid());
    ASSERT_TRUE(subEntityIndex.isValid());

    model->selectItem(nodeIndex.row(), root, false);
    EXPECT_TRUE(SelectionSet::getSingleton()->contains(data.node));
    EXPECT_TRUE(model->isSelected(nodeIndex.row(), root));

    model->selectItem(entityIndex.row(), nodeIndex, true);
    EXPECT_TRUE(SelectionSet::getSingleton()->contains(data.entity));
    EXPECT_TRUE(model->isSelected(entityIndex.row(), nodeIndex));

    model->selectItem(subEntityIndex.row(), entityIndex, true);
    EXPECT_TRUE(SelectionSet::getSingleton()->contains(data.subEntity));
    EXPECT_TRUE(model->isSelected(subEntityIndex.row(), entityIndex));

    model->selectItem(entityIndex.row(), nodeIndex, true);
    EXPECT_FALSE(SelectionSet::getSingleton()->contains(data.entity));
}

TEST_F(SceneTreeModelTests, SetMaterialUpdatesEntityAndSubEntityAndEmitsDataChanged)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    const auto data = createEntityHierarchy("MaterialNode", "MaterialEntity", "MaterialMesh");
    ASSERT_NE(data.node, nullptr);
    ASSERT_NE(data.entity, nullptr);
    ASSERT_NE(data.subEntity, nullptr);

    model->rebuild();

    QModelIndex root = model->rootIndex();
    QModelIndex nodeIndex;
    for (int row = 0; row < model->rowCount(root); ++row) {
        QModelIndex idx = model->index(row, 0, root);
        if (model->data(idx, SceneTreeModel::NameRole).toString() == "MaterialNode") {
            nodeIndex = idx;
            break;
        }
    }

    ASSERT_TRUE(nodeIndex.isValid());
    QModelIndex entityIndex = model->index(0, 0, nodeIndex);
    QModelIndex subEntityIndex = model->index(0, 0, entityIndex);
    ASSERT_TRUE(entityIndex.isValid());
    ASSERT_TRUE(subEntityIndex.isValid());

    QSignalSpy changedSpy(model, &QAbstractItemModel::dataChanged);
    ASSERT_TRUE(changedSpy.isValid());

    model->setMaterial(0, nodeIndex, "BaseWhiteNoLighting");
    EXPECT_EQ(QString::fromStdString(data.entity->getSubEntity(0)->getMaterialName()), QString("BaseWhiteNoLighting"));
    EXPECT_EQ(model->materialName(0, nodeIndex), QString("BaseWhiteNoLighting"));

    model->setMaterial(0, entityIndex, "BaseWhite");
    EXPECT_EQ(QString::fromStdString(data.subEntity->getMaterialName()), QString("BaseWhite"));
    EXPECT_EQ(model->materialName(0, entityIndex), QString("BaseWhite"));

    EXPECT_EQ(changedSpy.count(), 2);
}

TEST_F(SceneTreeModelTests, AvailableMaterialsFiltersInternalNamesAndSortsResults)
{
    auto& materialManager = Ogre::MaterialManager::getSingleton();
    if (!materialManager.resourceExists("ZZ_CustomMat"))
        materialManager.create("ZZ_CustomMat", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!materialManager.resourceExists("aa_CustomMat"))
        materialManager.create("aa_CustomMat", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!materialManager.resourceExists("Ogre/InternalShouldSkip"))
        materialManager.create("Ogre/InternalShouldSkip", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    const QStringList materials = model->availableMaterials();

    EXPECT_TRUE(materials.contains("aa_CustomMat"));
    EXPECT_TRUE(materials.contains("ZZ_CustomMat"));
    EXPECT_FALSE(materials.contains("Ogre/InternalShouldSkip"));
    EXPECT_FALSE(materials.contains("GUI_Material"));
    EXPECT_FALSE(materials.contains("BaseWhite"));
    EXPECT_FALSE(materials.contains("BaseWhiteNoLighting"));

    const int lowerIndex = materials.indexOf("aa_CustomMat");
    const int upperIndex = materials.indexOf("ZZ_CustomMat");
    ASSERT_NE(lowerIndex, -1);
    ASSERT_NE(upperIndex, -1);
    EXPECT_LT(lowerIndex, upperIndex);
}

TEST_F(SceneTreeModelTests, RebuildSkipsForbiddenNodeNames)
{
    Manager::getSingleton()->addSceneNode("TPCameraChildSceneNode");
    Manager::getSingleton()->addSceneNode("Unnamed_TestNode");
    Ogre::SceneNode* visibleNode = Manager::getSingleton()->addSceneNode("VisibleTreeNode");
    ASSERT_NE(visibleNode, nullptr);

    model->rebuild();

    QModelIndex root = model->rootIndex();
    QStringList names;
    for (int row = 0; row < model->rowCount(root); ++row)
        names.append(model->data(model->index(row, 0, root), SceneTreeModel::NameRole).toString());

    EXPECT_TRUE(names.contains("VisibleTreeNode"));
    EXPECT_FALSE(names.contains("TPCameraChildSceneNode"));
    EXPECT_FALSE(names.contains("Unnamed_TestNode"));
}
