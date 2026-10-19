// "Fit DNA body" action (epic #1095, phase 4): selection gating and the undoable commit. Uses the synthetic
// DNA (no Epic asset) and needs GL like the other scene tests.

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include "DnaFitController.h"
#include "DnaReader.h"
#include "DnaScene.h"
#include "DnaTestFixtures.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "UndoManager.h"
#include "commands/AddFittedDnaCharacterCommand.h"

#include <OgreAny.h>
#include <OgreEntity.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

using namespace Dna;

namespace {
QString writeSynthetic(QTemporaryDir& dir)
{
    const QString p = dir.filePath("synthetic.dna");
    const auto bytes = DnaTest::makeSyntheticDna();
    QFile f(p);
    f.open(QIODevice::WriteOnly);
    f.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size()));
    return p;
}
}  // namespace

class DnaFitControllerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        SelectionSet::getSingleton()->clearList();
    }
    void TearDown() override
    {
        SelectionSet::getSingleton()->clearList();
        if (auto* m = Manager::getSingletonPtr())
            if (auto* scene = m->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
    }
    static Ogre::Entity* entityOf(Ogre::SceneNode* n)
    {
        for (auto it = n->getAttachedObjectIterator(); it.hasMoreElements();) {
            Ogre::MovableObject* mo = it.getNext();
            if (mo->getMovableType() == "Entity") return static_cast<Ogre::Entity*>(mo);
        }
        return nullptr;
    }
};

TEST_F(DnaFitControllerTest, ImportedDnaEntitiesRememberTheirSourceFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeSynthetic(dir);
    QString err;
    Ogre::SceneNode* node = importToScene(path, ReadOptions(), nullptr, &err);
    ASSERT_NE(node, nullptr) << err.toStdString();
    Ogre::Entity* e = entityOf(node);
    ASSERT_NE(e, nullptr);
    const Ogre::Any a = e->getUserObjectBindings().getUserAny(kSourcePathKey);
    ASSERT_FALSE(a.isEmpty());
    EXPECT_EQ(QString::fromStdString(Ogre::any_cast<std::string>(a)), QFileInfo(path).absoluteFilePath());
}

TEST_F(DnaFitControllerTest, GatesOnTheSelectionAndSaysWhy)
{
    auto* c = DnaFitController::instance();
    c->refresh();
    EXPECT_FALSE(c->canFit());
    EXPECT_FALSE(c->hasSelection());
    EXPECT_TRUE(c->hint().contains("Import a MetaHuman"));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Ogre::SceneNode* node = importToScene(writeSynthetic(dir), ReadOptions(), nullptr, nullptr);
    ASSERT_NE(node, nullptr);
    c->refresh();
    EXPECT_TRUE(c->hasSelection());          // a DNA character exists
    SelectionSet::getSingleton()->clearList();
    c->refresh();
    EXPECT_FALSE(c->canFit());
    EXPECT_TRUE(c->hint().contains("Select the MetaHuman body"));
    SelectionSet::getSingleton()->selectOne(entityOf(node));
    c->refresh();
    EXPECT_FALSE(c->canFit());               // only the DNA is selected: a target is missing
    EXPECT_TRUE(c->hint().contains("Also select"));
    EXPECT_FALSE(c->fitSelected());          // and fitSelected refuses with a message instead of running
    EXPECT_FALSE(c->status().isEmpty());
}

TEST_F(DnaFitControllerTest, ANodeSelectionCountsLikeAnEntitySelection)
{
    // Clicking a mesh in the viewport / scene tree selects its NODE; the action must see that too.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Ogre::SceneNode* dnaNode = importToScene(writeSynthetic(dir), ReadOptions(), nullptr, nullptr);
    ASSERT_NE(dnaNode, nullptr);
    Ogre::SceneNode* other = importToScene(writeSynthetic(dir), ReadOptions(), nullptr, nullptr);   // a second character
    ASSERT_NE(other, nullptr);
    SelectionSet::getSingleton()->clearList();
    SelectionSet::getSingleton()->append(dnaNode);
    auto* c = DnaFitController::instance();
    c->refresh();
    EXPECT_TRUE(c->hint().contains("Also select")) << c->hint().toStdString();   // one node = one entity, not zero
    EXPECT_FALSE(c->canFit());
    SelectionSet::getSingleton()->append(other);   // two NODES selected: body + target
    c->refresh();
    EXPECT_TRUE(c->canFit()) << c->hint().toStdString();
}

TEST_F(DnaFitControllerTest, CommitAddsANewNodeAndUndoRemovesItWithoutTouchingTheOriginal)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeSynthetic(dir);
    const ReadResult rr = read(path);
    ASSERT_TRUE(rr.ok) << rr.error.toStdString();
    auto rig = std::make_shared<Rig>(rr.rig);
    const size_t before = Manager::getSingleton()->getSceneNodes().size();
    auto* cmd = new AddFittedDnaCharacterCommand(rig, QString(), "fitted_test");
    UndoManager::getSingleton()->push(cmd);
    ASSERT_TRUE(cmd->created()) << cmd->error().toStdString();
    EXPECT_EQ(Manager::getSingleton()->getSceneNodes().size(), before + 1);
    const QString name = cmd->nodeName();
    EXPECT_FALSE(name.isEmpty());

    UndoManager::getSingleton()->undo();
    EXPECT_EQ(Manager::getSingleton()->getSceneNodes().size(), before);
    UndoManager::getSingleton()->redo();
    EXPECT_EQ(Manager::getSingleton()->getSceneNodes().size(), before + 1);
    UndoManager::getSingleton()->undo();
    EXPECT_EQ(Manager::getSingleton()->getSceneNodes().size(), before);
}
