#include <gtest/gtest.h>

#include <QString>

#include "FeedbackPrefill.h"
#include "FeedbackReportHelper.h"

// These tests exercise only the pure-data portions of FeedbackReportHelper:
//   - importFailurePrefill / exportFailurePrefill builders
//   - setOpenFeedbackHandler / resetForTests handler state
//
// showFailureWithReportOption() is intentionally NOT tested here: it opens a
// modal QMessageBox via exec(), which requires a real display.

namespace {

// Ensure each test starts from a clean handler state.
class FeedbackReportHelperTest : public ::testing::Test {
protected:
    void SetUp() override { FeedbackReportHelper::resetForTests(); }
    void TearDown() override { FeedbackReportHelper::resetForTests(); }
};

} // namespace

// -------- importFailurePrefill --------

TEST_F(FeedbackReportHelperTest, ImportPrefillSetsTypeAndOperation)
{
    FeedbackPrefill p = FeedbackReportHelper::importFailurePrefill(
        QStringLiteral("fbx"), QStringLiteral("boom"), QStringLiteral("E42"));

    EXPECT_EQ(p.type, QStringLiteral("import_problem"));
    EXPECT_EQ(p.relatedOperation, QStringLiteral("import"));
    EXPECT_EQ(p.relatedFormat, QStringLiteral("fbx"));
    EXPECT_EQ(p.errorMessage, QStringLiteral("boom"));
    EXPECT_EQ(p.errorCode, QStringLiteral("E42"));
}

TEST_F(FeedbackReportHelperTest, ImportPrefillDefaultErrorCodeIsEmpty)
{
    FeedbackPrefill p = FeedbackReportHelper::importFailurePrefill(
        QStringLiteral("obj"), QStringLiteral("could not read file"));

    EXPECT_EQ(p.type, QStringLiteral("import_problem"));
    EXPECT_EQ(p.relatedOperation, QStringLiteral("import"));
    EXPECT_EQ(p.relatedFormat, QStringLiteral("obj"));
    EXPECT_EQ(p.errorMessage, QStringLiteral("could not read file"));
    EXPECT_TRUE(p.errorCode.isEmpty());
}

TEST_F(FeedbackReportHelperTest, ImportPrefillEmptyInputsPropagate)
{
    FeedbackPrefill p = FeedbackReportHelper::importFailurePrefill(
        QString(), QString(), QString());

    // type/operation are always set regardless of inputs.
    EXPECT_EQ(p.type, QStringLiteral("import_problem"));
    EXPECT_EQ(p.relatedOperation, QStringLiteral("import"));
    EXPECT_TRUE(p.relatedFormat.isEmpty());
    EXPECT_TRUE(p.errorMessage.isEmpty());
    EXPECT_TRUE(p.errorCode.isEmpty());
}

TEST_F(FeedbackReportHelperTest, ImportPrefillPreservesExactStringsIncludingUnicodeAndWhitespace)
{
    const QString fmt = QStringLiteral("  glTF 2.0  ");
    const QString msg = QStringLiteral("ünïcödé: line1\nline2\twith tab");
    const QString code = QStringLiteral("0xDEADBEEF");

    FeedbackPrefill p = FeedbackReportHelper::importFailurePrefill(fmt, msg, code);

    EXPECT_EQ(p.relatedFormat, fmt);
    EXPECT_EQ(p.errorMessage, msg);
    EXPECT_EQ(p.errorCode, code);
}

// -------- exportFailurePrefill --------

TEST_F(FeedbackReportHelperTest, ExportPrefillSetsTypeAndOperation)
{
    FeedbackPrefill p = FeedbackReportHelper::exportFailurePrefill(
        QStringLiteral("stl"), QStringLiteral("write failed"), QStringLiteral("W7"));

    EXPECT_EQ(p.type, QStringLiteral("export_problem"));
    EXPECT_EQ(p.relatedOperation, QStringLiteral("export"));
    EXPECT_EQ(p.relatedFormat, QStringLiteral("stl"));
    EXPECT_EQ(p.errorMessage, QStringLiteral("write failed"));
    EXPECT_EQ(p.errorCode, QStringLiteral("W7"));
}

TEST_F(FeedbackReportHelperTest, ExportPrefillDefaultErrorCodeIsEmpty)
{
    FeedbackPrefill p = FeedbackReportHelper::exportFailurePrefill(
        QStringLiteral("dae"), QStringLiteral("permission denied"));

    EXPECT_EQ(p.type, QStringLiteral("export_problem"));
    EXPECT_EQ(p.relatedOperation, QStringLiteral("export"));
    EXPECT_EQ(p.relatedFormat, QStringLiteral("dae"));
    EXPECT_EQ(p.errorMessage, QStringLiteral("permission denied"));
    EXPECT_TRUE(p.errorCode.isEmpty());
}

TEST_F(FeedbackReportHelperTest, ExportPrefillEmptyInputsPropagate)
{
    FeedbackPrefill p = FeedbackReportHelper::exportFailurePrefill(
        QString(), QString(), QString());

    EXPECT_EQ(p.type, QStringLiteral("export_problem"));
    EXPECT_EQ(p.relatedOperation, QStringLiteral("export"));
    EXPECT_TRUE(p.relatedFormat.isEmpty());
    EXPECT_TRUE(p.errorMessage.isEmpty());
    EXPECT_TRUE(p.errorCode.isEmpty());
}

// -------- import vs export distinctness --------

TEST_F(FeedbackReportHelperTest, ImportAndExportProduceDistinctTypesAndOperations)
{
    FeedbackPrefill imp = FeedbackReportHelper::importFailurePrefill(
        QStringLiteral("fbx"), QStringLiteral("m"), QStringLiteral("c"));
    FeedbackPrefill exp = FeedbackReportHelper::exportFailurePrefill(
        QStringLiteral("fbx"), QStringLiteral("m"), QStringLiteral("c"));

    EXPECT_NE(imp.type, exp.type);
    EXPECT_NE(imp.relatedOperation, exp.relatedOperation);

    // Other propagated fields are identical for identical inputs.
    EXPECT_EQ(imp.relatedFormat, exp.relatedFormat);
    EXPECT_EQ(imp.errorMessage, exp.errorMessage);
    EXPECT_EQ(imp.errorCode, exp.errorCode);
}

// -------- handler state: set then reset --------
