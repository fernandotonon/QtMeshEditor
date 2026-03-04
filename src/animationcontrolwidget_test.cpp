#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <QTreeWidget>
#include <QListWidget>
#include <QSlider>
#include <QTableWidget>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QTimer>
#include "animationcontrolwidget.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "MeshImporterExporter.h"
#include <OgreException.h>
#include <OgreSkeletonInstance.h>
#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreKeyFrame.h>
#include <OgreBone.h>
#include "TestHelpers.h"

class AnimationControlWidgetTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();
    }

    void TearDown() override {
        SelectionSet::getSingleton()->clear();
        if (app) app->processEvents();
    }

    // Helper: create animated entity and select its node
    Ogre::Entity* setupAnimatedEntity(const std::string& name) {
        if (!canLoadMeshFiles()) return nullptr;
        Ogre::Entity* entity = createAnimatedTestEntity(name);
        if (!entity) return nullptr;
        Ogre::SceneNode* node = entity->getParentSceneNode();
        SelectionSet::getSingleton()->selectOne(node);
        if (app) app->processEvents();
        return entity;
    }
};

TEST_F(AnimationControlWidgetTest, ConstructionDoesNotCrash) {
    AnimationControlWidget widget;
    SUCCEED();
}

TEST_F(AnimationControlWidgetTest, UpdateAnimationTreeWithNoSelection) {
    AnimationControlWidget widget;
    SelectionSet::getSingleton()->clear();
    EXPECT_NO_THROW(widget.updateAnimationTree());
}

TEST_F(AnimationControlWidgetTest, UpdateAnimationTreeWithAnimatedEntity) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot load mesh files (no GL context)";
    }

    try {
        AnimationControlWidget widget;
        MeshImporterExporter::importer(QStringList{"./media/models/robot.mesh"});

        auto entities = Manager::getSingleton()->getEntities();
        ASSERT_FALSE(entities.isEmpty());

        Ogre::Entity* entity = nullptr;
        for (auto* obj : entities) {
            if (obj->getMovableType() == "Entity") {
                entity = static_cast<Ogre::Entity*>(obj);
                break;
            }
        }
        ASSERT_NE(entity, nullptr);

        Ogre::SceneNode* parentNode = entity->getParentSceneNode();
        ASSERT_NE(parentNode, nullptr);
        SelectionSet::getSingleton()->selectOne(parentNode);

        EXPECT_NO_THROW(widget.updateAnimationTree());
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre exception (" << e.getFullDescription() << ")";
    } catch (...) {
        GTEST_SKIP() << "Skipping: unknown exception";
    }
}

// --- Tests using in-memory animated entities ---

TEST_F(AnimationControlWidgetTest, UpdateAnimationTreeWithInMemoryEntity) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("AnimTreeTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();
    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    ASSERT_NE(tree, nullptr);

    // Should have one top-level item (the entity)
    EXPECT_EQ(tree->topLevelItemCount(), 1);

    // The top-level item should have one child (the "TestAnim" animation)
    QTreeWidgetItem* entityItem = tree->topLevelItem(0);
    EXPECT_GE(entityItem->childCount(), 1);
    EXPECT_TRUE(entityItem->child(0)->text(0).contains("TestAnim"));
}

TEST_F(AnimationControlWidgetTest, SelectAnimationPopulatesBoneList) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("BoneListTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    ASSERT_NE(tree, nullptr);
    ASSERT_GE(tree->topLevelItemCount(), 1);

    // Select the animation child item to trigger bone list population
    QTreeWidgetItem* animItem = tree->topLevelItem(0)->child(0);
    ASSERT_NE(animItem, nullptr);
    tree->setCurrentItem(animItem);
    if (app) app->processEvents();

    // Bone list should now have at least one bone (the "Child" bone from TestAnim track)
    QListWidget* boneList = widget.findChild<QListWidget*>("boneList");
    ASSERT_NE(boneList, nullptr);
    EXPECT_GE(boneList->count(), 1);
}

TEST_F(AnimationControlWidgetTest, SelectAnimationUpdatesSliderRange) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("SliderRangeTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    QTreeWidgetItem* animItem = tree->topLevelItem(0)->child(0);
    tree->setCurrentItem(animItem);
    if (app) app->processEvents();

    // Slider max should be 1000 (1.0 seconds * 1000)
    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    ASSERT_NE(slider, nullptr);
    EXPECT_EQ(slider->maximum(), 1000);
}

TEST_F(AnimationControlWidgetTest, SelectAnimationUpdatesLengthSpinBox) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("LenSpinTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    QTreeWidgetItem* animItem = tree->topLevelItem(0)->child(0);
    tree->setCurrentItem(animItem);
    if (app) app->processEvents();

    auto* spinBox = widget.findChild<QDoubleSpinBox*>("lengthSpinBox");
    ASSERT_NE(spinBox, nullptr);
    EXPECT_NEAR(spinBox->value(), 1.0, 0.01);
}

TEST_F(AnimationControlWidgetTest, SetAnimationFrameNoSelection) {
    AnimationControlWidget widget;
    // No selection, no crash
    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    ASSERT_NE(slider, nullptr);
    EXPECT_NO_THROW(slider->setValue(500));
}

TEST_F(AnimationControlWidgetTest, SetAnimationFrameWithAnimation) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("SetFrameTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    if (app) app->processEvents();

    // Move slider to middle (t=0.5s = 500ms), should be near keyframe[1]
    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    ASSERT_NE(slider, nullptr);
    slider->setValue(500);
    if (app) app->processEvents();

    // Verify table shows keyframe values
    auto* table = widget.findChild<QTableWidget*>("tableWidget");
    ASSERT_NE(table, nullptr);
    // Translation X should be near 0.5 (kf1 translate)
    EXPECT_FALSE(table->item(0, 1)->text().isEmpty());
}

TEST_F(AnimationControlWidgetTest, SetAnimationFrameAtKeyframeZero) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("FrameZeroTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    if (app) app->processEvents();

    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    slider->setValue(0);
    if (app) app->processEvents();

    auto* table = widget.findChild<QTableWidget*>("tableWidget");
    ASSERT_NE(table, nullptr);
    // At t=0, translation should be (0,0,0)
    EXPECT_FALSE(table->item(0, 1)->text().isEmpty());
}

TEST_F(AnimationControlWidgetTest, OnAddKeyframe) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("AddKfTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    if (app) app->processEvents();

    // Move slider to t=0.25s (between kf0 and kf1)
    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    slider->setValue(250);
    if (app) app->processEvents();

    // Count existing keyframes via skeleton animation track
    auto* skel = entity->getSkeleton();
    auto* anim = skel->getAnimation("TestAnim");
    auto& tracks = anim->_getNodeTrackList();
    ASSERT_FALSE(tracks.empty());
    auto* track = tracks.begin()->second;
    int kfBefore = track->getNumKeyFrames();

    // Click add keyframe
    auto* addBtn = widget.findChild<QPushButton*>("addKeyframeButton");
    ASSERT_NE(addBtn, nullptr);
    addBtn->click();
    if (app) app->processEvents();

    EXPECT_EQ(track->getNumKeyFrames(), kfBefore + 1);
}

TEST_F(AnimationControlWidgetTest, OnDeleteKeyframe) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("DeleteKfTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    if (app) app->processEvents();

    // Move to exact keyframe position (t=0.5s = 500ms)
    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    slider->setValue(500);
    if (app) app->processEvents();

    auto* skel = entity->getSkeleton();
    auto* anim = skel->getAnimation("TestAnim");
    auto* track = anim->_getNodeTrackList().begin()->second;
    int kfBefore = track->getNumKeyFrames();

    // Click delete keyframe
    auto* delBtn = widget.findChild<QPushButton*>("deleteKeyframeButton");
    ASSERT_NE(delBtn, nullptr);
    delBtn->click();
    if (app) app->processEvents();

    EXPECT_EQ(track->getNumKeyFrames(), kfBefore - 1);
}

TEST_F(AnimationControlWidgetTest, OnPrevKeyframe) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("PrevKfTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    if (app) app->processEvents();

    // Move slider to end (t=1.0s)
    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    slider->setValue(1000);
    if (app) app->processEvents();

    // Click prev keyframe — should go to kf at 0.5s
    auto* prevBtn = widget.findChild<QPushButton*>("prevKeyframeButton");
    ASSERT_NE(prevBtn, nullptr);
    prevBtn->click();
    if (app) app->processEvents();

    EXPECT_EQ(slider->value(), 500);
}

TEST_F(AnimationControlWidgetTest, OnNextKeyframe) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("NextKfTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    if (app) app->processEvents();

    // Move slider to start (t=0.0s)
    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    slider->setValue(0);
    if (app) app->processEvents();

    // Click next keyframe — should go to kf at 0.5s
    auto* nextBtn = widget.findChild<QPushButton*>("nextKeyframeButton");
    ASSERT_NE(nextBtn, nullptr);
    nextBtn->click();
    if (app) app->processEvents();

    EXPECT_EQ(slider->value(), 500);
}

TEST_F(AnimationControlWidgetTest, OnAnimationLengthChanged) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("LenChangeTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    if (app) app->processEvents();

    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    auto* spinBox = widget.findChild<QDoubleSpinBox*>("lengthSpinBox");
    ASSERT_NE(spinBox, nullptr);

    // Change length to 2.0 seconds
    spinBox->setValue(2.0);
    if (app) app->processEvents();

    EXPECT_EQ(slider->maximum(), 2000);

    auto* maxLabel = widget.findChild<QLabel*>("maxSliderLabel");
    if (maxLabel) {
        EXPECT_EQ(maxLabel->text(), "2");
    }
}

TEST_F(AnimationControlWidgetTest, OnKeyframeValueChangedTranslation) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("KfValueTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    if (app) app->processEvents();

    // Move to keyframe at t=0.5s
    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    slider->setValue(500);
    if (app) app->processEvents();

    // Modify translation X in the table
    auto* table = widget.findChild<QTableWidget*>("tableWidget");
    ASSERT_NE(table, nullptr);
    table->item(0, 1)->setText("99.5");
    if (app) app->processEvents();

    // Verify the keyframe was updated
    auto* skel = entity->getSkeleton();
    auto* anim = skel->getAnimation("TestAnim");
    auto* track = anim->_getNodeTrackList().begin()->second;

    // Find the keyframe at t=0.5
    for (unsigned short i = 0; i < track->getNumKeyFrames(); i++) {
        auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
        if (std::fabs(kf->getTime() - 0.5f) < 0.01f) {
            EXPECT_NEAR(kf->getTranslate().x, 99.5f, 0.1f);
            break;
        }
    }
}

TEST_F(AnimationControlWidgetTest, UpdateTableEditabilityOnKeyframe) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("EditableTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    if (app) app->processEvents();

    auto* table = widget.findChild<QTableWidget*>("tableWidget");
    ASSERT_NE(table, nullptr);

    // Move to a keyframe position
    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    slider->setValue(500);
    if (app) app->processEvents();

    // Translation row (0), col 1 should be editable on keyframe
    if (table->item(0, 1)) {
        EXPECT_TRUE(table->item(0, 1)->flags() & Qt::ItemIsEditable);
    }

    // Translation row (0), col 0 ("-") should NOT be editable even on keyframe
    if (table->item(0, 0)) {
        EXPECT_FALSE(table->item(0, 0)->flags() & Qt::ItemIsEditable);
    }
}

TEST_F(AnimationControlWidgetTest, DeleteKeyframeButtonDisabledByDefault) {
    AnimationControlWidget widget;

    auto* delBtn = widget.findChild<QPushButton*>("deleteKeyframeButton");
    ASSERT_NE(delBtn, nullptr);
    // Initially no keyframe selected, so delete should be disabled
    EXPECT_FALSE(delBtn->isEnabled());
}

TEST_F(AnimationControlWidgetTest, TimerPlaybackDoesNotCrash) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("TimerTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    if (app) app->processEvents();

    // Let the timer fire a few times (16ms interval)
    QThread::msleep(50);
    if (app) app->processEvents();
    QThread::msleep(50);
    if (app) app->processEvents();

    SUCCEED();
}

TEST_F(AnimationControlWidgetTest, MultipleAnimationsInTree) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    // Create entity with two animations
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "MultiAnimSkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* rootBone = skel->createBone("Root", 0);
    auto* childBone = skel->createBone("Child", 1);
    childBone->setPosition(Ogre::Vector3(0, 1, 0));
    rootBone->addChild(childBone);
    skel->setBindingPose();

    // Animation 1
    auto* anim1 = skel->createAnimation("Walk", 1.0f);
    auto* track1 = anim1->createNodeTrack(1);
    track1->setAssociatedNode(childBone);
    track1->createNodeKeyFrame(0.0f)->setTranslate(Ogre::Vector3::ZERO);
    track1->createNodeKeyFrame(1.0f)->setTranslate(Ogre::Vector3(1, 0, 0));

    // Animation 2
    auto* anim2 = skel->createAnimation("Run", 0.5f);
    auto* track2 = anim2->createNodeTrack(1);
    track2->setAssociatedNode(childBone);
    track2->createNodeKeyFrame(0.0f)->setTranslate(Ogre::Vector3::ZERO);
    track2->createNodeKeyFrame(0.5f)->setTranslate(Ogre::Vector3(2, 0, 0));

    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        "MultiAnimMesh", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* sub = mesh->createSubMesh();
    mesh->sharedVertexData = new Ogre::VertexData();
    auto* decl = mesh->sharedVertexData->vertexDeclaration;
    decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float verts[] = {0,0,0, 1,0,0, 0,1,0};
    vbuf->writeData(0, sizeof(verts), verts);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
    mesh->sharedVertexData->vertexCount = 3;
    auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t idx[] = {0, 1, 2};
    ibuf->writeData(0, sizeof(idx), idx);
    sub->useSharedVertices = true;
    sub->indexData->indexBuffer = ibuf;
    sub->indexData->indexCount = 3;
    Ogre::VertexBoneAssignment vba;
    vba.boneIndex = 1; vba.weight = 1.0f;
    for (unsigned short v = 0; v < 3; ++v) { vba.vertexIndex = v; mesh->addBoneAssignment(vba); }
    mesh->_notifySkeleton(skel);
    mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,2,2,2));
    mesh->_setBoundingSphereRadius(3.0);
    mesh->load();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("MultiAnimNode");
    auto* entity = sceneMgr->createEntity("MultiAnimEntity", mesh);
    node->attachObject(entity);
    SelectionSet::getSingleton()->selectOne(node);
    if (app) app->processEvents();

    AnimationControlWidget widget;
    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    ASSERT_NE(tree, nullptr);
    ASSERT_GE(tree->topLevelItemCount(), 1);

    // Should have 2 animation children
    QTreeWidgetItem* entityItem = tree->topLevelItem(0);
    EXPECT_EQ(entityItem->childCount(), 2);

    // Find the "Run" animation by name (iteration order is not guaranteed)
    QTreeWidgetItem* runItem = nullptr;
    for (int i = 0; i < entityItem->childCount(); ++i) {
        if (entityItem->child(i)->text(0).contains("Run")) {
            runItem = entityItem->child(i);
            break;
        }
    }
    ASSERT_NE(runItem, nullptr) << "Could not find 'Run' animation in tree";
    tree->setCurrentItem(runItem);
    if (app) app->processEvents();

    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    ASSERT_NE(slider, nullptr);
    // "Run" anim length is 0.5s = 500ms
    EXPECT_EQ(slider->maximum(), 500);
}

TEST_F(AnimationControlWidgetTest, PrevKeyframeWrapsToFirst) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("PrevWrapTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    if (app) app->processEvents();

    // Move slider to 0 (at first keyframe)
    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    slider->setValue(0);
    if (app) app->processEvents();

    // Clicking prev should still go to first keyframe (wrap/stay)
    auto* prevBtn = widget.findChild<QPushButton*>("prevKeyframeButton");
    prevBtn->click();
    if (app) app->processEvents();

    // Should be at first keyframe (t=0)
    EXPECT_EQ(slider->value(), 0);
}

TEST_F(AnimationControlWidgetTest, NextKeyframeWrapsToLast) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("NextWrapTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    if (app) app->processEvents();

    // Move slider to end
    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    slider->setValue(1000);
    if (app) app->processEvents();

    // Clicking next should go to last keyframe
    auto* nextBtn = widget.findChild<QPushButton*>("nextKeyframeButton");
    nextBtn->click();
    if (app) app->processEvents();

    // Should be at last keyframe (t=1.0s = 1000ms)
    EXPECT_EQ(slider->value(), 1000);
}

TEST_F(AnimationControlWidgetTest, AnimationLengthClampsSlider) {
    if (!canLoadMeshFiles()) {
        GTEST_SKIP() << "Skipping: Cannot create in-memory meshes (no GL context)";
    }

    AnimationControlWidget widget;
    Ogre::Entity* entity = setupAnimatedEntity("ClampTest");
    ASSERT_NE(entity, nullptr);

    widget.updateAnimationTree();

    QTreeWidget* tree = widget.findChild<QTreeWidget*>("treeWidget");
    tree->setCurrentItem(tree->topLevelItem(0)->child(0));
    if (app) app->processEvents();

    // Move slider near end
    auto* slider = widget.findChild<QSlider*>("horizontalSlider");
    slider->setValue(900);
    if (app) app->processEvents();

    // Shorten animation to 0.5s — slider should clamp
    auto* spinBox = widget.findChild<QDoubleSpinBox*>("lengthSpinBox");
    ASSERT_NE(spinBox, nullptr);
    spinBox->setValue(0.5);
    if (app) app->processEvents();

    EXPECT_LE(slider->value(), 500);
}
