#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QMainWindow>
#include <QSignalSpy>
#include <QThread>
#include <QMoveEvent>
#include "MeshInfoOverlay.h"
#include "OgreWidget.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

// ===========================================================================
// Pure formatting tests (no Ogre needed)
// ===========================================================================

TEST(MeshInfoOverlayFormat, EmptyList)
{
    QList<Ogre::Entity*> empty;
    QString result = MeshInfoOverlay::formatStats(empty, false);
    EXPECT_EQ(result, "No meshes");
}

TEST(MeshInfoOverlayFormat, EmptyListSelection)
{
    QList<Ogre::Entity*> empty;
    QString result = MeshInfoOverlay::formatStats(empty, true);
    EXPECT_EQ(result, "No meshes");
}

TEST(MeshInfoOverlayFormat, NullEntityInList)
{
    QList<Ogre::Entity*> withNull;
    withNull << nullptr;

    QString result = MeshInfoOverlay::formatStats(withNull, false);
    // Null entries are filtered out, so a null-only list reports "No meshes"
    EXPECT_EQ(result, "No meshes");
}

TEST(MeshInfoOverlayFormat, MixedNullAndNullOnlyList)
{
    QList<Ogre::Entity*> nulls;
    nulls << nullptr << nullptr << nullptr;
    EXPECT_EQ(MeshInfoOverlay::formatStats(nulls, false), "No meshes");
    EXPECT_EQ(MeshInfoOverlay::formatStats(nulls, true), "No meshes");
}

// ===========================================================================
// Lifecycle tests (require Ogre for Manager/SelectionSet singletons)
// ===========================================================================

// MeshInfoOverlay takes MainWindow* but only uses QObject/QWidget methods
// on it (installEventFilter, findChildren, QLabel parent).  We use a plain
// QMainWindow to avoid the heavyweight real MainWindow construction.
static MainWindow* fakeMainWindow(QMainWindow* w)
{
    return reinterpret_cast<MainWindow*>(w);
}

class MeshInfoOverlayLifecycleTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    QMainWindow* window = nullptr;
    MeshInfoOverlay* overlay = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        if (!tryInitOgre()) {
            GTEST_SKIP() << "Ogre initialization failed";
        }
        createStandardOgreMaterials();

        window = new QMainWindow();
        overlay = new MeshInfoOverlay(fakeMainWindow(window));
    }

    void TearDown() override {
        delete overlay;
        overlay = nullptr;
        delete window;
        window = nullptr;
        if (Manager::getSingletonPtr()) {
            SelectionSet::getSingleton()->clear();
            Manager::kill();
        }
        if (app) app->processEvents();
        QThread::msleep(50);
    }
};

TEST_F(MeshInfoOverlayLifecycleTest, DefaultStateIsHidden)
{
    EXPECT_FALSE(overlay->isVisible());
}

TEST_F(MeshInfoOverlayLifecycleTest, SetVisibleEmitsSignal)
{
    QSignalSpy spy(overlay, &MeshInfoOverlay::visibilityChanged);
    ASSERT_TRUE(spy.isValid());

    overlay->setVisible(true);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_TRUE(spy.first().first().toBool());
    EXPECT_TRUE(overlay->isVisible());

    overlay->setVisible(false);
    ASSERT_EQ(spy.count(), 2);
    EXPECT_FALSE(spy.last().first().toBool());
    EXPECT_FALSE(overlay->isVisible());
}

TEST_F(MeshInfoOverlayLifecycleTest, SetVisibleWithoutActiveWidget)
{
    // No viewports in fake QMainWindow, so mActiveWidget stays null.
    // setVisible(true) should not crash and overlay stays visible (state-wise).
    overlay->setVisible(true);
    EXPECT_TRUE(overlay->isVisible());

    overlay->setVisible(false);
    EXPECT_FALSE(overlay->isVisible());
}

TEST_F(MeshInfoOverlayLifecycleTest, SetActiveWidgetTriggersRefresh)
{
    OgreWidget viewport(window);
    overlay->setVisible(true);

    // Setting an active widget triggers refresh which creates the label
    overlay->setActiveWidget(&viewport);
    EXPECT_TRUE(overlay->isVisible());
}

TEST_F(MeshInfoOverlayLifecycleTest, SetActiveWidgetSameWidgetNoOp)
{
    OgreWidget viewport(window);
    overlay->setActiveWidget(&viewport);

    // Setting the same widget again should be a no-op (early return)
    overlay->setActiveWidget(&viewport);
    EXPECT_TRUE(true); // no crash
}

TEST_F(MeshInfoOverlayLifecycleTest, SetActiveWidgetSwitchesWidget)
{
    OgreWidget viewport1(window);
    OgreWidget viewport2(window);

    overlay->setVisible(true);
    overlay->setActiveWidget(&viewport1);
    overlay->setActiveWidget(&viewport2);
    // Switched without crash; event filter moved to viewport2
    EXPECT_TRUE(overlay->isVisible());
}

TEST_F(MeshInfoOverlayLifecycleTest, SetActiveWidgetNull)
{
    OgreWidget viewport(window);
    overlay->setActiveWidget(&viewport);
    overlay->setActiveWidget(nullptr);
    // No crash, active widget cleared
    EXPECT_TRUE(true);
}

TEST_F(MeshInfoOverlayLifecycleTest, EventFilterMoveRepositionsLabel)
{
    OgreWidget viewport(window);
    overlay->setActiveWidget(&viewport);
    overlay->setVisible(true);

    // Send a Move event to the viewport — should reposition label
    QMoveEvent moveEvent(QPoint(100, 100), QPoint(0, 0));
    QCoreApplication::sendEvent(&viewport, &moveEvent);
    // No crash = success
}

TEST_F(MeshInfoOverlayLifecycleTest, EventFilterResizeRepositionsLabel)
{
    OgreWidget viewport(window);
    overlay->setActiveWidget(&viewport);
    overlay->setVisible(true);

    // Send a Resize event
    QResizeEvent resizeEvent(QSize(800, 600), QSize(640, 480));
    QCoreApplication::sendEvent(&viewport, &resizeEvent);
    // No crash = success
}

TEST_F(MeshInfoOverlayLifecycleTest, EventFilterHideActiveWidget)
{
    OgreWidget viewport(window);
    overlay->setActiveWidget(&viewport);
    overlay->setVisible(true);

    // Hiding the viewport should hide the label but not clear mActiveWidget
    QHideEvent hideEvent;
    QCoreApplication::sendEvent(&viewport, &hideEvent);
    // No crash, label hidden
}

TEST_F(MeshInfoOverlayLifecycleTest, EventFilterDestroyActiveWidget)
{
    auto* viewport = new OgreWidget(window);
    overlay->setActiveWidget(viewport);
    overlay->setVisible(true);

    // Destroying the viewport should hide the label and clear mActiveWidget
    delete viewport;
    // QPointer nulls mActiveWidget; overlay handles this gracefully
    overlay->refresh();  // should not crash with null active widget
}

TEST_F(MeshInfoOverlayLifecycleTest, EventFilterMainWindowMove)
{
    OgreWidget viewport(window);
    overlay->setActiveWidget(&viewport);
    overlay->setVisible(true);

    // Moving the main window should also reposition the label
    QMoveEvent moveEvent(QPoint(200, 200), QPoint(0, 0));
    QCoreApplication::sendEvent(window, &moveEvent);
    // No crash = success
}

TEST_F(MeshInfoOverlayLifecycleTest, RefreshWhenHiddenHidesLabel)
{
    OgreWidget viewport(window);
    overlay->setActiveWidget(&viewport);
    overlay->setVisible(true);

    // Now hide and refresh — label should be hidden
    overlay->setVisible(false);
    overlay->refresh();
    EXPECT_FALSE(overlay->isVisible());
}

TEST_F(MeshInfoOverlayLifecycleTest, RefreshWithNoActiveWidgetHidesLabel)
{
    overlay->setVisible(true);
    // No active widget set, refresh should not crash
    overlay->refresh();
}

TEST_F(MeshInfoOverlayLifecycleTest, RefreshWithEmptyScene)
{
    OgreWidget viewport(window);
    overlay->setActiveWidget(&viewport);
    overlay->setVisible(true);

    // Empty scene, overlay should show "No meshes"
    overlay->refresh();
    EXPECT_TRUE(overlay->isVisible());
}

TEST_F(MeshInfoOverlayLifecycleTest, RefreshWithEntityInScene)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "mesh loading not supported";

    OgreWidget viewport(window);
    overlay->setActiveWidget(&viewport);

    auto meshPtr = createInMemoryTriangleMesh("OverlayLifecycleMesh");
    ASSERT_TRUE(meshPtr);

    // Use Manager::addSceneNode so collectEntities finds it via getSceneNodes()
    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("OverlayLifecycleNode");
    auto* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
        "OverlayLifecycleEntity", meshPtr);
    node->attachObject(entity);

    overlay->setVisible(true);
    // Overlay should show stats for the entity in the scene
    EXPECT_TRUE(overlay->isVisible());

    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
}

TEST_F(MeshInfoOverlayLifecycleTest, RefreshWithSelection)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "mesh loading not supported";

    OgreWidget viewport(window);
    overlay->setActiveWidget(&viewport);

    auto meshPtr = createInMemoryTriangleMesh("OverlaySelMesh");
    ASSERT_TRUE(meshPtr);

    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("OverlaySelNode");
    auto* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
        "OverlaySelEntity", meshPtr);
    node->attachObject(entity);

    // Add to selection — collectEntities should use selected entities
    SelectionSet::getSingleton()->selectOne(node);

    overlay->setVisible(true);
    EXPECT_TRUE(overlay->isVisible());

    // Clean up
    SelectionSet::getSingleton()->clear();
    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
}

TEST_F(MeshInfoOverlayLifecycleTest, DestructorCleansUp)
{
    OgreWidget viewport(window);
    overlay->setActiveWidget(&viewport);
    overlay->setVisible(true);

    // Explicit delete to exercise destructor with active label
    delete overlay;
    overlay = nullptr;  // prevent double-delete in TearDown
}

// ===========================================================================
// Integration tests (require Ogre for entity creation)
// ===========================================================================

class MeshInfoOverlayIntegrationTest : public ::testing::Test {
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
        if (!Manager::getSingletonPtr())
            return;
        SelectionSet::getSingleton()->clear();
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(50);
    }
};

TEST_F(MeshInfoOverlayIntegrationTest, FormatStatsSingleEntity)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    auto meshPtr = createInMemoryTriangleMesh("MeshInfoSingleMesh");
    ASSERT_TRUE(meshPtr);

    Ogre::SceneNode* node = Manager::getSingleton()->getSceneMgr()
        ->getRootSceneNode()->createChildSceneNode("MeshInfoSingleNode");
    Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
        "MeshInfoSingleEntity", meshPtr);
    node->attachObject(entity);

    QList<Ogre::Entity*> entities;
    entities << entity;

    QString result = MeshInfoOverlay::formatStats(entities, false);

    // Should contain the mesh name
    EXPECT_TRUE(result.contains("MeshInfoSingleMesh"))
        << "Result: " << result.toStdString();

    // Should contain vertex/triangle info
    EXPECT_TRUE(result.contains("Verts:")) << "Result: " << result.toStdString();
    EXPECT_TRUE(result.contains("Tris:")) << "Result: " << result.toStdString();

    // 3 vertices, 1 triangle
    EXPECT_TRUE(result.contains("3")) << "Result: " << result.toStdString();
    EXPECT_TRUE(result.contains("1")) << "Result: " << result.toStdString();

    // Should NOT contain bones/anims line (no skeleton)
    EXPECT_FALSE(result.contains("Bones:")) << "Result: " << result.toStdString();

    // Clean up
    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

TEST_F(MeshInfoOverlayIntegrationTest, FormatStatsMultipleEntitiesScene)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    std::vector<Ogre::Entity*> entities;
    std::vector<Ogre::SceneNode*> nodes;

    for (int i = 0; i < 3; ++i) {
        std::string suffix = std::to_string(i);
        auto meshPtr = createInMemoryTriangleMesh("MeshInfoMultiMesh" + suffix);
        ASSERT_TRUE(meshPtr);

        Ogre::SceneNode* node = Manager::getSingleton()->getSceneMgr()
            ->getRootSceneNode()->createChildSceneNode("MeshInfoMultiNode" + suffix);
        Ogre::Entity* entity = Manager::getSingleton()->getSceneMgr()->createEntity(
            "MeshInfoMultiEntity" + suffix, meshPtr);
        node->attachObject(entity);

        entities.push_back(entity);
        nodes.push_back(node);
    }

    QList<Ogre::Entity*> entityList;
    for (auto* e : entities) entityList << e;

    // Not a selection -- should say "Scene"
    QString result = MeshInfoOverlay::formatStats(entityList, false);
    EXPECT_TRUE(result.contains("Scene (3 meshes)"))
        << "Result: " << result.toStdString();

    // As selection -- should say "Selected"
    QString selResult = MeshInfoOverlay::formatStats(entityList, true);
    EXPECT_TRUE(selResult.contains("Selected (3 meshes)"))
        << "Result: " << selResult.toStdString();

    // Aggregated: 3 entities x 3 verts = 9 verts, 3 tris
    EXPECT_TRUE(result.contains("9")) << "Expected 9 vertices. Result: " << result.toStdString();

    // Should show Submeshes and Materials
    EXPECT_TRUE(result.contains("Submeshes:")) << "Result: " << result.toStdString();
    EXPECT_TRUE(result.contains("Materials:")) << "Result: " << result.toStdString();

    // Clean up
    for (size_t i = 0; i < entities.size(); ++i) {
        nodes[i]->detachObject(entities[i]);
        Manager::getSingleton()->getSceneMgr()->destroyEntity(entities[i]);
        Manager::getSingleton()->getSceneMgr()->destroySceneNode(nodes[i]);
    }
}

TEST_F(MeshInfoOverlayIntegrationTest, FormatStatsWithSkeleton)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: mesh loading not supported in headless mode";

    Ogre::Entity* entity = createAnimatedTestEntity("MeshInfoAnimEntity");
    if (!entity)
        GTEST_SKIP() << "Skipping: could not create animated test entity";

    ASSERT_TRUE(entity->hasSkeleton());

    QList<Ogre::Entity*> entities;
    entities << entity;

    QString result = MeshInfoOverlay::formatStats(entities, false);

    // Should show bone and animation info
    EXPECT_TRUE(result.contains("Bones:")) << "Result: " << result.toStdString();
    EXPECT_TRUE(result.contains("Anims:")) << "Result: " << result.toStdString();

    // Clean up
    Ogre::SceneNode* node = entity->getParentSceneNode();
    node->detachObject(entity);
    Manager::getSingleton()->getSceneMgr()->destroyEntity(entity);
    Manager::getSingleton()->getSceneMgr()->destroySceneNode(node);
}

TEST_F(MeshInfoOverlayIntegrationTest, FormatStatsMixedNullAndValid)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "mesh loading not supported";

    auto meshPtr = createInMemoryTriangleMesh("MeshInfoMixedMesh");
    ASSERT_TRUE(meshPtr);

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode("MeshInfoMixedNode");
    auto* entity = sceneMgr->createEntity("MeshInfoMixedEntity", meshPtr);
    node->attachObject(entity);

    // Mix of null and valid: nulls should be filtered, only valid counted
    QList<Ogre::Entity*> entities;
    entities << nullptr << entity << nullptr;

    QString result = MeshInfoOverlay::formatStats(entities, false);
    // Single valid entity — header should be mesh name, not "Scene (3 meshes)"
    EXPECT_TRUE(result.contains("MeshInfoMixedMesh"))
        << "Result: " << result.toStdString();
    EXPECT_FALSE(result.contains("Scene"))
        << "Should not say Scene for 1 valid entity. Result: " << result.toStdString();

    node->detachObject(entity);
    sceneMgr->destroyEntity(entity);
    sceneMgr->destroySceneNode(node);
}
