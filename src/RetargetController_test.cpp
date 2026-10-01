// #523 — RetargetController (wizard backend), RetargetAnimationCommand (undo),
// and the wizard QML itself.

#include <gtest/gtest.h>

#include "Manager.h"
#include "MaterialEditorQML.h"
#include "PropertiesPanelController.h"
#include "RetargetController.h"
#include "TestHelpers.h"
#include "ThemeManager.h"
#include "UndoManager.h"
#include "commands/RetargetAnimationCommand.h"

#include <QDir>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreEntity.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSkeletonInstance.h>

// ---------------------------------------------------------------------------
// No scene needed
// ---------------------------------------------------------------------------

TEST(RetargetControllerStandalone, OptionsValidateTheirIds)
{
    auto* c = RetargetController::instance();
    c->setTranslationMode(QStringLiteral("all"));
    EXPECT_EQ(c->translationMode(), QStringLiteral("all"));
    c->setTranslationMode(QStringLiteral("bogus"));
    EXPECT_EQ(c->translationMode(), QStringLiteral("all")) << "an unknown id must be ignored";
    c->setTranslationMode(QStringLiteral("root"));
    c->setSourceRest(QStringLiteral("first-frame"));
    EXPECT_EQ(c->sourceRest(), QStringLiteral("first-frame"));
    c->setSourceRest(QStringLiteral("bind"));
    EXPECT_EQ(c->bundledMaps().size(), Retarget::bundledBoneMapNames().size());
}

TEST(RetargetControllerStandalone, BoneMapFileRoundTrip)
{
    auto* c = RetargetController::instance();
    ASSERT_TRUE(c->loadBundledMap(QStringLiteral("mixamo_to_unreal")));
    const size_t n = c->boneMap().pairs.size();
    EXPECT_GT(n, 20u);
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("m.bonemap"));
    ASSERT_TRUE(c->saveBoneMapTo(path));
    c->clearMapping();
    EXPECT_EQ(c->mappedCount(), 0);
    ASSERT_TRUE(c->loadBoneMapFrom(path));
    EXPECT_EQ(c->boneMap().pairs.size(), n);
    EXPECT_FALSE(c->loadBoneMapFrom(QDir(dir.path()).filePath(QStringLiteral("missing.bonemap"))));
    EXPECT_FALSE(c->lastOk());
    c->clearMapping();
}

TEST(RetargetControllerStandalone, ApplyRefusesWithoutEntities)
{
    auto* c = RetargetController::instance();
    c->setSourceEntity(QString());
    c->setTargetEntity(QString());
    EXPECT_FALSE(c->apply());
    EXPECT_FALSE(c->lastOk());
    EXPECT_FALSE(c->startPreview());
}

TEST(RetargetControllerStandalone, CommandForAMissingEntityIsAnInvalidNoOp)
{
    RetargetAnimationCommand cmd("no_such_entity", "clip", true);
    EXPECT_FALSE(cmd.valid());
    cmd.undo();   // must not crash
    cmd.redo();
    cmd.redo();
}

// ---------------------------------------------------------------------------
// Live scene
// ---------------------------------------------------------------------------

class RetargetControllerSceneTest : public ::testing::Test
{
protected:
    void SetUp() override { ASSERT_TRUE(tryInitOgre()); }
    void TearDown() override
    {
        RetargetController::instance()->stopPreview();
        RetargetController::instance()->setSourceEntity(QString());
        RetargetController::instance()->setTargetEntity(QString());
        if (auto* mgr = Manager::getSingletonPtr())
            if (auto* scene = mgr->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
    }
};

TEST_F(RetargetControllerSceneTest, ApplyCreatesAClipThatUndoAndRedoRemoveAndRestore)
{
    Ogre::Entity* src = createAnimatedTestEntity("RT_Src");
    Ogre::Entity* tgt = createAnimatedTestEntity("RT_Tgt");
    ASSERT_NE(src, nullptr);
    ASSERT_NE(tgt, nullptr);
    auto* c = RetargetController::instance();
    c->refresh();
    ASSERT_TRUE(c->skeletalEntities().contains(QStringLiteral("RT_Src")));
    c->setSourceEntity(QStringLiteral("RT_Src"));
    c->setTargetEntity(QStringLiteral("RT_Tgt"));
    EXPECT_EQ(c->sourceAnimation(), QStringLiteral("TestAnim"));
    EXPECT_EQ(c->mappedCount(), 2) << "Root and Child auto-map by name";
    c->setNewAnimationName(QStringLiteral("Copied"));

    ASSERT_TRUE(c->apply()) << c->status().toStdString();
    Ogre::SkeletonInstance* ts = tgt->getSkeleton();
    ASSERT_TRUE(ts->hasAnimation("Copied"));
    EXPECT_TRUE(tgt->getAllAnimationStates()->hasAnimationState("Copied"))
        << "the new clip must be a normal, playable skeletal clip";

    auto* um = UndoManager::getSingleton();
    ASSERT_NE(um, nullptr);
    um->undo();
    EXPECT_FALSE(ts->hasAnimation("Copied"));
    EXPECT_FALSE(tgt->getAllAnimationStates()->hasAnimationState("Copied"))
        << "undo must not leave a ghost animation state";
    um->redo();
    ASSERT_TRUE(ts->hasAnimation("Copied"));
    EXPECT_TRUE(tgt->getAllAnimationStates()->hasAnimationState("Copied"));

    // Applying again must not clobber the existing clip.
    ASSERT_TRUE(c->apply());
    EXPECT_TRUE(ts->hasAnimation("Copied_2"));
    um->undo();
    um->undo();
}

TEST_F(RetargetControllerSceneTest, PreviewRestoresEverythingWhenItStops)
{
    Ogre::Entity* src = createAnimatedTestEntity("RT_PSrc");
    Ogre::Entity* tgt = createAnimatedTestEntity("RT_PTgt");
    ASSERT_NE(src, nullptr);
    ASSERT_NE(tgt, nullptr);
    tgt->getAllAnimationStates()->getAnimationState("TestAnim")->setEnabled(true);
    const Ogre::Vector3 before = tgt->getParentSceneNode()->getPosition();

    auto* c = RetargetController::instance();
    c->refresh();
    c->setSourceEntity(QStringLiteral("RT_PSrc"));
    c->setTargetEntity(QStringLiteral("RT_PTgt"));
    ASSERT_TRUE(c->startPreview()) << c->status().toStdString();
    EXPECT_TRUE(c->previewing());
    EXPECT_TRUE(tgt->getSkeleton()->hasAnimation("__qtme_retarget_preview"));
    EXPECT_FALSE(tgt->getAllAnimationStates()->getAnimationState("TestAnim")->getEnabled())
        << "the target's own clip is paused during the preview";

    c->stopPreview();
    EXPECT_FALSE(c->previewing());
    EXPECT_FALSE(tgt->getSkeleton()->hasAnimation("__qtme_retarget_preview"));
    EXPECT_FALSE(tgt->getAllAnimationStates()->hasAnimationState("__qtme_retarget_preview"))
        << "the preview must not leave a ghost animation state";
    EXPECT_TRUE(tgt->getAllAnimationStates()->getAnimationState("TestAnim")->getEnabled())
        << "the user's animation-state flags must come back";
    EXPECT_LT((tgt->getParentSceneNode()->getPosition() - before).length(), 1e-6f)
        << "the side-by-side nudge must be undone";
}

TEST_F(RetargetControllerSceneTest, WizardQmlLoadsWithoutErrors)
{
    qmlRegisterSingletonType<PropertiesPanelController>("PropertiesPanel", 1, 0, "PropertiesPanelController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* { return PropertiesPanelController::qmlInstance(e, nullptr); });
    qmlRegisterSingletonType<RetargetController>("PropertiesPanel", 1, 0, "RetargetController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* { return RetargetController::qmlInstance(e, nullptr); });
    qmlRegisterSingletonType<MaterialEditorQML>("MaterialEditorQML", 1, 0, "MaterialEditorQML",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return MaterialEditorQML::qmlInstance(e, s); });
    qmlRegisterSingletonType<ThemeManager>("ThemeManager", 1, 0, "ThemeManager",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return ThemeManager::qmlInstance(e, s); });

    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/MaterialEditorQML/RetargetAnimationDialog.qml")));
    while (component.isLoading()) QCoreApplication::processEvents();
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    std::unique_ptr<QObject> obj(component.create());
    ASSERT_NE(obj, nullptr) << component.errorString().toStdString();
}
