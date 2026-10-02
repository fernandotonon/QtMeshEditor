// #524 — MCP generator tools: argument validation and a live round trip.

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#define private public
#include "MCPServer.h"
#undef private

#include "AnimGeneratorManager.h"
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

class MCPServerGeneratorsCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        AnimGeneratorManager::instance()->clear();
        server = std::make_unique<MCPServer>();
    }
    void TearDown() override
    {
        AnimGeneratorManager::instance()->clear();
        if (auto* m = Manager::getSingletonPtr())
            if (auto* scene = m->getSceneMgr()) {
                try { scene->destroyAllEntities(); } catch (...) {}
                try { scene->getRootSceneNode()->removeAndDestroyAllChildren(); } catch (...) {}
            }
    }
    std::unique_ptr<MCPServer> server;
};

TEST_F(MCPServerGeneratorsCoverageTest, RejectsBadArguments)
{
    EXPECT_TRUE(server->toolAddGenerator({{"type", "wobble"}, {"target", "node:N/position.y"}})["isError"].toBool());
    const auto badTarget = server->toolAddGenerator({{"type", "sine"}, {"target", "node:N"}});
    EXPECT_TRUE(badTarget["isError"].toBool());
    EXPECT_TRUE(server->toolAddGenerator({{"type", "sine"}, {"target", "node:N/position.y"},
                                          {"params", QJsonObject{{"wobble", 1}}}})["isError"].toBool());
    EXPECT_TRUE(server->toolAddGenerator({{"type", "sine"}, {"target", "node:N/position.y"},
                                          {"enabled", "true"}})["isError"].toBool())
        << "a string 'enabled' must be refused, not read as false";
    EXPECT_TRUE(server->toolSetGenerator({{"id", "nope"}})["isError"].toBool());
    EXPECT_TRUE(server->toolBakeGenerator({{"id", "nope"}})["isError"].toBool());
    EXPECT_TRUE(server->toolRemoveGenerator({{"id", "nope"}})["isError"].toBool());
    const auto list = payload(server->toolListGenerators({}));
    EXPECT_EQ(list["count"].toInt(), 0);
    EXPECT_TRUE(list["types"].toArray().contains(QJsonValue("follow-path")));
}

TEST_F(MCPServerGeneratorsCoverageTest, AddEditBakeRemoveRoundTrip)
{
    Ogre::Entity* e = createAnimatedTestEntity("MCP_Gen");
    ASSERT_NE(e, nullptr);
    const auto add = server->toolAddGenerator({{"type", "sine"},
                                               {"target", "bone:MCP_Gen/Child/position.y@TestAnim"},
                                               {"params", QJsonObject{{"amplitude", 0.25}, {"frequency", 2}}}});
    ASSERT_FALSE(add["isError"].toBool()) << textOf(add).toStdString();
    const QString id = payload(add)["id"].toString();
    ASSERT_FALSE(id.isEmpty());
    EXPECT_TRUE(payload(add)["generator"].toObject()["bound"].toBool());

    // params + enabled in one request; a non-bool enabled is refused.
    EXPECT_TRUE(server->toolSetGenerator({{"id", id}, {"enabled", 1}})["isError"].toBool());
    const auto set = server->toolSetGenerator({{"id", id}, {"params", QJsonObject{{"amplitude", 0.5}}}, {"enabled", false}});
    ASSERT_FALSE(set["isError"].toBool()) << textOf(set).toStdString();
    EXPECT_DOUBLE_EQ(payload(set)["generator"].toObject()["amplitude"].toDouble(), 0.5);
    EXPECT_FALSE(payload(set)["generator"].toObject()["enabled"].toBool());

    ASSERT_FALSE(server->toolSetGenerator({{"id", id}, {"enabled", true}})["isError"].toBool());
    const auto bake = server->toolBakeGenerator({{"id", id}});
    ASSERT_FALSE(bake["isError"].toBool()) << textOf(bake).toStdString();
    EXPECT_TRUE(payload(bake)["generator"].toObject()["baked"].toBool());
    EXPECT_TRUE(server->toolSetGenerator({{"id", id}, {"enabled", true}})["isError"].toBool())
        << "a baked generator cannot be re-enabled";
    EXPECT_EQ(payload(server->toolListGenerators({}))["count"].toInt(), 1);
    ASSERT_FALSE(server->toolRemoveGenerator({{"id", id}})["isError"].toBool());
    EXPECT_EQ(payload(server->toolListGenerators({}))["count"].toInt(), 0);
}
