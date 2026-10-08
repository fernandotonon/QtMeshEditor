// #525 — MCP constraint tools: argument validation and a live round trip.

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#define private public
#include "MCPServer.h"
#undef private

#include "ConstraintManager.h"
#include "TestHelpers.h"

#include <OgreEntity.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>

namespace {
QString textOf(const QJsonObject& r)
{
    const QJsonArray c = r["content"].toArray();
    return c.isEmpty() ? QString() : c[0].toObject()["text"].toString();
}
QJsonObject payload(const QJsonObject& r) { return QJsonDocument::fromJson(textOf(r).toUtf8()).object(); }
}  // namespace

class MCPServerConstraintsCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        ConstraintManager::instance()->clear();
        server = std::make_unique<MCPServer>();
    }
    void TearDown() override
    {
        ConstraintManager::instance()->clear();
        if (auto* m = Manager::getSingletonPtr())
            if (auto* scene = m->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
    }
    std::unique_ptr<MCPServer> server;
};

TEST_F(MCPServerConstraintsCoverageTest, RejectsBadArguments)
{
    EXPECT_TRUE(server->toolAddConstraint({{"type", "wobble"}, {"owner", "node:N"}})["isError"].toBool());
    EXPECT_TRUE(server->toolAddConstraint({{"type", "look-at"}, {"owner", "nonsense"}})["isError"].toBool());
    EXPECT_TRUE(server->toolAddConstraint({{"type", "look-at"}, {"owner", "node:N"}, {"target", "node:T"},
                                           {"params", QJsonObject{{"wobble", 1}}}})["isError"].toBool());
    EXPECT_TRUE(server->toolAddConstraint({{"type", "look-at"}, {"owner", "node:N"}, {"target", "node:T"},
                                           {"enabled", "true"}})["isError"].toBool());
    EXPECT_TRUE(server->toolSetConstraint({{"id", "nope"}})["isError"].toBool());
    EXPECT_TRUE(server->toolMoveConstraint({{"id", "nope"}, {"direction", "sideways"}})["isError"].toBool());
    EXPECT_TRUE(server->toolRemoveConstraint({{"id", "nope"}})["isError"].toBool());
    EXPECT_TRUE(server->toolBakeConstraints({{"fps", 0}})["isError"].toBool());
    EXPECT_TRUE(server->toolBakeConstraints({})["isError"].toBool()) << "nothing to bake";
    const auto list = payload(server->toolListConstraints({}));
    EXPECT_EQ(list["count"].toInt(), 0);
    EXPECT_TRUE(list["types"].toArray().contains(QJsonValue("ik")));
}

TEST_F(MCPServerConstraintsCoverageTest, AddEditMoveBakeRemoveRoundTrip)
{
    Ogre::SceneNode* owner = Manager::getSingleton()->addSceneNode(QStringLiteral("MCP_ConOwner"));
    Ogre::SceneNode* a = Manager::getSingleton()->addSceneNode(QStringLiteral("MCP_ConA"));
    Ogre::SceneNode* b = Manager::getSingleton()->addSceneNode(QStringLiteral("MCP_ConB"));
    a->setPosition(1, 0, 0);
    b->setPosition(0, 2, 0);
    const QString o = "node:" + QString::fromStdString(owner->getName());
    const auto add1 = server->toolAddConstraint({{"type", "copy-position"}, {"owner", o},
                                                 {"target", "node:" + QString::fromStdString(a->getName())}});
    ASSERT_FALSE(add1["isError"].toBool()) << textOf(add1).toStdString();
    const auto add2 = server->toolAddConstraint({{"type", "look-at"}, {"owner", o},
                                                 {"target", "node:" + QString::fromStdString(b->getName())},
                                                 {"params", QJsonObject{{"aim", "-z"}, {"influence", 0.5}}}});
    ASSERT_FALSE(add2["isError"].toBool()) << textOf(add2).toStdString();
    const QString id2 = payload(add2)["id"].toString();
    EXPECT_EQ(payload(add2)["constraint"].toObject()["aim"].toString(), QStringLiteral("-z"));

    const auto set = server->toolSetConstraint({{"id", id2}, {"enabled", false}});
    ASSERT_FALSE(set["isError"].toBool());
    EXPECT_FALSE(payload(set)["constraint"].toObject()["enabled"].toBool());
    ASSERT_FALSE(server->toolSetConstraint({{"id", id2}, {"enabled", true}})["isError"].toBool());

    const auto mv = server->toolMoveConstraint({{"id", id2}, {"direction", "down"}});
    ASSERT_FALSE(mv["isError"].toBool()) << textOf(mv).toStdString();
    EXPECT_EQ(payload(mv)["constraints"].toArray().last().toObject()["id"].toString(), id2);

    const auto bake = server->toolBakeConstraints({{"fps", 10}});
    ASSERT_FALSE(bake["isError"].toBool()) << textOf(bake).toStdString();
    EXPECT_EQ(payload(bake)["active"].toInt(), 0);

    ASSERT_FALSE(server->toolRemoveConstraint({{"id", id2}})["isError"].toBool());
    EXPECT_EQ(payload(server->toolListConstraints({}))["count"].toInt(), 1);
}
