// Additional coverage for ScanEngine's pure-logic static helpers.
//
// These exercise branches not touched by ScanEngine_test.cpp:
//   - convertNameToCase: PascalCase / camelCase passthrough (line 1439),
//     snake_case + kebab-case run-collapse and space/dash normalization,
//     leading-cap not prefixing an underscore/dash (i == 0).
//   - scanReportUtcTimes: empty-completed fallback to current UTC, empty-started
//     mirrors completed, both-populated passthrough.
//   - checkNameCase: failing-case branches for camelCase / PascalCase / lowercase,
//     plus the empty-stem early-return.
//
// All targets are static, pure-logic, and require no Ogre / no display.
// Distinct suite name (ScanEngineHelpersCoverage) avoids collision with the
// existing ScanEngineTest suite.

#include <gtest/gtest.h>

#include "ScanEngine.h"

#include <QDateTime>
#include <QString>

// ---------------------------------------------------------------------------
// convertNameToCase — PascalCase / camelCase passthrough branch (line 1439)
// ---------------------------------------------------------------------------

TEST(ScanEngineHelpersCoverage, ConvertNameToCase_PascalCasePassthrough)
{
    // PascalCase is ambiguous from arbitrary input, so the function returns the
    // input verbatim (including spaces and original extension casing).
    EXPECT_EQ(ScanEngine::convertNameToCase("my file.fbx", "PascalCase"), "my file.fbx");
    EXPECT_EQ(ScanEngine::convertNameToCase("PlayerModel.FBX", "PascalCase"), "PlayerModel.FBX");
}

TEST(ScanEngineHelpersCoverage, ConvertNameToCase_CamelCasePassthrough)
{
    EXPECT_EQ(ScanEngine::convertNameToCase("my file.fbx", "camelCase"), "my file.fbx");
    EXPECT_EQ(ScanEngine::convertNameToCase("Some-Mixed_Name.obj", "camelCase"),
              "Some-Mixed_Name.obj");
}

// ---------------------------------------------------------------------------
// convertNameToCase — snake_case run-collapse + space/dash normalization
// ---------------------------------------------------------------------------

TEST(ScanEngineHelpersCoverage, ConvertNameToCase_SnakeCaseCollapseAndNormalize)
{
    // Dashes and spaces normalize to underscores; intermediate runs collapse to
    // one underscore. Extension casing is preserved (suffix() keeps case).
    EXPECT_EQ(ScanEngine::convertNameToCase("My-Cool File.FBX", "snake_case"),
              "my_cool_file.FBX");
}

TEST(ScanEngineHelpersCoverage, ConvertNameToCase_SnakeCaseLeadingCapNoPrefix)
{
    // i == 0: a leading uppercase letter must NOT be prefixed by an underscore.
    EXPECT_EQ(ScanEngine::convertNameToCase("Player.fbx", "snake_case"), "player.fbx");
}

TEST(ScanEngineHelpersCoverage, ConvertNameToCase_SnakeCaseConsecutiveCapsNoSplit)
{
    // Consecutive uppercase letters (prev isUpper) do not insert underscores.
    EXPECT_EQ(ScanEngine::convertNameToCase("ABCThing.fbx", "snake_case"), "abcthing.fbx");
}

// ---------------------------------------------------------------------------
// convertNameToCase — kebab-case run-collapse + underscore/space normalization
// ---------------------------------------------------------------------------

TEST(ScanEngineHelpersCoverage, ConvertNameToCase_KebabCaseCollapseAndNormalize)
{
    // Underscores and spaces normalize to dashes; runs of dashes collapse.
    EXPECT_EQ(ScanEngine::convertNameToCase("My_Cool File.FBX", "kebab-case"),
              "my-cool-file.FBX");
}

TEST(ScanEngineHelpersCoverage, ConvertNameToCase_KebabCaseLeadingCapNoPrefix)
{
    // i == 0: leading uppercase must not be prefixed by a dash.
    EXPECT_EQ(ScanEngine::convertNameToCase("Player.fbx", "kebab-case"), "player.fbx");
}

TEST(ScanEngineHelpersCoverage, ConvertNameToCase_KebabCaseMultiDashCollapse)
{
    // An existing run of dashes collapses to a single dash.
    EXPECT_EQ(ScanEngine::convertNameToCase("a---b.fbx", "kebab-case"), "a-b.fbx");
}

// ---------------------------------------------------------------------------
// scanReportUtcTimes — fallback and passthrough branches
// ---------------------------------------------------------------------------

TEST(ScanEngineHelpersCoverage, ScanReportUtcTimes_EmptyCompletedFallsBackToNow)
{
    ScanResult result;          // both timestamps empty
    QString started, completed;
    ScanEngine::scanReportUtcTimes(result, &started, &completed);

    // Completed falls back to current UTC: ends with 'Z' and round-trips ISO+ms.
    EXPECT_FALSE(completed.isEmpty());
    EXPECT_TRUE(completed.endsWith('Z'));

    QDateTime parsed = QDateTime::fromString(completed, Qt::ISODateWithMs);
    EXPECT_TRUE(parsed.isValid());

    // Started mirrors completed when started was empty.
    EXPECT_EQ(started, completed);
}

TEST(ScanEngineHelpersCoverage, ScanReportUtcTimes_EmptyStartedMirrorsCompleted)
{
    ScanResult result;
    result.scanCompletedUtc = "2026-06-12T10:20:30.123Z";
    // scanStartedUtc left empty

    QString started, completed;
    ScanEngine::scanReportUtcTimes(result, &started, &completed);

    EXPECT_EQ(completed, "2026-06-12T10:20:30.123Z");
    EXPECT_EQ(started, completed);   // empty started mirrors completed
}

TEST(ScanEngineHelpersCoverage, ScanReportUtcTimes_BothPopulatedPassthrough)
{
    ScanResult result;
    result.scanStartedUtc   = "2026-06-12T10:00:00.000Z";
    result.scanCompletedUtc = "2026-06-12T10:05:00.500Z";

    QString started, completed;
    ScanEngine::scanReportUtcTimes(result, &started, &completed);

    EXPECT_EQ(started, "2026-06-12T10:00:00.000Z");
    EXPECT_EQ(completed, "2026-06-12T10:05:00.500Z");
}

// ---------------------------------------------------------------------------
// checkNameCase — failing-case branches + empty-stem early return
// ---------------------------------------------------------------------------

TEST(ScanEngineHelpersCoverage, CheckNameCase_CamelCaseRejectsLeadingCap)
{
    // camelCase requires a lowercase first letter — "MyFile" must fail.
    EXPECT_FALSE(ScanEngine::checkNameCase("MyFile.fbx", "camelCase"));
    // Sanity: a valid camelCase name passes.
    EXPECT_TRUE(ScanEngine::checkNameCase("myFile.fbx", "camelCase"));
}

TEST(ScanEngineHelpersCoverage, CheckNameCase_PascalCaseRejectsLeadingLower)
{
    // PascalCase requires an uppercase first letter — "myFile" must fail.
    EXPECT_FALSE(ScanEngine::checkNameCase("myFile.fbx", "PascalCase"));
    EXPECT_TRUE(ScanEngine::checkNameCase("MyFile.fbx", "PascalCase"));
}

TEST(ScanEngineHelpersCoverage, CheckNameCase_LowercaseRejectsMixedCase)
{
    // lowercase requires stem == stem.toLower() — "MyFile" must fail.
    EXPECT_FALSE(ScanEngine::checkNameCase("MyFile.fbx", "lowercase"));
    EXPECT_TRUE(ScanEngine::checkNameCase("myfile.fbx", "lowercase"));
}

TEST(ScanEngineHelpersCoverage, CheckNameCase_EmptyStemReturnsTrue)
{
    // Empty stem (e.g. a dotfile-only name) short-circuits to true regardless
    // of convention.
    EXPECT_TRUE(ScanEngine::checkNameCase(".fbx", "snake_case"));
    EXPECT_TRUE(ScanEngine::checkNameCase("", "PascalCase"));
}
