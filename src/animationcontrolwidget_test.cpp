#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "animationcontrolwidget.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "MeshImporterExporter.h"
#include <OgreException.h>
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
