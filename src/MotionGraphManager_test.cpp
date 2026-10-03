// #526 — MotionGraphManager: authoring + undo, driving Ogre animation states
// (cumulative blending, per-bone masks), restore on stop, sidecar, panel QML.

#include <gtest/gtest.h>

#include "AnimGraph.h"
#include "AnimationControlController.h"
#include "Manager.h"
#include "MotionGraphManager.h"
#include "PropertiesPanelController.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "ThemeManager.h"
#include "UndoManager.h"
#include "commands/MotionGraphCommands.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QTemporaryDir>

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreAnimationTrack.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSkeletonInstance.h>

using namespace AnimGraph;

namespace {

MotionGraphManager* mgr() { return MotionGraphManager::instance(); }

// createAnimatedTestEntity's rig (Root → Child, clip "TestAnim") plus a
// second clip "Other", so the graph has two states to move between.
Ogre::Entity* makeTwoClipEntity(const std::string& name)
{
    Ogre::Entity* e = createAnimatedTestEntity(name);
    Ogre::SkeletonInstance* skel = e->getSkeleton();
    Ogre::Animation* other = skel->createAnimation("Other", 2.0f);
    auto* t = other->createNodeTrack(0, skel->getBone("Root"));
    t->createNodeKeyFrame(0.0f);
    t->createNodeKeyFrame(2.0f);
    e->refreshAvailableAnimationState();
    return e;
}

Graph twoStates(const QString& paramType = QStringLiteral("float"))
{
    Graph g;
    g.entry = QStringLiteral("a");
    g.states = {{QStringLiteral("a"), QStringLiteral("TestAnim")}, {QStringLiteral("b"), QStringLiteral("Other")}};
    ParamType pt = ParamType::Float;
    paramTypeFromId(paramType, &pt);
    g.params = {{QStringLiteral("go"), pt, 0.0}};
    Transition t;
    t.id = QStringLiteral("t1");
    t.from = QStringLiteral("a");
    t.to = QStringLiteral("b");
    t.conditions = {{QStringLiteral("go"), pt == ParamType::Float ? Op::Greater : Op::IsTrue, 0.5}};
    t.duration = 0.4;
    g.transitions = {t};
    return g;
}

float maskEntry(Ogre::Entity* e, const char* clip, const char* bone)
{
    Ogre::AnimationState* st = e->getAnimationState(clip);
    if (!st->getEnabled()) return 0.0f;
    if (!st->hasBlendMask()) return 1.0f;
    return st->getBlendMaskEntry(e->getSkeleton()->getBone(bone)->getHandle());
}

} // namespace

// ---------------------------------------------------------------------------
// No scene needed
// ---------------------------------------------------------------------------

TEST(MotionGraphManagerStandalone, RejectsInvalidGraphsAndPlayWithoutEntity)
{
    mgr()->clear();
    Graph bad = twoStates();
    bad.transitions[0].to = QStringLiteral("nowhere");
    QString err;
    EXPECT_FALSE(mgr()->setGraph(QStringLiteral("X"), bad, false, &err));
    EXPECT_FALSE(mgr()->hasGraph(QStringLiteral("X")));
    EXPECT_FALSE(mgr()->play(QStringLiteral("X"), &err));
    EXPECT_FALSE(mgr()->playing());
    mgr()->tick(0.1);   // no-op when stopped
}

TEST(MotionGraphManagerStandalone, DocCommandSkipsItsFirstRedoAndSwapsDocuments)
{
    mgr()->clear();
    const QJsonObject g = toJson(twoStates());
    MotionGraphDocCommand cmd(QStringLiteral("x"), QStringLiteral("E"), QJsonObject{}, g);
    cmd.redo();   // skipped
    EXPECT_FALSE(mgr()->hasGraph(QStringLiteral("E")));
    cmd.undo();
    EXPECT_FALSE(mgr()->hasGraph(QStringLiteral("E")));
    cmd.redo();
    EXPECT_TRUE(mgr()->hasGraph(QStringLiteral("E")));
    cmd.undo();
    EXPECT_FALSE(mgr()->hasGraph(QStringLiteral("E")));
}

TEST(MotionGraphManagerStandalone, SidecarPathSitsBesideTheAsset)
{
    EXPECT_TRUE(MotionGraphManager::sidecarPath(QStringLiteral("/a/b/hero.glb")).endsWith(QStringLiteral("/a/b/hero.animgraph.json")));
}

// ---------------------------------------------------------------------------
// Live scene
// ---------------------------------------------------------------------------

class MotionGraphSceneTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        mgr()->clear();
        UndoManager::getSingleton()->clear();
    }
    void TearDown() override
    {
        mgr()->clear();
        UndoManager::getSingleton()->clear();
        if (auto* sel = SelectionSet::getSingletonPtr()) sel->clear();
        if (auto* m = Manager::getSingletonPtr())
            if (auto* scene = m->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
    }
};

TEST_F(MotionGraphSceneTest, PlayDrivesTheEntityStatesAndStopRestoresThem)
{
    Ogre::Entity* e = makeTwoClipEntity("MG_Play");
    e->getAnimationState("Other")->setEnabled(true);
    e->getAnimationState("Other")->setWeight(0.7f);
    ASSERT_TRUE(mgr()->setGraph(QStringLiteral("MG_Play"), twoStates(), false));
    QString err;
    ASSERT_TRUE(mgr()->play(QStringLiteral("MG_Play"), &err)) << err.toStdString();
    EXPECT_TRUE(mgr()->drives(e));
    EXPECT_EQ(e->getSkeleton()->getBlendMode(), Ogre::ANIMBLEND_CUMULATIVE)
        << "AVERAGE would halve two complementary masked clips";
    EXPECT_TRUE(e->getAnimationState("TestAnim")->getEnabled());
    EXPECT_FALSE(e->getAnimationState("Other")->getEnabled()) << "only the graph's clips play";
    EXPECT_FLOAT_EQ(maskEntry(e, "TestAnim", "Child"), 1.0f);

    mgr()->tick(0.25);
    EXPECT_NEAR(e->getAnimationState("TestAnim")->getTimePosition(), 0.25f, 1e-4f);

    mgr()->stop();
    EXPECT_FALSE(mgr()->drives(e));
    EXPECT_EQ(e->getSkeleton()->getBlendMode(), Ogre::ANIMBLEND_AVERAGE);
    EXPECT_FALSE(e->getAnimationState("TestAnim")->getEnabled());
    EXPECT_FALSE(e->getAnimationState("TestAnim")->hasBlendMask()) << "masks the graph created are removed";
    EXPECT_TRUE(e->getAnimationState("Other")->getEnabled());
    EXPECT_FLOAT_EQ(e->getAnimationState("Other")->getWeight(), 0.7f);
}

TEST_F(MotionGraphSceneTest, ParameterChangeCrossFadesThroughTheBlendMasks)
{
    Ogre::Entity* e = makeTwoClipEntity("MG_Blend");
    ASSERT_TRUE(mgr()->setGraph(QStringLiteral("MG_Blend"), twoStates(), false));
    ASSERT_TRUE(mgr()->play(QStringLiteral("MG_Blend")));
    QString err;
    ASSERT_TRUE(mgr()->setParamValue(QStringLiteral("go"), 1.0, &err)) << err.toStdString();
    EXPECT_DOUBLE_EQ(mgr()->graph(QStringLiteral("MG_Blend")).params[0].value, 0.0)
        << "while playing, only the running copy changes";
    mgr()->tick(0.0);   // fires a → b
    EXPECT_EQ(mgr()->currentState(), QStringLiteral("b"));
    mgr()->tick(0.2);   // half of the 0.4 s blend
    EXPECT_NEAR(maskEntry(e, "Other", "Root"), 0.5f, 1e-4f);
    EXPECT_NEAR(maskEntry(e, "TestAnim", "Root"), 0.5f, 1e-4f);
    EXPECT_NEAR(maskEntry(e, "Other", "Root") + maskEntry(e, "TestAnim", "Root"), 1.0f, 1e-5f)
        << "per-bone weights sum to 1";
    mgr()->tick(0.3);
    EXPECT_FALSE(e->getAnimationState("TestAnim")->getEnabled()) << "the outgoing clip drops out after the blend";
    EXPECT_FLOAT_EQ(maskEntry(e, "Other", "Root"), 1.0f);
}

TEST_F(MotionGraphSceneTest, MaskedTransitionDrivesOnlyTheMaskedBones)
{
    Ogre::Entity* e = makeTwoClipEntity("MG_Mask");
    Graph g = twoStates();
    g.transitions[0].mask = {QStringLiteral("Child")};
    g.transitions[0].duration = 0.0;
    ASSERT_TRUE(mgr()->setGraph(QStringLiteral("MG_Mask"), g, false));
    ASSERT_TRUE(mgr()->play(QStringLiteral("MG_Mask")));
    ASSERT_TRUE(mgr()->setParamValue(QStringLiteral("go"), 1.0));
    mgr()->tick(0.1);
    EXPECT_EQ(mgr()->currentState(), QStringLiteral("b"));
    EXPECT_FLOAT_EQ(maskEntry(e, "Other", "Child"), 1.0f) << "the target drives the mask";
    EXPECT_FLOAT_EQ(maskEntry(e, "Other", "Root"), 0.0f);
    EXPECT_FLOAT_EQ(maskEntry(e, "TestAnim", "Root"), 1.0f) << "the previous clip keeps the rest";
    EXPECT_FLOAT_EQ(maskEntry(e, "TestAnim", "Child"), 0.0f);
}

TEST_F(MotionGraphSceneTest, AuthoringIsUndoable)
{
    makeTwoClipEntity("MG_Undo");
    mgr()->setEntity(QStringLiteral("MG_Undo"));
    UndoManager* um = UndoManager::getSingleton();
    const QString a = mgr()->addState(QStringLiteral("TestAnim"), 10, 10);
    const QString b = mgr()->addState(QStringLiteral("Other"), 200, 10);
    ASSERT_FALSE(a.isEmpty());
    ASSERT_FALSE(b.isEmpty());
    EXPECT_EQ(mgr()->entry(), a) << "the first state becomes the entry";
    const QString t = mgr()->addTransition(a, b);
    ASSERT_FALSE(t.isEmpty());
    ASSERT_TRUE(mgr()->addParam(QStringLiteral("speed"), QStringLiteral("float")));
    ASSERT_TRUE(mgr()->addCondition(t, QStringLiteral("speed"), QStringLiteral(">"), 1.0));
    EXPECT_FALSE(mgr()->removeParam(QStringLiteral("speed"))) << "in use by a condition";
    EXPECT_FALSE(mgr()->addState(QStringLiteral("NoSuchClip"), 0, 0).size());

    // A drag is ONE undo step however many moves it took.
    const int before = um->stack()->index();
    mgr()->moveState(a, 20, 20, false);
    mgr()->moveState(a, 30, 30, false);
    mgr()->moveState(a, 40, 40, true);
    EXPECT_EQ(um->stack()->index(), before + 1);
    um->undo();
    EXPECT_DOUBLE_EQ(mgr()->graph(QStringLiteral("MG_Undo")).state(a)->x, 10.0);

    ASSERT_TRUE(mgr()->renameState(a, QStringLiteral("idle")));
    EXPECT_EQ(mgr()->graph(QStringLiteral("MG_Undo")).transitions[0].from, QStringLiteral("idle"))
        << "renaming rewires the transitions";
    EXPECT_EQ(mgr()->entry(), QStringLiteral("idle"));
    ASSERT_TRUE(mgr()->removeState(b));
    EXPECT_TRUE(mgr()->graph(QStringLiteral("MG_Undo")).transitions.empty()) << "its transitions go with it";
    um->undo();
    EXPECT_EQ(mgr()->graph(QStringLiteral("MG_Undo")).transitions.size(), 1u);
}

TEST_F(MotionGraphSceneTest, MaskFromBoneSubtreeAndTemplate)
{
    makeTwoClipEntity("MG_Sub");
    mgr()->setEntity(QStringLiteral("MG_Sub"));
    ASSERT_TRUE(mgr()->buildLocomotionTemplate());
    const Graph g = mgr()->graph(QStringLiteral("MG_Sub"));
    EXPECT_EQ(g.states.size(), 3u);
    EXPECT_EQ(g.entry, QStringLiteral("idle"));
    ASSERT_TRUE(g.param(QStringLiteral("speed")));
    ASSERT_TRUE(mgr()->setTransitionMaskFromBone(QStringLiteral("t1"), QStringLiteral("Root"), true));
    EXPECT_EQ(mgr()->graph(QStringLiteral("MG_Sub")).transitions[0].mask,
              (QStringList{QStringLiteral("Root"), QStringLiteral("Child")}));
    ASSERT_TRUE(mgr()->setTransitionMaskFromBone(QStringLiteral("t1"), QString(), false));
    EXPECT_TRUE(mgr()->graph(QStringLiteral("MG_Sub")).transitions[0].mask.isEmpty());
    EXPECT_FALSE(mgr()->setTransitionMaskFromBone(QStringLiteral("t1"), QStringLiteral("Ghost"), true));
}

TEST_F(MotionGraphSceneTest, SidecarRoundTripRebindsToTheImportedEntity)
{
    makeTwoClipEntity("MG_Save");
    ASSERT_TRUE(mgr()->setGraph(QStringLiteral("MG_Save"), twoStates(), false));
    QTemporaryDir dir;
    const QString asset = dir.filePath(QStringLiteral("hero.glb"));
    ASSERT_TRUE(mgr()->writeSidecar(asset, {QStringLiteral("MG_Save")}));
    mgr()->clear();
    QString err;
    ASSERT_EQ(mgr()->loadSidecar(asset, {}, QStringLiteral("MG_Renamed"), &err), 1) << err.toStdString();
    EXPECT_TRUE(mgr()->hasGraph(QStringLiteral("MG_Renamed")));
    EXPECT_EQ(mgr()->graph(QStringLiteral("MG_Renamed")).transitions.size(), 1u);

    // Nothing to write → a stale file is removed.
    mgr()->clear();
    EXPECT_FALSE(mgr()->writeSidecar(asset));
    EXPECT_FALSE(QFile::exists(MotionGraphManager::sidecarPath(asset)));
}

TEST_F(MotionGraphSceneTest, ReplacingTheSceneStopsAndDropsGraphs)
{
    makeTwoClipEntity("MG_Clear");
    ASSERT_TRUE(mgr()->setGraph(QStringLiteral("MG_Clear"), twoStates(), false));
    ASSERT_TRUE(mgr()->play(QStringLiteral("MG_Clear")));
    emit Manager::getSingleton()->sceneClearing();
    EXPECT_FALSE(mgr()->playing());
    EXPECT_FALSE(mgr()->hasGraph(QStringLiteral("MG_Clear")));
}

TEST_F(MotionGraphSceneTest, PlayRefusesAStateWhoseClipIsMissing)
{
    makeTwoClipEntity("MG_Missing");
    Graph g = twoStates();
    g.states[1].clip = QStringLiteral("Gone");
    ASSERT_TRUE(mgr()->setGraph(QStringLiteral("MG_Missing"), g, false));
    QString err;
    EXPECT_FALSE(mgr()->play(QStringLiteral("MG_Missing"), &err));
    EXPECT_TRUE(err.contains(QStringLiteral("Gone"))) << err.toStdString();
}

TEST_F(MotionGraphSceneTest, PanelQmlLoadsWithoutErrors)
{
    qmlRegisterSingletonType<AnimationControlController>("AnimationControl", 1, 0, "AnimationControlController",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return AnimationControlController::qmlInstance(e, s); });
    qmlRegisterSingletonType<MotionGraphManager>("PropertiesPanel", 1, 0, "MotionGraphManager",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return MotionGraphManager::qmlInstance(e, s); });
    qmlRegisterSingletonType<PropertiesPanelController>("PropertiesPanel", 1, 0, "PropertiesPanelController",
        [](QQmlEngine* e, QJSEngine*) -> QObject* { return PropertiesPanelController::qmlInstance(e, nullptr); });
    qmlRegisterSingletonType<ThemeManager>("ThemeManager", 1, 0, "ThemeManager",
        [](QQmlEngine* e, QJSEngine* s) -> QObject* { return ThemeManager::qmlInstance(e, s); });
    makeTwoClipEntity("MG_Qml");
    ASSERT_TRUE(mgr()->setGraph(QStringLiteral("MG_Qml"), twoStates(), false));
    mgr()->setEntity(QStringLiteral("MG_Qml"));
    QQmlEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/"));
    QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/AnimationControl/MotionGraphPanel.qml")));
    while (component.isLoading()) QCoreApplication::processEvents();
    ASSERT_FALSE(component.isError()) << component.errorString().toStdString();
    std::unique_ptr<QObject> obj(component.create());
    ASSERT_NE(obj, nullptr) << component.errorString().toStdString();
}
