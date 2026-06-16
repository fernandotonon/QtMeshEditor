// Coverage for ScanEngine's report formatters fed by a REAL ScanEngine::run()
// over generated temp assets, rather than hand-built ScanResult/AssetInfo
// fixtures (the existing FormatJson_* / SARIF tests in ScanEngine_test.cpp do
// the latter). This exercises serialization of fields populated by the real
// inspectAsset + evaluateRules walk:
//   * ScanEngine::scanReportToJsonObject(result)  — fields from a live run()
//     (meshCount/vertexCount/faceCount/format/fileSize, per-asset findings
//     array, summary counts, loadError flag on a skipped asset).
//   * ScanEngine::formatJson(result)              — full-string Indented path
//     (line 2552) wrapping scanReportToJsonObject; asserted parseable + keys.
//   * ScanEngine::formatSarif(result, profileId)  — the non-empty
//     activeProfileId branch (runs.properties.profile) + real rule
//     descriptions on the run()-produced findings.
//
// Distinct filename + suite name (ScanEngineReportRoundTripCoverageTest) from
// ScanEngine_test.cpp / ScanEngineRunPipeline_coverage_test.cpp to avoid ODR /
// duplicate-registration clashes.
//
// Needs Ogre because ScanEngine::inspectAsset loads each asset through the
// editor's own loader. SetUp uses ASSERT_TRUE(tryInitOgre()) — never skips.

#include <gtest/gtest.h>

#include "ScanConfig.h"
#include "ScanEngine.h"
#include "TestHelpers.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

namespace {

// Writes a minimal but valid single-triangle OBJ that references a material
// library, so the inspectAsset walk populates material names. Returns the
// absolute OBJ path, or empty on failure.
QString writeTexturedTriObj(const QString& dirPath, const QString& baseName)
{
    const QString objPath = QDir(dirPath).filePath(baseName + ".obj");
    const QString mtlName = baseName + ".mtl";
    const QString mtlPath = QDir(dirPath).filePath(mtlName);
    const QString texName = baseName + "_diffuse.png";

    // A tiny valid PNG so the referenced texture exists on disk.
    {
        QImage img(2, 2, QImage::Format_RGBA8888);
        img.fill(Qt::blue);
        if (!img.save(QDir(dirPath).filePath(texName), "PNG"))
            return QString();
    }

    {
        QFile m(mtlPath);
        if (!m.open(QIODevice::WriteOnly | QIODevice::Text))
            return QString();
        const QByteArray mtl =
            "newmtl ScanMat\n"
            "Kd 0.8 0.2 0.2\n"
            "map_Kd " + texName.toUtf8() + "\n";
        m.write(mtl);
        m.close();
    }

    QFile f(objPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    const QByteArray obj =
        "mtllib " + mtlName.toUtf8() + "\n"
        "o ScanTri\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "vt 0 0\n"
        "vt 1 0\n"
        "vt 0 1\n"
        "usemtl ScanMat\n"
        "f 1/1 2/2 3/3\n";
    f.write(obj);
    f.close();
    return objPath;
}

// Writes a file with an asset extension but garbage content so inspectAsset
// flags loadError and run() tallies it as skipped.
QString writeBrokenObj(const QString& dirPath, const QString& baseName)
{
    const QString path = QDir(dirPath).filePath(baseName + ".obj");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    f.write("this is not a valid wavefront obj @@@ \x00\x01\x02 not geometry\n");
    f.close();
    return path;
}

ScanConfig reportConfig()
{
    ScanConfig config = ScanConfig::defaults();
    config.includePatterns = {"**/*.obj"};
    config.excludePatterns = {};
    config.failOn = "never";
    config.fixEnabled = false;
    config.dryRun = false;
    return config;
}

} // namespace

class ScanEngineReportRoundTripCoverageTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        createStandardOgreMaterials();
    }

    // Builds a temp dir with one good textured OBJ and one broken OBJ, then
    // runs the full scan. Returns the produced ScanResult by out-param and the
    // tmp dir stays alive for the duration of the test via the member.
    ScanResult runMixedScan(QTemporaryDir& tmpDir, QString& goodRel, QString& badRel)
    {
        EXPECT_TRUE(tmpDir.isValid());
        const QString good = writeTexturedTriObj(tmpDir.path(), "good_asset");
        const QString bad  = writeBrokenObj(tmpDir.path(), "broken_asset");
        EXPECT_FALSE(good.isEmpty());
        EXPECT_FALSE(bad.isEmpty());
        goodRel = QFileInfo(good).fileName();
        badRel  = QFileInfo(bad).fileName();

        ScanConfig config = reportConfig();
        return ScanEngine::run(config, tmpDir.path());
    }
};

// --- scanReportToJsonObject over a real run() --------------------------------

TEST_F(ScanEngineReportRoundTripCoverageTest, JsonObjectReflectsRealRunCountsAndAssets)
{
    QTemporaryDir tmpDir;
    QString goodRel, badRel;
    const ScanResult result = runMixedScan(tmpDir, goodRel, badRel);

    // Both files enumerated and inspected.
    EXPECT_EQ(result.scanned, 2);
    // The broken file must have been recorded as a skipped (loadError) asset.
    EXPECT_GE(result.skipped, 1);
    ASSERT_EQ(result.assets.size(), 2);

    const QJsonObject root = ScanEngine::scanReportToJsonObject(result);

    // Top-level schema keys.
    EXPECT_TRUE(root.contains(QStringLiteral("version")));
    EXPECT_TRUE(root.contains(QStringLiteral("scanStartedUtc")));
    EXPECT_TRUE(root.contains(QStringLiteral("scanCompletedUtc")));
    ASSERT_TRUE(root.contains(QStringLiteral("summary")));
    ASSERT_TRUE(root.contains(QStringLiteral("assets")));

    // Summary mirrors the run() tallies.
    const QJsonObject summary = root.value(QStringLiteral("summary")).toObject();
    EXPECT_EQ(summary.value(QStringLiteral("scanned")).toInt(), result.scanned);
    EXPECT_EQ(summary.value(QStringLiteral("passed")).toInt(), result.passed);
    EXPECT_EQ(summary.value(QStringLiteral("warnings")).toInt(), result.warnings);
    EXPECT_EQ(summary.value(QStringLiteral("errors")).toInt(), result.errors);
    EXPECT_EQ(summary.value(QStringLiteral("skipped")).toInt(), result.skipped);
    EXPECT_TRUE(summary.contains(QStringLiteral("elapsedMs")));

    // Assets array carries both relative paths.
    const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
    ASSERT_EQ(assets.size(), 2);

    QJsonObject goodObj, badObj;
    bool sawGood = false, sawBad = false;
    for (const QJsonValue& v : assets) {
        const QJsonObject ao = v.toObject();
        const QString file = ao.value(QStringLiteral("file")).toString();
        EXPECT_TRUE(ao.contains(QStringLiteral("format")));
        EXPECT_TRUE(ao.contains(QStringLiteral("fileSize")));
        EXPECT_TRUE(ao.contains(QStringLiteral("findings")));
        if (file == goodRel) { goodObj = ao; sawGood = true; }
        if (file == badRel)  { badObj  = ao; sawBad  = true; }
    }
    ASSERT_TRUE(sawGood);
    ASSERT_TRUE(sawBad);

    // The good asset: real geometry fields populated by the Ogre/Assimp walk.
    EXPECT_EQ(goodObj.value(QStringLiteral("format")).toString(), QStringLiteral("obj"));
    EXPECT_GT(goodObj.value(QStringLiteral("fileSize")).toInt(), 0);
    EXPECT_GE(goodObj.value(QStringLiteral("meshCount")).toInt(), 1);
    EXPECT_GE(goodObj.value(QStringLiteral("vertexCount")).toInt(), 3);
    EXPECT_GE(goodObj.value(QStringLiteral("faceCount")).toInt(), 1);
    // Good asset is not flagged as a load error.
    EXPECT_FALSE(goodObj.contains(QStringLiteral("loadError")));

    // The broken asset: loadError flag set in the serialized object.
    EXPECT_TRUE(badObj.value(QStringLiteral("loadError")).toBool());
    EXPECT_TRUE(badObj.contains(QStringLiteral("findings")));
}

TEST_F(ScanEngineReportRoundTripCoverageTest, JsonObjectGoodAssetHasMaterialOrTextureSignal)
{
    QTemporaryDir tmpDir;
    QString goodRel, badRel;
    const ScanResult result = runMixedScan(tmpDir, goodRel, badRel);
    ASSERT_EQ(result.assets.size(), 2);

    // Find the AssetInfo for the good asset and assert the real inspect walk
    // produced sensible material/texture-reference counts.
    bool checked = false;
    for (const AssetInfo& a : result.assets) {
        if (a.relativePath != goodRel) continue;
        EXPECT_FALSE(a.loadError);
        EXPECT_GE(a.meshCount, 1u);
        EXPECT_GE(a.vertexCount, 3u);
        EXPECT_GE(a.faceCount, 1u);
        EXPECT_EQ(a.format, QStringLiteral("obj"));
        // At least one of: a named material or a texture reference was found.
        EXPECT_TRUE(!a.materialNames.isEmpty() || a.textureRefCount > 0
                    || !a.texturePaths.isEmpty());
        checked = true;
    }
    EXPECT_TRUE(checked);
}

// --- formatJson full-string path ---------------------------------------------

TEST_F(ScanEngineReportRoundTripCoverageTest, FormatJsonProducesParseableDocumentWithKeys)
{
    QTemporaryDir tmpDir;
    QString goodRel, badRel;
    const ScanResult result = runMixedScan(tmpDir, goodRel, badRel);

    const QString jsonStr = ScanEngine::formatJson(result);
    EXPECT_FALSE(jsonStr.isEmpty());
    // Indented output spans multiple lines.
    EXPECT_TRUE(jsonStr.contains(QLatin1Char('\n')));

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(jsonStr.toUtf8(), &perr);
    ASSERT_EQ(perr.error, QJsonParseError::NoError) << perr.errorString().toStdString();
    ASSERT_TRUE(doc.isObject());

    const QJsonObject root = doc.object();
    EXPECT_TRUE(root.contains(QStringLiteral("summary")));
    EXPECT_TRUE(root.contains(QStringLiteral("assets")));
    EXPECT_TRUE(root.contains(QStringLiteral("version")));

    // The parsed document equals the canonical object (formatJson is a thin
    // QJsonDocument::Indented wrapper around scanReportToJsonObject).
    EXPECT_EQ(root, ScanEngine::scanReportToJsonObject(result));

    // Both relative paths appear somewhere in the serialized assets.
    const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
    QStringList files;
    for (const QJsonValue& v : assets)
        files << v.toObject().value(QStringLiteral("file")).toString();
    EXPECT_TRUE(files.contains(goodRel));
    EXPECT_TRUE(files.contains(badRel));
}

// --- formatSarif with a non-empty activeProfileId ----------------------------

TEST_F(ScanEngineReportRoundTripCoverageTest, FormatSarifWithProfileIdEmitsProfileAndRules)
{
    QTemporaryDir tmpDir;
    QString goodRel, badRel;
    const ScanResult result = runMixedScan(tmpDir, goodRel, badRel);

    const QString profileId = QStringLiteral("example-minimal");
    const QString sarifStr = ScanEngine::formatSarif(result, profileId);
    EXPECT_FALSE(sarifStr.isEmpty());

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(sarifStr.toUtf8(), &perr);
    ASSERT_EQ(perr.error, QJsonParseError::NoError) << perr.errorString().toStdString();
    ASSERT_TRUE(doc.isObject());

    const QJsonObject sarif = doc.object();
    EXPECT_EQ(sarif.value(QStringLiteral("version")).toString(), QStringLiteral("2.1.0"));
    EXPECT_TRUE(sarif.contains(QStringLiteral("$schema")));

    const QJsonArray runs = sarif.value(QStringLiteral("runs")).toArray();
    ASSERT_EQ(runs.size(), 1);
    const QJsonObject run = runs.first().toObject();

    // The non-empty profile id branch wrote run.properties.profile.
    ASSERT_TRUE(run.contains(QStringLiteral("properties")));
    EXPECT_EQ(run.value(QStringLiteral("properties")).toObject()
                  .value(QStringLiteral("profile")).toString(),
              profileId);

    // Driver carries the tool name + rule definitions.
    const QJsonObject driver = run.value(QStringLiteral("tool")).toObject()
                                   .value(QStringLiteral("driver")).toObject();
    EXPECT_EQ(driver.value(QStringLiteral("name")).toString(), QStringLiteral("qtmesh scan"));
    EXPECT_TRUE(driver.contains(QStringLiteral("rules")));

    // Invocations present with execution flag.
    const QJsonArray invocations = run.value(QStringLiteral("invocations")).toArray();
    ASSERT_EQ(invocations.size(), 1);
    EXPECT_TRUE(invocations.first().toObject()
                    .value(QStringLiteral("executionSuccessful")).toBool());

    // Results: every finding became a SARIF result with a known rule id and
    // a real shortDescription (not just the bare id) where described.
    const QJsonArray results = run.value(QStringLiteral("results")).toArray();
    EXPECT_EQ(results.size(), result.findings.size());

    // Build the set of rule ids present in the rules array and assert the
    // load_error rule (fired by the broken asset) carries its real description.
    const QJsonArray rules = driver.value(QStringLiteral("rules")).toArray();
    bool sawLoadErrorRuleWithDesc = false;
    for (const QJsonValue& rv : rules) {
        const QJsonObject ro = rv.toObject();
        if (ro.value(QStringLiteral("id")).toString() == QStringLiteral("load_error")) {
            const QString desc = ro.value(QStringLiteral("shortDescription")).toObject()
                                     .value(QStringLiteral("text")).toString();
            EXPECT_EQ(desc, QStringLiteral("Asset file could not be loaded"));
            sawLoadErrorRuleWithDesc = true;
        }
    }
    // The broken asset should have produced a load_error finding.
    bool sawLoadErrorFinding = false;
    for (const Finding& f : result.findings)
        if (f.rule == QLatin1String("load_error")) sawLoadErrorFinding = true;
    if (sawLoadErrorFinding)
        EXPECT_TRUE(sawLoadErrorRuleWithDesc);
}

TEST_F(ScanEngineReportRoundTripCoverageTest, FormatSarifEmptyProfileIdOmitsProperties)
{
    QTemporaryDir tmpDir;
    QString goodRel, badRel;
    const ScanResult result = runMixedScan(tmpDir, goodRel, badRel);

    // Empty profile id (default branch): run.properties must be absent.
    const QString sarifStr = ScanEngine::formatSarif(result, QString());
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(sarifStr.toUtf8(), &perr);
    ASSERT_EQ(perr.error, QJsonParseError::NoError) << perr.errorString().toStdString();

    const QJsonObject run = doc.object().value(QStringLiteral("runs")).toArray()
                                .first().toObject();
    EXPECT_FALSE(run.contains(QStringLiteral("properties")));
    // Results still present and equal in count to the findings.
    EXPECT_EQ(run.value(QStringLiteral("results")).toArray().size(), result.findings.size());
}

// --- formatters over an empty (no-asset) run() -------------------------------

TEST_F(ScanEngineReportRoundTripCoverageTest, FormattersHandleEmptyRunGracefully)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    // Empty directory → no matching assets.
    ScanConfig config = reportConfig();
    const ScanResult result = ScanEngine::run(config, tmpDir.path());

    EXPECT_EQ(result.scanned, 0);
    EXPECT_EQ(result.assets.size(), 0);

    const QJsonObject root = ScanEngine::scanReportToJsonObject(result);
    EXPECT_EQ(root.value(QStringLiteral("assets")).toArray().size(), 0);
    EXPECT_EQ(root.value(QStringLiteral("summary")).toObject()
                  .value(QStringLiteral("scanned")).toInt(), 0);

    // formatJson stays parseable on an empty result.
    QJsonParseError jerr{};
    const QJsonDocument jdoc = QJsonDocument::fromJson(
        ScanEngine::formatJson(result).toUtf8(), &jerr);
    EXPECT_EQ(jerr.error, QJsonParseError::NoError);
    EXPECT_TRUE(jdoc.isObject());

    // formatSarif with a profile id still emits a valid doc with zero results.
    QJsonParseError serr{};
    const QJsonDocument sdoc = QJsonDocument::fromJson(
        ScanEngine::formatSarif(result, QStringLiteral("example-minimal")).toUtf8(), &serr);
    ASSERT_EQ(serr.error, QJsonParseError::NoError);
    const QJsonObject srun = sdoc.object().value(QStringLiteral("runs")).toArray()
                                 .first().toObject();
    EXPECT_EQ(srun.value(QStringLiteral("results")).toArray().size(), 0);
    // Profile id branch still fires even with no findings.
    EXPECT_EQ(srun.value(QStringLiteral("properties")).toObject()
                  .value(QStringLiteral("profile")).toString(),
              QStringLiteral("example-minimal"));
}
