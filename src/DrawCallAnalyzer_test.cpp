#include <gtest/gtest.h>

#include "DrawCallAnalyzer.h"

#include <QJsonArray>
#include <QJsonObject>

// All tests here exercise the pure-data path. They never touch Ogre, so they
// run on every CI build regardless of whether tryInitOgre() succeeded.

namespace {
// Build a synthetic cluster directly to test buildSuggestions /
// toJson / toText without standing up an Ogre Entity. analyze() itself is
// covered by integration tests in a separate Ogre-backed suite.
MaterialCluster makeCluster(const QString& name, int submeshes,
                            const QStringList& entities)
{
    MaterialCluster c;
    c.materialName = name;
    c.submeshCount = submeshes;
    c.entityNames = entities;
    return c;
}
} // namespace

TEST(DrawCallAnalyzerTest, BuildSuggestionsFiltersSingletons)
{
    QList<MaterialCluster> clusters;
    clusters << makeCluster("Mat.A", 1, {"E1"})           // 1 entity → skipped
             << makeCluster("Mat.B", 3, {"E2","E3","E4"}) // 3 entities → kept
             << makeCluster("Mat.C", 2, {"E5","E6"});     // 2 entities → kept
    auto out = DrawCallAnalyzer::buildSuggestions(clusters);
    ASSERT_EQ(2, out.size());
    EXPECT_EQ(QString("Mat.B"), out[0].materialName);
    EXPECT_EQ(2, out[0].estimatedSavings);  // 3 entities → save 2 calls
    EXPECT_EQ(QString("Mat.C"), out[1].materialName);
    EXPECT_EQ(1, out[1].estimatedSavings);
}

TEST(DrawCallAnalyzerTest, BuildSuggestionsRespectsCustomThreshold)
{
    QList<MaterialCluster> clusters;
    clusters << makeCluster("Mat.A", 1, {"E1"})
             << makeCluster("Mat.B", 3, {"E2","E3","E4"});
    // Threshold 4 → nothing qualifies
    auto out = DrawCallAnalyzer::buildSuggestions(clusters, 4);
    EXPECT_TRUE(out.isEmpty());
}

TEST(DrawCallAnalyzerTest, BuildSuggestionsEmptyClusters)
{
    EXPECT_TRUE(DrawCallAnalyzer::buildSuggestions({}).isEmpty());
}

TEST(DrawCallAnalyzerTest, MergeSavingsArithmetic)
{
    MaterialCluster empty;
    EXPECT_EQ(0, empty.mergeSavings());

    MaterialCluster single = makeCluster("M", 1, {"E1"});
    EXPECT_EQ(0, single.mergeSavings());

    MaterialCluster five = makeCluster("M", 5, {"E1","E2","E3","E4","E5"});
    EXPECT_EQ(4, five.mergeSavings());
}

TEST(DrawCallAnalyzerTest, JsonEmptyReport)
{
    DrawCallReport empty;
    QJsonObject obj = DrawCallAnalyzer::toJson(empty);
    EXPECT_TRUE(obj.contains("totals"));
    EXPECT_TRUE(obj.contains("clusters"));
    EXPECT_TRUE(obj.contains("suggestions"));
    EXPECT_EQ(0, obj["totals"].toObject()["entities"].toInt());
    EXPECT_EQ(0, obj["clusters"].toArray().size());
    EXPECT_EQ(0, obj["suggestions"].toArray().size());
}

TEST(DrawCallAnalyzerTest, JsonShapeWithSuggestion)
{
    DrawCallReport report;
    report.totalEntities = 4;
    report.totalSubmeshes = 4;
    report.totalDrawCalls = 4;
    report.uniqueMaterials = 2;
    report.totalSavings = 2;
    report.potentialDrawCalls = 2;
    report.clusters << makeCluster("Mat.A", 3, {"E1","E2","E3"})
                    << makeCluster("Mat.B", 1, {"E4"});
    report.suggestions = DrawCallAnalyzer::buildSuggestions(report.clusters);

    QJsonObject obj = DrawCallAnalyzer::toJson(report);
    EXPECT_EQ(4, obj["totals"].toObject()["drawCalls"].toInt());
    EXPECT_EQ(2, obj["totals"].toObject()["totalSavings"].toInt());
    EXPECT_EQ(2, obj["clusters"].toArray().size());
    EXPECT_EQ(1, obj["suggestions"].toArray().size());

    QJsonObject sug = obj["suggestions"].toArray()[0].toObject();
    EXPECT_EQ(QString("Mat.A"), sug["material"].toString());
    EXPECT_EQ(2, sug["estimatedSavings"].toInt());
    EXPECT_EQ(3, sug["entities"].toArray().size());
}

TEST(DrawCallAnalyzerTest, TextHasHeaderAndMaterials)
{
    DrawCallReport report;
    report.totalEntities = 2;
    report.totalSubmeshes = 2;
    report.totalDrawCalls = 2;
    report.uniqueMaterials = 1;
    report.clusters << makeCluster("Mat.A", 2, {"E1","E2"});
    report.suggestions = DrawCallAnalyzer::buildSuggestions(report.clusters);
    report.totalSavings = 1;
    report.potentialDrawCalls = 1;

    QString text = DrawCallAnalyzer::toText(report);
    EXPECT_TRUE(text.contains("Draw Call Analysis"));
    EXPECT_TRUE(text.contains("Mat.A"));
    EXPECT_TRUE(text.contains("Merge suggestions"));
    EXPECT_TRUE(text.contains("E1"));
    EXPECT_TRUE(text.contains("E2"));
}

TEST(DrawCallAnalyzerTest, TextHandlesNoMergeOpportunities)
{
    DrawCallReport report;
    report.totalEntities = 2;
    report.totalSubmeshes = 2;
    report.totalDrawCalls = 2;
    report.uniqueMaterials = 2;
    report.clusters << makeCluster("Mat.A", 1, {"E1"})
                    << makeCluster("Mat.B", 1, {"E2"});
    // No suggestions because each cluster has only 1 entity
    QString text = DrawCallAnalyzer::toText(report);
    EXPECT_TRUE(text.contains("No merge opportunities"));
}

TEST(DrawCallAnalyzerTest, TextHandlesEmpty)
{
    DrawCallReport empty;
    QString text = DrawCallAnalyzer::toText(empty);
    EXPECT_TRUE(text.contains("Draw Call Analysis"));
    EXPECT_FALSE(text.contains("Merge suggestions"));
}

TEST(DrawCallAnalyzerTest, AnalyzeNullEntityList)
{
    QList<Ogre::Entity*> empty;
    DrawCallReport report = DrawCallAnalyzer::analyze(empty);
    EXPECT_EQ(0, report.totalEntities);
    EXPECT_EQ(0, report.totalDrawCalls);
    EXPECT_TRUE(report.suggestions.isEmpty());
}

TEST(DrawCallAnalyzerTest, AnalyzeListOfNullPointers)
{
    QList<Ogre::Entity*> nulls;
    nulls << nullptr << nullptr;
    DrawCallReport report = DrawCallAnalyzer::analyze(nulls);
    EXPECT_EQ(0, report.totalEntities);  // nulls skipped
    EXPECT_EQ(0, report.totalDrawCalls);
}
