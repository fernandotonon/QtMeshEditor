#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>
#include <QTemporaryDir>
#include "MeshLodController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

// ===========================================================================
// Test fixture
// ===========================================================================

class MeshLodControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        MeshLodController::kill();
        Manager::kill();
        QThread::msleep(20);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }

    void TearDown() override {
        if (Manager::getSingletonPtr())
            SelectionSet::getSingleton()->clear();
        app->processEvents();
        MeshLodController::kill();
        Manager::kill();
        QThread::msleep(20);
    }

    // Create a triangle mesh entity and select it via SelectionSet::selectOne(entity)
    // so that hasEntities() returns true. Returns nullptr if GL context unavailable.
    Ogre::Entity* createAndSelectMesh(const std::string& name) {
        if (!canLoadMeshFiles()) return nullptr;

        auto meshPtr = createInMemoryTriangleMesh(name + "_mesh");
        if (!meshPtr) return nullptr;

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode(name + "_node");
        auto* entity = sceneMgr->createEntity(name + "_entity", meshPtr);
        node->attachObject(entity);

        // Must select the *entity* (not the node) so hasEntities() is true
        SelectionSet::getSingleton()->selectOne(entity);
        app->processEvents();
        return entity;
    }

    QApplication* app = nullptr;
};

// ===========================================================================
// Singleton lifecycle
// ===========================================================================

TEST_F(MeshLodControllerTest, InstanceReturnsSameObject) {
    auto* a = MeshLodController::instance();
    auto* b = MeshLodController::instance();
    EXPECT_EQ(a, b);
}

TEST_F(MeshLodControllerTest, KillResetsInstance) {
    MeshLodController::instance(); // ensure singleton exists
    MeshLodController::kill();     // destroy it
    // After kill, a fresh functional instance must be available
    auto* b = MeshLodController::instance();
    ASSERT_NE(b, nullptr);
    // Fresh instance starts with no selection (pointer equality is unreliable
    // due to allocator reuse — test behaviour instead)
    EXPECT_FALSE(b->hasSelection());
}

TEST_F(MeshLodControllerTest, QmlInstanceReturnsSameObject) {
    auto* a = MeshLodController::instance();
    auto* b = MeshLodController::qmlInstance(nullptr, nullptr);
    EXPECT_EQ(a, b);
}

// ===========================================================================
// No-selection state
// ===========================================================================

TEST_F(MeshLodControllerTest, HasSelectionFalseWithNoSelection) {
    EXPECT_FALSE(MeshLodController::instance()->hasSelection());
}

TEST_F(MeshLodControllerTest, CurrentLodLevelsZeroWithNoSelection) {
    EXPECT_EQ(MeshLodController::instance()->currentLodLevels(), 0);
}

TEST_F(MeshLodControllerTest, LodLevelInfoEmptyWithNoSelection) {
    EXPECT_TRUE(MeshLodController::instance()->lodLevelInfo().isEmpty());
}

TEST_F(MeshLodControllerTest, PreviewLodNoopWithNoSelection) {
    EXPECT_NO_FATAL_FAILURE(MeshLodController::instance()->previewLod(0));
    EXPECT_NO_FATAL_FAILURE(MeshLodController::instance()->previewLod(-1));
}

TEST_F(MeshLodControllerTest, RemoveLodsNoopWithNoSelection) {
    EXPECT_NO_FATAL_FAILURE(MeshLodController::instance()->removeLods());
}

TEST_F(MeshLodControllerTest, GenerateLodsEmitsErrorWithNoSelection) {
    auto* ctrl = MeshLodController::instance();
    QSignalSpy spy(ctrl, &MeshLodController::error);
    ASSERT_TRUE(spy.isValid());

    ctrl->generateLods(2, QVariantList{});
    app->processEvents();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_TRUE(spy.first().first().toString().contains("No mesh found in selection"));
}

TEST_F(MeshLodControllerTest, GenerateAutoLodsEmitsErrorWithNoSelection) {
    auto* ctrl = MeshLodController::instance();
    QSignalSpy spy(ctrl, &MeshLodController::error);
    ASSERT_TRUE(spy.isValid());

    ctrl->generateAutoLods();
    app->processEvents();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_TRUE(spy.first().first().toString().contains("No mesh found in selection"));
}

TEST_F(MeshLodControllerTest, ExportLodsEmitsErrorWithNoSelection) {
    auto* ctrl = MeshLodController::instance();
    QSignalSpy spy(ctrl, &MeshLodController::error);
    ASSERT_TRUE(spy.isValid());

    ctrl->exportLods("gltf");
    app->processEvents();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_TRUE(spy.first().first().toString().contains("No mesh found in selection"));
}

// ===========================================================================
// With a selected entity — basic state
// ===========================================================================

TEST_F(MeshLodControllerTest, HasSelectionTrueWithEntitySelected) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("HasSel"), nullptr);

    EXPECT_TRUE(MeshLodController::instance()->hasSelection());
}

TEST_F(MeshLodControllerTest, HasSelectionTrueWhenAncestorNodeSelected) {
    ASSERT_TRUE(canLoadMeshFiles());
    SelectionSet::getSingleton()->clear();

    Ogre::SceneNode* parent = Manager::getSingleton()->addSceneNode(QStringLiteral("lod_nested_parent"));
    ASSERT_NE(parent, nullptr);
    SelectionSet::getSingleton()->clear();

    Ogre::SceneNode* child = parent->createChildSceneNode("lod_nested_child");
    Ogre::MeshPtr mesh = createInMemoryTriangleMesh("lod_nested_mesh_data");
    ASSERT_NE(mesh, nullptr);
    Ogre::SceneManager* sm = Manager::getSingleton()->getSceneMgr();
    Ogre::Entity* entity = sm->createEntity(child->getName(), mesh);
    child->attachObject(entity);

    SelectionSet::getSingleton()->append(parent);
    app->processEvents();

    EXPECT_TRUE(MeshLodController::instance()->hasSelection());

    SelectionSet::getSingleton()->clear();
    Manager::getSingleton()->destroySceneNode(parent);
}

TEST_F(MeshLodControllerTest, GenerateLodsSucceedsWithAncestorNodeSelectionOnly) {
    ASSERT_TRUE(canLoadMeshFiles());
    SelectionSet::getSingleton()->clear();

    Ogre::SceneNode* parent = Manager::getSingleton()->addSceneNode(QStringLiteral("lod_gen_nested_parent"));
    ASSERT_NE(parent, nullptr);
    SelectionSet::getSingleton()->clear();

    Ogre::SceneNode* child = parent->createChildSceneNode("lod_gen_nested_child");
    Ogre::MeshPtr mesh = createInMemoryTriangleMesh("lod_gen_nested_mesh_data");
    ASSERT_NE(mesh, nullptr);
    Ogre::SceneManager* sm = Manager::getSingleton()->getSceneMgr();
    Ogre::Entity* entity = sm->createEntity(child->getName(), mesh);
    child->attachObject(entity);

    SelectionSet::getSingleton()->append(parent);
    app->processEvents();

    auto* ctrl = MeshLodController::instance();
    QSignalSpy errSpy(ctrl, &MeshLodController::error);
    QSignalSpy okSpy(ctrl, &MeshLodController::generationSucceeded);
    ASSERT_TRUE(errSpy.isValid());
    ASSERT_TRUE(okSpy.isValid());

    ctrl->generateLods(1, QVariantList{0.5f});
    app->processEvents();

    EXPECT_EQ(errSpy.count(), 0);
    ASSERT_EQ(okSpy.count(), 1);
    EXPECT_GE(ctrl->currentLodLevels(), 1);

    SelectionSet::getSingleton()->clear();
    Manager::getSingleton()->destroySceneNode(parent);
}

TEST_F(MeshLodControllerTest, CurrentLodLevelsZeroBeforeGeneration) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("LodLevelZero"), nullptr);

    EXPECT_EQ(MeshLodController::instance()->currentLodLevels(), 0);
}

TEST_F(MeshLodControllerTest, LodLevelInfoReturnsBaseEntryWithEntity) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("LodInfoBase"), nullptr);

    auto info = MeshLodController::instance()->lodLevelInfo();
    ASSERT_EQ(info.size(), 1);

    auto base = info.first().toMap();
    EXPECT_EQ(base["level"].toInt(), 0);
    EXPECT_EQ(base["label"].toString(), "Base");
    EXPECT_GE(base["triangles"].toInt(), 1);
}

TEST_F(MeshLodControllerTest, ExportLodsEmitsErrorWhenNoLodsGenerated) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("ExportNoLods"), nullptr);

    auto* ctrl = MeshLodController::instance();
    QSignalSpy spy(ctrl, &MeshLodController::error);
    ASSERT_TRUE(spy.isValid());

    ctrl->exportLods("gltf");
    app->processEvents();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_TRUE(spy.first().first().toString().contains("No LOD levels"));
}

TEST_F(MeshLodControllerTest, SelectionChangePropagatesLodChangedSignal) {
    ASSERT_TRUE(canLoadMeshFiles());

    auto* ctrl = MeshLodController::instance();
    QSignalSpy selSpy(ctrl, &MeshLodController::selectionChanged);
    QSignalSpy lodSpy(ctrl, &MeshLodController::lodChanged);
    ASSERT_TRUE(selSpy.isValid());
    ASSERT_TRUE(lodSpy.isValid());

    ASSERT_NE(createAndSelectMesh("SelChange"), nullptr);

    EXPECT_GE(selSpy.count(), 1);
    EXPECT_GE(lodSpy.count(), 1);
}

// ===========================================================================
// LOD generation
// ===========================================================================

TEST_F(MeshLodControllerTest, GenerateLodsEmitsGenerationSucceeded) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("GenSucceed"), nullptr);

    auto* ctrl = MeshLodController::instance();
    QSignalSpy genSpy(ctrl, &MeshLodController::generationSucceeded);
    QSignalSpy lodSpy(ctrl, &MeshLodController::lodChanged);
    ASSERT_TRUE(genSpy.isValid());
    ASSERT_TRUE(lodSpy.isValid());

    ctrl->generateLods(1, QVariantList{0.5f});
    app->processEvents();

    ASSERT_EQ(genSpy.count(), 1);
    EXPECT_EQ(genSpy.first().first().toInt(), 1);
    EXPECT_GE(lodSpy.count(), 1);
}

TEST_F(MeshLodControllerTest, GenerateLodsUpdatesCurrentLodLevels) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("GenLevels"), nullptr);

    auto* ctrl = MeshLodController::instance();
    ctrl->generateLods(1, QVariantList{0.5f});
    app->processEvents();

    EXPECT_GE(ctrl->currentLodLevels(), 1);
}

TEST_F(MeshLodControllerTest, GenerateLodsCountClampedToMin1) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("ClampMin"), nullptr);

    auto* ctrl = MeshLodController::instance();
    QSignalSpy spy(ctrl, &MeshLodController::generationSucceeded);
    ASSERT_TRUE(spy.isValid());

    ctrl->generateLods(0, QVariantList{});   // 0 clamped → 1
    app->processEvents();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.first().first().toInt(), 1);
}

TEST_F(MeshLodControllerTest, GenerateLodsCountClampedToMax4) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("ClampMax"), nullptr);

    auto* ctrl = MeshLodController::instance();
    QSignalSpy spy(ctrl, &MeshLodController::generationSucceeded);
    ASSERT_TRUE(spy.isValid());

    ctrl->generateLods(10, QVariantList{});  // 10 clamped → 4
    app->processEvents();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.first().first().toInt(), 4);
}

TEST_F(MeshLodControllerTest, GenerateLodsReductionFallbackWhenListShort) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("FallbackReduction"), nullptr);

    auto* ctrl = MeshLodController::instance();
    QSignalSpy spy(ctrl, &MeshLodController::generationSucceeded);
    ASSERT_TRUE(spy.isValid());

    // Pass 2 levels but empty reductions list — should use fallback values
    ctrl->generateLods(2, QVariantList{});
    app->processEvents();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.first().first().toInt(), 2);
}

TEST_F(MeshLodControllerTest, LodLevelInfoAfterGeneration) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("InfoAfterGen"), nullptr);

    auto* ctrl = MeshLodController::instance();
    ctrl->generateLods(1, QVariantList{0.5f});
    app->processEvents();

    auto info = ctrl->lodLevelInfo();
    ASSERT_GE(info.size(), 2);

    auto base = info.at(0).toMap();
    EXPECT_EQ(base["level"].toInt(), 0);
    EXPECT_EQ(base["label"].toString(), "Base");

    auto lod1 = info.at(1).toMap();
    EXPECT_EQ(lod1["level"].toInt(), 1);
    EXPECT_EQ(lod1["label"].toString(), "LOD 1");
}

TEST_F(MeshLodControllerTest, GenerateAutoLodsEmitsGenerationSucceeded) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("AutoSucceed"), nullptr);

    auto* ctrl = MeshLodController::instance();
    QSignalSpy spy(ctrl, &MeshLodController::generationSucceeded);
    ASSERT_TRUE(spy.isValid());

    ctrl->generateAutoLods();
    app->processEvents();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.first().first().toInt(), -1);  // -1 = auto
}

// ===========================================================================
// Remove LODs
// ===========================================================================

TEST_F(MeshLodControllerTest, RemoveLodsClearsLodLevels) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("Remove"), nullptr);

    auto* ctrl = MeshLodController::instance();
    ctrl->generateLods(1, QVariantList{0.5f});
    app->processEvents();
    ASSERT_GE(ctrl->currentLodLevels(), 1);

    QSignalSpy spy(ctrl, &MeshLodController::lodChanged);
    ctrl->removeLods();
    app->processEvents();

    EXPECT_EQ(ctrl->currentLodLevels(), 0);
    EXPECT_GE(spy.count(), 1);
}

TEST_F(MeshLodControllerTest, RemoveLodsLodLevelInfoReturnsSingleBaseAfterRemoval) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("RemoveInfo"), nullptr);

    auto* ctrl = MeshLodController::instance();
    ctrl->generateLods(2, QVariantList{0.5f, 0.75f});
    app->processEvents();
    ASSERT_GE(ctrl->lodLevelInfo().size(), 2);

    ctrl->removeLods();
    app->processEvents();

    EXPECT_EQ(ctrl->lodLevelInfo().size(), 1);
    EXPECT_EQ(ctrl->lodLevelInfo().first().toMap()["label"].toString(), "Base");
}

// ===========================================================================
// Preview LOD
// ===========================================================================

TEST_F(MeshLodControllerTest, PreviewLodWithEntityDoesNotCrash) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("Preview"), nullptr);

    EXPECT_NO_FATAL_FAILURE(MeshLodController::instance()->previewLod(-1));
    EXPECT_NO_FATAL_FAILURE(MeshLodController::instance()->previewLod(0));
}

TEST_F(MeshLodControllerTest, PreviewLodAfterGenerationDoesNotCrash) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("PreviewAfterGen"), nullptr);

    auto* ctrl = MeshLodController::instance();
    ctrl->generateLods(1, QVariantList{0.5f});
    app->processEvents();

    EXPECT_NO_FATAL_FAILURE(ctrl->previewLod(1));
    EXPECT_NO_FATAL_FAILURE(ctrl->previewLod(-1));  // restore
}

// ===========================================================================
// Export LODs
// ===========================================================================

TEST_F(MeshLodControllerTest, ExportLodsEmitsExportLodsRequestedAfterGeneration) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("ExportReq"), nullptr);

    auto* ctrl = MeshLodController::instance();
    ctrl->generateLods(1, QVariantList{0.5f});
    app->processEvents();

    QSignalSpy spy(ctrl, &MeshLodController::exportLodsRequested);
    ASSERT_TRUE(spy.isValid());

    ctrl->exportLods("gltf");
    app->processEvents();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.first().first().toString(), "gltf");
}

TEST_F(MeshLodControllerTest, DoExportLodsNoopWithNoLods) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("DoExportNoLods"), nullptr);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    auto* ctrl = MeshLodController::instance();
    QSignalSpy spy(ctrl, &MeshLodController::exportSucceeded);
    QSignalSpy errSpy(ctrl, &MeshLodController::error);

    // No LODs → doExportLods returns early (totalLods <= 1), no signal
    ctrl->doExportLods("obj", tmpDir.path());
    app->processEvents();

    EXPECT_EQ(spy.count(), 0);
    EXPECT_EQ(errSpy.count(), 0);
}

TEST_F(MeshLodControllerTest, DoExportLodsEmitsExportSucceeded) {
    ASSERT_TRUE(canLoadMeshFiles());
    ASSERT_NE(createAndSelectMesh("DoExport"), nullptr);

    auto* ctrl = MeshLodController::instance();
    ctrl->generateLods(1, QVariantList{0.5f});
    app->processEvents();

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QSignalSpy spy(ctrl, &MeshLodController::exportSucceeded);
    ASSERT_TRUE(spy.isValid());

    ctrl->doExportLods("obj", tmpDir.path());
    app->processEvents();

    ASSERT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.first().at(1).toString(), tmpDir.path());
}
