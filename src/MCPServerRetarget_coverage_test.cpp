// #523 — MCP retarget_animation argument validation (no scene needed).

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>

#define private public
#include "MCPServer.h"
#undef private

#include "TestHelpers.h"
#include "UndoManager.h"

#include <OgreEntity.h>
#include <OgreSceneManager.h>
#include <OgreSkeletonInstance.h>

namespace {
QString textOf(const QJsonObject& r)
{
    const QJsonArray c = r["content"].toArray();
    return c.isEmpty() ? QString() : c[0].toObject()["text"].toString();
}
}  // namespace

class MCPServerRetargetCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        server = std::make_unique<MCPServer>();
    }
    std::unique_ptr<MCPServer> server;
};

TEST_F(MCPServerRetargetCoverageTest, RejectsBadOptionsBeforeTouchingTheScene)
{
    EXPECT_TRUE(server->toolRetargetAnimation({{"translation", "some"}})["isError"].toBool());
    EXPECT_TRUE(server->toolRetargetAnimation({{"source_rest", "x"}})["isError"].toBool());
    EXPECT_TRUE(server->toolRetargetAnimation({{"fps", 0}})["isError"].toBool());
    const auto bm = server->toolRetargetAnimation({{"bonemap", "no_such_map"}});
    EXPECT_TRUE(bm["isError"].toBool());
    EXPECT_TRUE(textOf(bm).contains("bundled")) << textOf(bm).toStdString();
    QJsonArray bad; bad.append(QJsonObject{{"source", "a"}});
    EXPECT_TRUE(server->toolRetargetAnimation({{"pairs", bad}})["isError"].toBool());
}

TEST_F(MCPServerRetargetCoverageTest, RequiresSourceAndTarget)
{
    const auto r = server->toolRetargetAnimation({});
    EXPECT_TRUE(r["isError"].toBool());
    EXPECT_TRUE(textOf(r).contains("source_entity")) << textOf(r).toStdString();
    const auto t = server->toolRetargetAnimation({{"source_file", "/nonexistent/x.fbx"}});
    EXPECT_TRUE(textOf(t).contains("not found")) << textOf(t).toStdString();
    const auto e = server->toolRetargetAnimation({{"source_entity", "nope"}});
    EXPECT_TRUE(textOf(e).contains("no entity")) << textOf(e).toStdString();
}

TEST_F(MCPServerRetargetCoverageTest, InSceneRetargetIsUndoableAndDryRunReturnsTheMap)
{
    createAnimatedTestEntity("MCP_RT_Src");
    createAnimatedTestEntity("MCP_RT_Tgt");
    const auto dry = server->toolRetargetAnimation(
        {{"source_entity", "MCP_RT_Src"}, {"target_entity", "MCP_RT_Tgt"}, {"dry_run", true}});
    ASSERT_FALSE(dry["isError"].toBool()) << textOf(dry).toStdString();
    EXPECT_TRUE(textOf(dry).contains("\"Child\"")) << textOf(dry).toStdString();

    const auto r = server->toolRetargetAnimation(
        {{"source_entity", "MCP_RT_Src"}, {"target_entity", "MCP_RT_Tgt"},
         {"animation", "TestAnim"}, {"new_name", "ViaMcp"}, {"translation", "none"}});
    ASSERT_FALSE(r["isError"].toBool()) << textOf(r).toStdString();
    EXPECT_TRUE(textOf(r).contains("ViaMcp"));
    auto* tgt = Manager::getSingleton()->getSceneMgr()->getEntity("MCP_RT_Tgt");
    EXPECT_TRUE(tgt->getSkeleton()->hasAnimation("ViaMcp"));
    UndoManager::getSingleton()->undo();
    EXPECT_FALSE(tgt->getSkeleton()->hasAnimation("ViaMcp"));

    const auto noAnim = server->toolRetargetAnimation(
        {{"source_entity", "MCP_RT_Src"}, {"target_entity", "MCP_RT_Tgt"}, {"animation", "Nope"}});
    EXPECT_TRUE(textOf(noAnim).contains("TestAnim")) << "the error lists the source's clips";
}
