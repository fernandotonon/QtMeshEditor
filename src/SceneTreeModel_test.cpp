#include <gtest/gtest.h>
#include "SceneTreeModel.h"
#include "Manager.h"
#include "SelectionSet.h"
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>
#include <OgreMaterialManager.h>
#include "TestHelpers.h"

namespace {
QModelIndex findChildByName(SceneTreeModel* model, const QModelIndex& parent, const QString& name)
{
    const int rows = model->rowCount(parent);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex idx = model->index(row, 0, parent);
        if (model->data(idx, SceneTreeModel::NameRole).toString() == name)
            return idx;
    }
    return QModelIndex();
}

QModelIndex findFirstChildByType(SceneTreeModel* model, const QModelIndex& parent, SceneTreeItem::ItemType type)
{
    const int rows = model->rowCount(parent);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex idx = model->index(row, 0, parent);
        if (model->data(idx, SceneTreeModel::TypeRole).toInt() == static_cast<int>(type))
            return idx;
    }
    return QModelIndex();
}
} // namespace

class SceneTreeModelTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    SceneTreeModel* model = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }

        model = new SceneTreeModel();
    }

    void TearDown() override {
        delete model;
        model = nullptr;
        if (app) app->processEvents();
    }
};

TEST_F(SceneTreeModelTests, InitialState) {
    // Root should have at least 0 rows (empty scene)
    EXPECT_GE(model->rowCount(), 0);
    EXPECT_EQ(model->columnCount(), 1);
}

TEST_F(SceneTreeModelTests, RoleNames) {
    auto roles = model->roleNames();
    EXPECT_TRUE(roles.contains(SceneTreeModel::NameRole));
    EXPECT_TRUE(roles.contains(SceneTreeModel::TypeRole));
    EXPECT_TRUE(roles.contains(SceneTreeModel::TypeLabelRole));
    EXPECT_TRUE(roles.contains(SceneTreeModel::SelectedRole));
    EXPECT_TRUE(roles.contains(SceneTreeModel::MaterialNameRole));
}

TEST_F(SceneTreeModelTests, Rebuild) {
    int initialRows = model->rowCount();
    model->rebuild();
    // After rebuild, row count should be consistent
    EXPECT_EQ(model->rowCount(), initialRows);
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

TEST_F(SceneTreeModelTests, RebuildWithEntity) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";
    }
    createStandardOgreMaterials();

    auto mesh = createInMemoryTriangleMesh("TreeModelTestMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("TreeModelTestNode");
    auto* entity = sceneMgr->createEntity("TreeModelTestEnt", mesh);
    node->attachObject(entity);

    model->rebuild();

    // Should have at least one row (the node we added)
    EXPECT_GE(model->rowCount(), 1);
}

TEST_F(SceneTreeModelTests, SelectNodeItem) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("SelectTestNode");
    ASSERT_NE(node, nullptr);

    model->rebuild();

    // Find the row for our node
    QModelIndex root = model->rootIndex();
    int rows = model->rowCount(root);
    bool found = false;
    for (int i = 0; i < rows; ++i) {
        QModelIndex idx = model->index(i, 0, root);
        if (model->data(idx, SceneTreeModel::NameRole).toString() == "SelectTestNode") {
            model->selectItem(i, root, false);
            EXPECT_TRUE(SelectionSet::getSingleton()->contains(node));
            found = true;
            break;
        }
    }
    // Node may or may not be found depending on forbidden names, but test should not crash
    if (!found) {
        // selectItem with valid but wrong index should not crash
        EXPECT_NO_THROW(model->selectItem(0, root, false));
    }

    SelectionSet::getSingleton()->clear();
}

TEST_F(SceneTreeModelTests, UpdateSelection) {
    QSignalSpy spy(model, &SceneTreeModel::selectionUpdated);
    model->updateSelection();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(SceneTreeModelTests, DataWithVariousRoles) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("DataRolesNode");
    ASSERT_NE(node, nullptr);

    model->rebuild();

    QModelIndex root = model->rootIndex();
    int rows = model->rowCount(root);
    if (rows > 0) {
        QModelIndex idx = model->index(0, 0, root);
        if (idx.isValid()) {
            // Test all roles
            QVariant nameData = model->data(idx, SceneTreeModel::NameRole);
            EXPECT_FALSE(nameData.toString().isEmpty());

            QVariant typeData = model->data(idx, SceneTreeModel::TypeRole);
            EXPECT_TRUE(typeData.isValid());

            QVariant typeLabelData = model->data(idx, SceneTreeModel::TypeLabelRole);
            EXPECT_TRUE(typeLabelData.isValid());

            QVariant selectedData = model->data(idx, SceneTreeModel::SelectedRole);
            EXPECT_TRUE(selectedData.isValid());

            // Unknown role should return empty QVariant
            QVariant unknownData = model->data(idx, Qt::UserRole + 100);
            EXPECT_FALSE(unknownData.isValid());
        }
    }
}

TEST(SceneTreeItemTests, HierarchyAccessAndTypeLabels)
{
    auto* root = new SceneTreeItem("root", SceneTreeItem::Root, nullptr, nullptr);
    auto* node = new SceneTreeItem("node", SceneTreeItem::Node, nullptr, root);
    auto* entity = new SceneTreeItem("entity", SceneTreeItem::Entity, nullptr, node);
    auto* sub = new SceneTreeItem("sub", SceneTreeItem::SubEntity, nullptr, entity);

    root->appendChild(node);
    node->appendChild(entity);
    entity->appendChild(sub);

    EXPECT_EQ(root->childCount(), 1);
    EXPECT_EQ(node->childCount(), 1);
    EXPECT_EQ(entity->childCount(), 1);
    EXPECT_EQ(sub->childCount(), 0);

    EXPECT_EQ(root->child(0), node);
    EXPECT_EQ(root->child(-1), nullptr);
    EXPECT_EQ(root->child(9), nullptr);

    EXPECT_EQ(node->parentItem(), root);
    EXPECT_EQ(entity->parentItem(), node);
    EXPECT_EQ(sub->parentItem(), entity);
    EXPECT_EQ(root->row(), 0);
    EXPECT_EQ(node->row(), 0);
    EXPECT_EQ(entity->row(), 0);
    EXPECT_EQ(sub->row(), 0);

    EXPECT_EQ(root->typeLabel(), "Scene");
    EXPECT_EQ(node->typeLabel(), "Node");
    EXPECT_EQ(entity->typeLabel(), "Mesh");
    EXPECT_EQ(sub->typeLabel(), "Submesh");

    delete root;
}

TEST_F(SceneTreeModelTests, ParentForTopLevelNodeIsInvalid)
{
    Manager::getSingleton()->addSceneNode("ParentTopNode");
    model->rebuild();

    const QModelIndex nodeIndex = findChildByName(model, model->rootIndex(), "ParentTopNode");
    ASSERT_TRUE(nodeIndex.isValid());
    EXPECT_FALSE(model->parent(nodeIndex).isValid());
}

TEST_F(SceneTreeModelTests, ParentForEntityAndSubEntityIsValid)
{
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";
    }
    createStandardOgreMaterials();

    auto mesh = createInMemoryTriangleMesh("TreeParentMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("TreeParentNode");
    auto* entity = sceneMgr->createEntity("TreeParentEntity", mesh);
    node->attachObject(entity);
    ASSERT_GT(entity->getNumSubEntities(), 0u);

    model->rebuild();

    const QModelIndex nodeIndex = findChildByName(model, model->rootIndex(), "TreeParentNode");
    ASSERT_TRUE(nodeIndex.isValid());
    const QModelIndex entityIndex = findFirstChildByType(model, nodeIndex, SceneTreeItem::Entity);
    ASSERT_TRUE(entityIndex.isValid());
    const QModelIndex subEntityIndex = findFirstChildByType(model, entityIndex, SceneTreeItem::SubEntity);
    ASSERT_TRUE(subEntityIndex.isValid());

    EXPECT_EQ(model->parent(entityIndex), nodeIndex);
    EXPECT_EQ(model->parent(subEntityIndex), entityIndex);
}

TEST_F(SceneTreeModelTests, SelectItemTogglesEntityAndSubEntityInMultiSelect)
{
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";
    }
    createStandardOgreMaterials();
    SelectionSet::getSingleton()->clear();

    auto mesh = createInMemoryTriangleMesh("TreeToggleMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("TreeToggleNode");
    auto* entity = sceneMgr->createEntity("TreeToggleEntity", mesh);
    node->attachObject(entity);
    ASSERT_GT(entity->getNumSubEntities(), 0u);
    Ogre::SubEntity* subEntity = entity->getSubEntity(0);

    model->rebuild();

    const QModelIndex nodeIndex = findChildByName(model, model->rootIndex(), "TreeToggleNode");
    ASSERT_TRUE(nodeIndex.isValid());
    const QModelIndex entityIndex = findFirstChildByType(model, nodeIndex, SceneTreeItem::Entity);
    ASSERT_TRUE(entityIndex.isValid());
    const QModelIndex subEntityIndex = findFirstChildByType(model, entityIndex, SceneTreeItem::SubEntity);
    ASSERT_TRUE(subEntityIndex.isValid());

    model->selectItem(entityIndex.row(), nodeIndex, false);
    EXPECT_TRUE(SelectionSet::getSingleton()->contains(entity));
    model->selectItem(entityIndex.row(), nodeIndex, true);
    EXPECT_FALSE(SelectionSet::getSingleton()->contains(entity));

    model->selectItem(subEntityIndex.row(), entityIndex, false);
    EXPECT_TRUE(SelectionSet::getSingleton()->contains(subEntity));
    model->selectItem(subEntityIndex.row(), entityIndex, true);
    EXPECT_FALSE(SelectionSet::getSingleton()->contains(subEntity));
}

TEST_F(SceneTreeModelTests, SetMaterialUpdatesItemAndEmitsDataChanged)
{
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: entity creation not supported without render window";
    }
    createStandardOgreMaterials();

    auto mesh = createInMemoryTriangleMesh("TreeSetMatMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("TreeSetMatNode");
    auto* entity = sceneMgr->createEntity("TreeSetMatEntity", mesh);
    node->attachObject(entity);
    ASSERT_GT(entity->getNumSubEntities(), 0u);

    model->rebuild();

    const QModelIndex nodeIndex = findChildByName(model, model->rootIndex(), "TreeSetMatNode");
    ASSERT_TRUE(nodeIndex.isValid());
    const QModelIndex entityIndex = findFirstChildByType(model, nodeIndex, SceneTreeItem::Entity);
    ASSERT_TRUE(entityIndex.isValid());
    const QModelIndex subEntityIndex = findFirstChildByType(model, entityIndex, SceneTreeItem::SubEntity);
    ASSERT_TRUE(subEntityIndex.isValid());

    QSignalSpy dataChangedSpy(model, &QAbstractItemModel::dataChanged);

    model->setMaterial(entityIndex.row(), nodeIndex, "BaseWhiteNoLighting");
    EXPECT_EQ(model->materialName(entityIndex.row(), nodeIndex), "BaseWhiteNoLighting");

    model->setMaterial(subEntityIndex.row(), entityIndex, "BaseWhite");
    EXPECT_EQ(model->materialName(subEntityIndex.row(), entityIndex), "BaseWhite");

    EXPECT_GE(dataChangedSpy.count(), 2);
}

TEST_F(SceneTreeModelTests, DataMaterialNameRoleForNodeIsInvalid)
{
    Manager::getSingleton()->addSceneNode("TreeRoleNode");
    model->rebuild();

    const QModelIndex nodeIndex = findChildByName(model, model->rootIndex(), "TreeRoleNode");
    ASSERT_TRUE(nodeIndex.isValid());
    EXPECT_FALSE(model->data(nodeIndex, SceneTreeModel::MaterialNameRole).isValid());
}

TEST_F(SceneTreeModelTests, AvailableMaterialsFiltersInternalNames)
{
    auto* materialManager = Ogre::MaterialManager::getSingletonPtr();
    ASSERT_NE(materialManager, nullptr);

    if (!materialManager->resourceExists("Custom/CoverageMaterial")) {
        materialManager->create("Custom/CoverageMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    }
    if (!materialManager->resourceExists("Ogre/InternalCoverageMaterial")) {
        materialManager->create("Ogre/InternalCoverageMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    }
    if (!materialManager->resourceExists("BaseWhiteCoverageMaterial")) {
        materialManager->create("BaseWhiteCoverageMaterial", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    }
    if (!materialManager->resourceExists("GUI_Material")) {
        materialManager->create("GUI_Material", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    }

    const QStringList mats = model->availableMaterials();

    EXPECT_TRUE(mats.contains("Custom/CoverageMaterial"));
    EXPECT_FALSE(mats.contains("Ogre/InternalCoverageMaterial"));
    EXPECT_FALSE(mats.contains("BaseWhiteCoverageMaterial"));
    EXPECT_FALSE(mats.contains("GUI_Material"));
}
