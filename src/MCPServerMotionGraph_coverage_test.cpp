// #526 — MCP motion-graph tools: argument validation and a live round trip.

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#define private public
#include "MCPServer.h"
#undef private

#include "MotionGraphManager.h"
#include "TestHelpers.h"

#include <OgreAnimation.h>
#include <OgreEntity.h>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include <OgreSkeletonInstance.h>

namespace {
QString textOf(const QJsonObject& r)
{
    const QJsonArray c = r["content"].toArray();
    return c.isEmpty() ? QString() : c[0].toObject()["text"].toString();
}
QJsonObject payload(const QJsonObject& r) { return QJsonDocument::fromJson(textOf(r).toUtf8()).object(); }

QJsonObject graphJson()
{
    return QJsonObject{
        {"entry", "a"},
        {"states", QJsonArray{QJsonObject{{"name", "a"}, {"clip", "TestAnim"}}, QJsonObject{{"name", "b"}, {"clip", "Other"}}}},
        {"params", QJsonArray{QJsonObject{{"name", "go"}, {"type", "bool"}, {"value", 0}}}},
        {"transitions", QJsonArray{QJsonObject{{"id", "t1"}, {"from", "a"}, {"to", "b"}, {"duration", 0},
                                               {"conditions", QJsonArray{QJsonObject{{"param", "go"}, {"op", "true"}, {"value", 0}}}}}}}};
}
}  // namespace

class MCPServerMotionGraphCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        MotionGraphManager::instance()->clear();
        server = std::make_unique<MCPServer>();
    }
    void TearDown() override
    {
        MotionGraphManager::instance()->clear();
        if (auto* m = Manager::getSingletonPtr())
            if (auto* scene = m->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
    }
    std::unique_ptr<MCPServer> server;
};

TEST_F(MCPServerMotionGraphCoverageTest, RejectsBadArguments)
{
    EXPECT_TRUE(server->toolSetMotionGraph({{"entity", "Nope"}, {"graph", graphJson()}})["isError"].toBool());
    EXPECT_TRUE(server->toolPlayMotionGraph({{"entity", "Nope"}})["isError"].toBool());
    EXPECT_TRUE(server->toolSetMotionGraphParam({{"name", "x"}, {"value", "fast"}})["isError"].toBool());
    EXPECT_FALSE(payload(server->toolGetMotionGraph({{"entity", "Nope"}}))["has_graph"].toBool());
}

TEST_F(MCPServerMotionGraphCoverageTest, SetPlayParamStopRoundTrip)
{
    Ogre::Entity* e = createAnimatedTestEntity("MCP_Graph");
    Ogre::Animation* other = e->getSkeleton()->createAnimation("Other", 1.0f);
    other->createNodeTrack(0, e->getSkeleton()->getBone("Root"))->createNodeKeyFrame(0.0f);
    e->refreshAvailableAnimationState();

    QJsonObject bad = graphJson();
    QJsonArray states = bad["states"].toArray();
    QJsonObject s0 = states[0].toObject();
    s0["clip"] = "Missing";
    states[0] = s0;
    bad["states"] = states;
    EXPECT_TRUE(server->toolSetMotionGraph({{"entity", "MCP_Graph"}, {"graph", bad}})["isError"].toBool())
        << "every state's clip must exist";

    const auto set = server->toolSetMotionGraph({{"entity", "MCP_Graph"}, {"graph", graphJson()}});
    ASSERT_FALSE(set["isError"].toBool()) << textOf(set).toStdString();
    const auto play = server->toolPlayMotionGraph({{"entity", "MCP_Graph"}});
    ASSERT_FALSE(play["isError"].toBool()) << textOf(play).toStdString();
    EXPECT_EQ(payload(play)["current_state"].toString(), QStringLiteral("a"));
    ASSERT_FALSE(server->toolSetMotionGraphParam({{"name", "go"}, {"value", true}})["isError"].toBool());
    MotionGraphManager::instance()->tick(0.05);
    EXPECT_EQ(payload(server->toolGetMotionGraph({{"entity", "MCP_Graph"}}))["current_state"].toString(), QStringLiteral("b"));
    const auto stop = server->toolStopMotionGraph({});
    EXPECT_FALSE(payload(stop)["playing"].toBool());
}
