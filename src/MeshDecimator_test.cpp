#include <gtest/gtest.h>

#include "MeshDecimator.h"

#include <QJsonArray>
#include <QJsonObject>
#include <limits>

// Pure-data tests — no Ogre needed.

TEST(MeshDecimatorTest, ReductionFromTargetTrisEmpty)
{
    EXPECT_DOUBLE_EQ(0.0, MeshDecimator::reductionFromTargetTris(0, 100));
    EXPECT_DOUBLE_EQ(0.0, MeshDecimator::reductionFromTargetTris(-5, 100));
}

TEST(MeshDecimatorTest, ReductionFromTargetTrisNoChange)
{
    // target >= current → no reduction
    EXPECT_DOUBLE_EQ(0.0, MeshDecimator::reductionFromTargetTris(100, 100));
    EXPECT_DOUBLE_EQ(0.0, MeshDecimator::reductionFromTargetTris(100, 200));
}

TEST(MeshDecimatorTest, ReductionFromTargetTrisHalving)
{
    // 10000 → 5000 = 50% reduction
    EXPECT_NEAR(0.5, MeshDecimator::reductionFromTargetTris(10000, 5000), 1e-9);
}

TEST(MeshDecimatorTest, ReductionFromTargetTrisFloor)
{
    // target = 0 → max reduction (caps at kMaxReduction so we don't degenerate)
    EXPECT_DOUBLE_EQ(MeshDecimator::kMaxReduction,
                     MeshDecimator::reductionFromTargetTris(10000, 0));
    EXPECT_DOUBLE_EQ(MeshDecimator::kMaxReduction,
                     MeshDecimator::reductionFromTargetTris(10000, -10));
}

TEST(MeshDecimatorTest, ReductionFromTargetVertsHalving)
{
    EXPECT_NEAR(0.5, MeshDecimator::reductionFromTargetVerts(2000, 1000), 1e-9);
}

TEST(MeshDecimatorTest, ClampReductionBounds)
{
    EXPECT_DOUBLE_EQ(0.0, MeshDecimator::clampReduction(0.0));
    EXPECT_DOUBLE_EQ(0.0, MeshDecimator::clampReduction(-0.5));
    EXPECT_DOUBLE_EQ(0.25, MeshDecimator::clampReduction(0.25));
    EXPECT_DOUBLE_EQ(MeshDecimator::kMaxReduction,
                     MeshDecimator::clampReduction(0.99));
    EXPECT_DOUBLE_EQ(MeshDecimator::kMaxReduction,
                     MeshDecimator::clampReduction(2.0));
}

TEST(MeshDecimatorTest, ClampReductionNaN)
{
    // NaN should fold to 0 (analyze-only / safe behaviour).
    EXPECT_DOUBLE_EQ(0.0, MeshDecimator::clampReduction(
        std::numeric_limits<double>::quiet_NaN()));
}

TEST(MeshDecimatorTest, JsonEmptyReport)
{
    DecimationReport empty;
    const QJsonObject obj = MeshDecimator::toJson(empty);
    EXPECT_TRUE(obj.contains("mesh"));
    EXPECT_TRUE(obj.contains("submeshes"));
    EXPECT_TRUE(obj.contains("totals"));
    EXPECT_FALSE(obj["applied"].toBool());
    EXPECT_EQ(0, obj["totals"].toObject()["trianglesBefore"].toInt());
    EXPECT_DOUBLE_EQ(0.0, obj["totals"].toObject()["effectiveReduction"].toDouble());
}

TEST(MeshDecimatorTest, JsonShapeWithSubmeshes)
{
    DecimationReport report;
    report.meshName = "Cube.mesh";
    report.appliedReduction = 0.5;
    report.applied = true;
    DecimationSubmeshReport sr;
    sr.meshName = "Cube.mesh";
    sr.submeshIndex = 0;
    sr.trianglesBefore = 1000;
    sr.trianglesAfter = 500;
    report.submeshes.append(sr);
    report.totalTrianglesBefore = 1000;
    report.totalTrianglesAfter = 500;

    const QJsonObject obj = MeshDecimator::toJson(report);
    EXPECT_EQ(QString("Cube.mesh"), obj["mesh"].toString());
    EXPECT_DOUBLE_EQ(0.5, obj["appliedReduction"].toDouble());
    EXPECT_TRUE(obj["applied"].toBool());
    EXPECT_EQ(1, obj["submeshes"].toArray().size());
    EXPECT_EQ(1000, obj["totals"].toObject()["trianglesBefore"].toInt());
    EXPECT_EQ(500, obj["totals"].toObject()["trianglesAfter"].toInt());
    EXPECT_NEAR(0.5, obj["totals"].toObject()["effectiveReduction"].toDouble(), 1e-9);
}

TEST(MeshDecimatorTest, EffectiveReductionDivByZero)
{
    DecimationReport report;
    EXPECT_DOUBLE_EQ(0.0, report.effectiveReduction());
}

TEST(MeshDecimatorTest, TextHasHeaderAndAppliedFlag)
{
    DecimationReport report;
    report.meshName = "M.mesh";
    report.appliedReduction = 0.3;
    report.applied = true;
    DecimationSubmeshReport sr;
    sr.submeshIndex = 0;
    sr.trianglesBefore = 100;
    sr.trianglesAfter = 70;
    report.submeshes.append(sr);
    report.totalTrianglesBefore = 100;
    report.totalTrianglesAfter = 70;

    const QString text = MeshDecimator::toText(report);
    EXPECT_TRUE(text.contains("Mesh Decimation"));
    EXPECT_TRUE(text.contains("M.mesh"));
    EXPECT_TRUE(text.contains("30.0%"));
    EXPECT_TRUE(text.contains("applied"));
}

TEST(MeshDecimatorTest, TextHandlesProjected)
{
    DecimationReport report;
    report.meshName = "M.mesh";
    report.appliedReduction = 0.25;
    report.applied = false;
    report.totalTrianglesBefore = 4;
    report.totalTrianglesAfter = 3;
    QString text = MeshDecimator::toText(report);
    EXPECT_TRUE(text.contains("projected"));
    EXPECT_FALSE(text.contains("applied"));
}

TEST(MeshDecimatorTest, TextHandlesEmpty)
{
    DecimationReport empty;
    const QString text = MeshDecimator::toText(empty);
    EXPECT_TRUE(text.contains("(no mesh)"));
}
