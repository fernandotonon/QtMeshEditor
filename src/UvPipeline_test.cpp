#include <gtest/gtest.h>

#include "UvPipeline.h"

TEST(UvPipelineParseProjectMode, AcceptsKnownModes)
{
    bool ok = false;
    EXPECT_EQ(UvPipeline::parseProjectMode(QStringLiteral("box"), &ok), UvProject::Mode::Box);
    EXPECT_TRUE(ok);

    EXPECT_EQ(UvPipeline::parseProjectMode(QStringLiteral("  CYLINDER  "), &ok),
              UvProject::Mode::Cylinder);
    EXPECT_TRUE(ok);

    EXPECT_EQ(UvPipeline::parseProjectMode(QStringLiteral("cyl"), &ok),
              UvProject::Mode::Cylinder);
    EXPECT_TRUE(ok);

    EXPECT_EQ(UvPipeline::parseProjectMode(QStringLiteral("sphere"), &ok),
              UvProject::Mode::Sphere);
    EXPECT_TRUE(ok);

    EXPECT_EQ(UvPipeline::parseProjectMode(QStringLiteral("sph"), &ok),
              UvProject::Mode::Sphere);
    EXPECT_TRUE(ok);

    EXPECT_EQ(UvPipeline::parseProjectMode(QStringLiteral("reset"), &ok),
              UvProject::Mode::ResetBox);
    EXPECT_TRUE(ok);

    EXPECT_EQ(UvPipeline::parseProjectMode(QStringLiteral("reset_box"), &ok),
              UvProject::Mode::ResetBox);
    EXPECT_TRUE(ok);

    EXPECT_EQ(UvPipeline::parseProjectMode(QStringLiteral("resetbox"), &ok),
              UvProject::Mode::ResetBox);
    EXPECT_TRUE(ok);
}

TEST(UvPipelineParseProjectMode, RejectsUnknownMode)
{
    bool ok = true;
    UvPipeline::parseProjectMode(QStringLiteral("view"), &ok);
    EXPECT_FALSE(ok);
}

TEST(UvPipelineParseSeamEdgeList, ParsesSubmeshPrefixedEdges)
{
    std::vector<UvPipeline::SeamEdge> edges;
    QString error;
    ASSERT_TRUE(UvPipeline::parseSeamEdgeList(QStringLiteral("0:1-2,1:10-11"), edges, &error));
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0].subMeshIndex, 0);
    EXPECT_EQ(edges[0].vertA, 1u);
    EXPECT_EQ(edges[0].vertB, 2u);
    EXPECT_EQ(edges[1].subMeshIndex, 1);
    EXPECT_EQ(edges[1].vertA, 10u);
    EXPECT_EQ(edges[1].vertB, 11u);
}

TEST(UvPipelineParseSeamEdgeList, RejectsEmptySpec)
{
    std::vector<UvPipeline::SeamEdge> edges;
    QString error;
    EXPECT_FALSE(UvPipeline::parseSeamEdgeList(QString(), edges, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(UvPipelineParseSeamEdgeList, RejectsMalformedToken)
{
    std::vector<UvPipeline::SeamEdge> edges;
    QString error;
    EXPECT_FALSE(UvPipeline::parseSeamEdgeList(QStringLiteral("0:1"), edges, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(UvPipelineInfoReport, JsonAndTextIncludeIslandsAndOverlap)
{
    UvPipeline::InfoReport report;
    report.islandCount = 3;
    report.overlappingUvsRatio = 0.125;

    const QJsonObject json = UvPipeline::infoToJson(QStringLiteral("mesh.fbx"), report);
    EXPECT_EQ(json.value(QStringLiteral("islandCount")).toInt(), 3);
    EXPECT_DOUBLE_EQ(json.value(QStringLiteral("overlappingUvsRatio")).toDouble(), 0.125);
    EXPECT_DOUBLE_EQ(json.value(QStringLiteral("overlappingUvsPercent")).toDouble(), 12.5);

    const QString text = UvPipeline::infoToText(QStringLiteral("mesh.fbx"), report);
    EXPECT_TRUE(text.contains(QStringLiteral("Islands:")));
    EXPECT_TRUE(text.contains(QStringLiteral("12.5")));
}
