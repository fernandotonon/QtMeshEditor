// Coverage for ScanEngine::run() end-to-end fix-pipeline branches that the
// existing ScanEngine_test.cpp Run_* cases never exercise:
//   * run() with config.fixEnabled=true + a file_name_case violation: drives a
//     real QFile::rename through applyFixes and the run() fix-tally branches
//     (result.fixed++, asset.filePath/relativePath mutation) — ScanEngine.cpp
//     lines ~2188-2218 + applyFixes ~1931-1953.
//   * run() onAssetProcessed callback (lines ~2185-2186): asserted to fire once
//     per scanned asset, carrying the AssetInfo + its findings.
//   * run() multi-root iteration via config.roots.
//   * dryRun fix path producing the "[dry-run: would rename to ...]" message
//     without actually renaming the file on disk.
//
// Distinct filename + suite name (ScanEngineRunPipelineCoverageTest) from the
// existing ScanEngine_test.cpp suites to avoid ODR / duplicate-registration.
//
// Needs Ogre because ScanEngine::inspectAsset loads each asset through
// MeshImporterExporter (the editor's own loader).

#include <gtest/gtest.h>

#include "ScanConfig.h"
#include "ScanEngine.h"
#include "TestHelpers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

// Writes a minimal but valid single-triangle OBJ (mirrors the helper in
// ScanEngine_test.cpp). Returns the absolute path, or empty on failure.
QString writeTriObj(const QString& dirPath, const QString& fileName)
{
    const QString path = QDir(dirPath).filePath(fileName);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    const QByteArray obj =
        "o Tri\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n";
    f.write(obj);
    f.close();
    return path;
}

} // namespace

class ScanEngineRunPipelineCoverageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        createStandardOgreMaterials();
    }

    // A config that only cares about the snake_case naming rule so a clean OBJ
    // produces exactly one (fixable) finding.
    static ScanConfig snakeCaseFixConfig(bool fixEnabled, bool dryRun)
    {
        ScanConfig config = ScanConfig::defaults();
        config.includePatterns = {"**/*.obj"};
        config.excludePatterns = {};
        config.failOn = "never";
        config.fileNameCase = "snake_case";
        config.fixEnabled = fixEnabled;
        config.dryRun = dryRun;
        return config;
    }
};

// --- Live rename via run() + fixEnabled=true ---------------------------------

TEST_F(ScanEngineRunPipelineCoverageTest, RunFixRenamesFileAndTalliesFixed)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    // "MixedCase.obj" violates snake_case → convertNameToCase => "mixed_case.obj".
    const QString original = writeTriObj(tmpDir.path(), "MixedCase.obj");
    ASSERT_FALSE(original.isEmpty());
    ASSERT_TRUE(QFile::exists(original));

    ScanConfig config = snakeCaseFixConfig(/*fixEnabled=*/true, /*dryRun=*/false);

    const ScanResult result = ScanEngine::run(config, tmpDir.path());

    EXPECT_EQ(result.scanned, 1);
    // The fix tally branch (result.fixed++) must have run.
    EXPECT_GE(result.fixed, 1);

    // The renamed file exists on disk; the original no longer does.
    const QString renamed = QDir(tmpDir.path()).filePath("mixed_case.obj");
    EXPECT_TRUE(QFile::exists(renamed));
    EXPECT_FALSE(QFile::exists(original));

    // A fixed finding does not count toward warnings/errors.
    EXPECT_EQ(result.warnings, 0);
    EXPECT_EQ(result.errors, 0);

    // The recorded AssetInfo had its paths mutated to the new name.
    ASSERT_EQ(result.assets.size(), 1);
    const AssetInfo& a = result.assets.first();
    EXPECT_EQ(QFileInfo(a.filePath).fileName(), QStringLiteral("mixed_case.obj"));
    EXPECT_EQ(a.relativePath, QStringLiteral("mixed_case.obj"));

    // The finding itself is flagged fixed with the renamed message.
    bool sawFixed = false;
    for (const Finding& f : result.findings) {
        if (f.rule == QLatin1String("file_name_case")) {
            EXPECT_TRUE(f.fixed);
            EXPECT_TRUE(f.message.contains(QStringLiteral("renamed to mixed_case.obj")));
            sawFixed = true;
        }
    }
    EXPECT_TRUE(sawFixed);
}

// --- onAssetProcessed callback fires once per asset --------------------------

TEST_F(ScanEngineRunPipelineCoverageTest, RunInvokesOnAssetProcessedCallbackPerAsset)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    ASSERT_FALSE(writeTriObj(tmpDir.path(), "alpha_one.obj").isEmpty());
    ASSERT_FALSE(writeTriObj(tmpDir.path(), "BravoTwo.obj").isEmpty()); // violates snake_case

    ScanConfig config = snakeCaseFixConfig(/*fixEnabled=*/false, /*dryRun=*/false);

    int callbackCount = 0;
    QStringList seenRelPaths;
    int totalFindings = 0;
    bool sawFindingForBravo = false;

    auto cb = [&](const AssetInfo& asset, const QList<Finding>& findings) {
        ++callbackCount;
        seenRelPaths << asset.relativePath;
        totalFindings += findings.size();
        // The clean snake_case asset has no findings; the MixedCase one has the
        // file_name_case finding routed through the callback.
        if (asset.relativePath.startsWith(QStringLiteral("BravoTwo"))) {
            for (const Finding& f : findings) {
                if (f.rule == QLatin1String("file_name_case")) {
                    sawFindingForBravo = true;
                    EXPECT_TRUE(f.fixable);
                    EXPECT_FALSE(f.fixed); // fixEnabled=false, so not applied
                }
            }
        }
    };

    const ScanResult result = ScanEngine::run(config, tmpDir.path(), cb);

    EXPECT_EQ(result.scanned, 2);
    EXPECT_EQ(callbackCount, 2);
    EXPECT_EQ(seenRelPaths.size(), 2);
    EXPECT_TRUE(seenRelPaths.contains(QStringLiteral("alpha_one.obj")));
    EXPECT_TRUE(seenRelPaths.contains(QStringLiteral("BravoTwo.obj")));
    EXPECT_GE(totalFindings, 1);
    EXPECT_TRUE(sawFindingForBravo);

    // With fix disabled, nothing was renamed.
    EXPECT_EQ(result.fixed, 0);
    EXPECT_TRUE(QFile::exists(QDir(tmpDir.path()).filePath("BravoTwo.obj")));
}

// --- dry-run fix path: message annotated, no rename --------------------------

TEST_F(ScanEngineRunPipelineCoverageTest, RunDryRunAnnotatesMessageWithoutRenaming)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    const QString original = writeTriObj(tmpDir.path(), "DryRunCase.obj");
    ASSERT_FALSE(original.isEmpty());

    ScanConfig config = snakeCaseFixConfig(/*fixEnabled=*/true, /*dryRun=*/true);

    const ScanResult result = ScanEngine::run(config, tmpDir.path());

    EXPECT_EQ(result.scanned, 1);
    // dry-run: nothing actually fixed.
    EXPECT_EQ(result.fixed, 0);

    // File must NOT have been renamed.
    EXPECT_TRUE(QFile::exists(original));
    EXPECT_FALSE(QFile::exists(QDir(tmpDir.path()).filePath("dry_run_case.obj")));

    // The finding carries the dry-run annotation and is not marked fixed.
    bool sawDryRun = false;
    for (const Finding& f : result.findings) {
        if (f.rule == QLatin1String("file_name_case")) {
            EXPECT_FALSE(f.fixed);
            EXPECT_TRUE(f.message.contains(
                QStringLiteral("[dry-run: would rename to dry_run_case.obj]")));
            sawDryRun = true;
        }
    }
    EXPECT_TRUE(sawDryRun);

    // dry-run findings still count as warnings (they were not "fixed").
    EXPECT_GE(result.warnings, 1);
}

// --- multi-root iteration via config.roots, with a fix in each root ----------

TEST_F(ScanEngineRunPipelineCoverageTest, RunMultiRootFixesAcrossAllRoots)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    const QString rootA = QDir(tmpDir.path()).filePath("rootA");
    const QString rootB = QDir(tmpDir.path()).filePath("rootB");
    ASSERT_TRUE(QDir().mkpath(rootA));
    ASSERT_TRUE(QDir().mkpath(rootB));

    ASSERT_FALSE(writeTriObj(rootA, "FirstAsset.obj").isEmpty());
    ASSERT_FALSE(writeTriObj(rootB, "SecondAsset.obj").isEmpty());

    ScanConfig config = snakeCaseFixConfig(/*fixEnabled=*/true, /*dryRun=*/false);
    config.roots = {rootA, rootB};

    int callbackCount = 0;
    auto cb = [&](const AssetInfo&, const QList<Finding>&) { ++callbackCount; };

    // No rootOverride → run() iterates config.roots.
    const ScanResult result = ScanEngine::run(config, QString(), cb);

    EXPECT_EQ(result.scanned, 2);
    EXPECT_EQ(callbackCount, 2);
    // One fixable name violation per root → both renamed.
    EXPECT_GE(result.fixed, 2);

    EXPECT_TRUE(QFile::exists(QDir(rootA).filePath("first_asset.obj")));
    EXPECT_TRUE(QFile::exists(QDir(rootB).filePath("second_asset.obj")));
    EXPECT_FALSE(QFile::exists(QDir(rootA).filePath("FirstAsset.obj")));
    EXPECT_FALSE(QFile::exists(QDir(rootB).filePath("SecondAsset.obj")));
}

// --- a clean (already snake_case) asset produces no fix, passes --------------

TEST_F(ScanEngineRunPipelineCoverageTest, RunCleanNameProducesNoFixAndPasses)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    ASSERT_FALSE(writeTriObj(tmpDir.path(), "already_snake.obj").isEmpty());

    ScanConfig config = snakeCaseFixConfig(/*fixEnabled=*/true, /*dryRun=*/false);

    int callbackCount = 0;
    int findingCount = 0;
    auto cb = [&](const AssetInfo&, const QList<Finding>& findings) {
        ++callbackCount;
        findingCount += findings.size();
    };

    const ScanResult result = ScanEngine::run(config, tmpDir.path(), cb);

    EXPECT_EQ(result.scanned, 1);
    EXPECT_EQ(callbackCount, 1);
    EXPECT_EQ(findingCount, 0);
    EXPECT_EQ(result.fixed, 0);
    EXPECT_EQ(result.passed, 1);
    EXPECT_EQ(result.warnings, 0);
    EXPECT_EQ(result.errors, 0);

    // File untouched.
    EXPECT_TRUE(QFile::exists(QDir(tmpDir.path()).filePath("already_snake.obj")));
}
