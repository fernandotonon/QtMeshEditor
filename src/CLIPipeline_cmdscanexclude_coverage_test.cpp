// Coverage tests for CLIPipeline::cmdScan filter / fix / report-path branches
// that the heavily-covered CLIPipeline_test.cpp suite (lines ~2498-3179) and the
// CLIPipelineCmdScanProfileCoverage suite do NOT assert in isolation:
//
//   * --exclude '<bare-ext>' as the SOLE filter: the bare pattern is normalized
//     with the **/ prefix (CLIPipeline.cpp ~4366-4375) and excluded assets must
//     not appear in the written report JSON.
//   * --fix --dry-run: both config.fixEnabled AND config.dryRun get set true
//     (CLIPipeline.cpp ~4237-4238) and the scan still completes (exit governed
//     by --fail-on never -> 0).
//   * --report into a NESTED non-existent subdir forces QDir().mkpath of the
//     parent (CLIPipeline.cpp ~4423-4424); the report file is created and is
//     valid JSON.
//   * --include <bare-ext> AND --exclude <bare-ext> together on a populated
//     QTemporaryDir: the resulting report JSON contains only the kept files.
//
// Distinct filename + distinct suite name (CLIPipelineCmdScanExcludeCoverage) so
// there is no ODR clash / duplicate registration with the existing translation
// units. The scan walk loads assets through MeshImporterExporter, so the
// directory-scan cases need Ogre — the fixture brings Ogre up with
// tryInitOgre() (NEVER skips, per the CI harness rule) and seeds a QTemporaryDir
// with deterministically-generated asset files (minimal .obj geometry + a real
// .mesh copied from testRobotMeshPath() when available). --fail-on never is
// always passed so the exit code is deterministic regardless of findings.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <initializer_list>

#include "CLIPipeline.h"
#include "TestHelpers.h"

namespace {

/// RAII argc/argv builder (anonymous-namespace local so it does not collide with
/// the TestArgv in CLIPipeline_test.cpp or the *Argv helpers in the other
/// CLIPipeline_cmd*_coverage_test.cpp translation units).
class ScanExcludeArgv {
public:
    ScanExcludeArgv(std::initializer_list<const char*> args)
    {
        for (auto* a : args)
            m_storage.push_back(QByteArray(a));
        for (auto& ba : m_storage)
            m_argv.push_back(ba.data());
        m_argc = static_cast<int>(m_argv.size());
    }
    int argc() const { return m_argc; }
    char** argv() { return m_argv.data(); }

private:
    QList<QByteArray> m_storage;
    QList<char*> m_argv;
    int m_argc = 0;
};

// Write a minimal-but-valid OBJ triangle (mirrors writeMinimalObj in
// CLIPipeline_test.cpp; kept local to this TU to avoid cross-file linkage).
QString writeTriObj(const QString& dirPath, const QString& fileName)
{
    const QString path = QDir(dirPath).filePath(fileName);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
        return QString();
    f.write(
        "o Tri\n"
        "v 0 0 0\n"
        "v 1 0 0\n"
        "v 0 1 0\n"
        "f 1 2 3\n");
    f.close();
    return path;
}

// Collect the "file" entries of the assets[] array of a scan report JSON.
QStringList reportAssetFiles(const QJsonObject& root)
{
    QStringList out;
    const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
    for (const auto& v : assets)
        out.append(v.toObject().value(QStringLiteral("file")).toString());
    return out;
}

// Returns true if any asset "file" entry has the given suffix (case-insensitive).
bool anyFileHasSuffix(const QStringList& files, const QString& suffix)
{
    for (const QString& f : files)
        if (f.endsWith(suffix, Qt::CaseInsensitive))
            return true;
    return false;
}

} // namespace

// ===========================================================================
// Ogre-backed fixture: ScanEngine::run loads assets via MeshImporterExporter.
// ===========================================================================

class CLIPipelineScanExcludeOgreFixture : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        createStandardOgreMaterials();
    }

    // Build a temp dir with two .obj triangles and (when available) a real
    // .mesh copied from the test data. Returns the count of .obj + .mesh assets
    // actually written via *out params.
    void seedScanDir(QTemporaryDir& dir, int* outObjCount, bool* outHaveMesh)
    {
        ASSERT_TRUE(dir.isValid());
        int objs = 0;
        if (!writeTriObj(dir.path(), QStringLiteral("alpha.obj")).isEmpty()) ++objs;
        if (!writeTriObj(dir.path(), QStringLiteral("beta.obj")).isEmpty())  ++objs;

        bool haveMesh = false;
        const QString robot = testRobotMeshPath();
        if (!robot.isEmpty() && QFile::exists(robot)) {
            const QString dst = QDir(dir.path()).filePath(QStringLiteral("robot.mesh"));
            haveMesh = QFile::copy(robot, dst);
        }
        if (outObjCount) *outObjCount = objs;
        if (outHaveMesh) *outHaveMesh = haveMesh;
    }
};

// ---------------------------------------------------------------------------
// --exclude '<bare-ext>' as the SOLE filter (no --include). The bare pattern
// "*.obj" must be normalized to "**/*.obj" (lines ~4371-4373) and every .obj
// asset must be excluded from the report; surviving assets (.mesh, if present)
// stay. Verified by parsing the written --report JSON.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineScanExcludeOgreFixture, ExcludeBareExtensionDropsMatchingAssets)
{
    QTemporaryDir scanDir;
    int objCount = 0; bool haveMesh = false;
    seedScanDir(scanDir, &objCount, &haveMesh);
    ASSERT_GT(objCount, 0);

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    const QString reportPath = QDir(outDir.path()).filePath(QStringLiteral("report.json"));

    const QByteArray rootBa   = scanDir.path().toUtf8();
    const QByteArray reportBa = reportPath.toUtf8();

    ScanExcludeArgv args({"qtmesh", "scan", rootBa.constData(),
                          "--exclude", "*.obj",
                          "--report", reportBa.constData(),
                          "--fail-on", "never"});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFileInfo(reportPath).exists());
    QFile rf(reportPath);
    ASSERT_TRUE(rf.open(QIODevice::ReadOnly | QIODevice::Text));
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(rf.readAll(), &perr);
    rf.close();
    ASSERT_EQ(perr.error, QJsonParseError::NoError) << perr.errorString().toStdString();
    ASSERT_TRUE(doc.isObject());
    const QJsonObject root = doc.object();

    const QStringList files = reportAssetFiles(root);
    // No .obj asset may survive the exclude filter.
    EXPECT_FALSE(anyFileHasSuffix(files, QStringLiteral(".obj")))
        << "files: " << files.join(",").toStdString();
    // If a real .mesh was seeded it is NOT matched by *.obj and must remain.
    if (haveMesh)
        EXPECT_TRUE(anyFileHasSuffix(files, QStringLiteral(".mesh")))
            << "files: " << files.join(",").toStdString();
}

// ---------------------------------------------------------------------------
// --fix --dry-run: both config.fixEnabled and config.dryRun are set true
// (lines ~4237-4238). The scan must still complete and, with --fail-on never,
// return 0. The report's summary block is present and reflects a completed
// scan (scanned >= number of asset files we wrote).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineScanExcludeOgreFixture, FixDryRunCompletesAndWritesSummary)
{
    QTemporaryDir scanDir;
    int objCount = 0; bool haveMesh = false;
    seedScanDir(scanDir, &objCount, &haveMesh);
    ASSERT_GT(objCount, 0);

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    const QString reportPath = QDir(outDir.path()).filePath(QStringLiteral("fixreport.json"));

    const QByteArray rootBa   = scanDir.path().toUtf8();
    const QByteArray reportBa = reportPath.toUtf8();

    ScanExcludeArgv args({"qtmesh", "scan", rootBa.constData(),
                          "--fix", "--dry-run",
                          "--report", reportBa.constData(),
                          "--fail-on", "never"});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFileInfo(reportPath).exists());
    QFile rf(reportPath);
    ASSERT_TRUE(rf.open(QIODevice::ReadOnly | QIODevice::Text));
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(rf.readAll(), &perr);
    rf.close();
    ASSERT_EQ(perr.error, QJsonParseError::NoError) << perr.errorString().toStdString();
    ASSERT_TRUE(doc.isObject());

    const QJsonObject root = doc.object();
    ASSERT_TRUE(root.contains(QStringLiteral("summary")));
    const QJsonObject summary = root.value(QStringLiteral("summary")).toObject();
    ASSERT_TRUE(summary.contains(QStringLiteral("scanned")));
    const int expectedMin = objCount + (haveMesh ? 1 : 0);
    EXPECT_GE(summary.value(QStringLiteral("scanned")).toInt(), expectedMin)
        << "scanned=" << summary.value(QStringLiteral("scanned")).toInt();
}

// --fix --dry-run without a --report still completes and exits 0 under
// --fail-on never (exercises the config wiring without the report-path branch).
TEST_F(CLIPipelineScanExcludeOgreFixture, FixDryRunNoReportStillExitsZero)
{
    QTemporaryDir scanDir;
    int objCount = 0; bool haveMesh = false;
    seedScanDir(scanDir, &objCount, &haveMesh);
    ASSERT_GT(objCount, 0);

    const QByteArray rootBa = scanDir.path().toUtf8();
    ScanExcludeArgv args({"qtmesh", "scan", rootBa.constData(),
                          "--fix", "--dry-run",
                          "--fail-on", "never"});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 0);
}

// ---------------------------------------------------------------------------
// --report into a NESTED non-existent subdir: QDir().mkpath() must create the
// missing parent chain (lines ~4423-4424). Assert the file is created and is
// valid JSON with the expected top-level keys.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineScanExcludeOgreFixture, ReportIntoNestedNonexistentDirMakesPath)
{
    QTemporaryDir scanDir;
    int objCount = 0; bool haveMesh = false;
    seedScanDir(scanDir, &objCount, &haveMesh);
    ASSERT_GT(objCount, 0);

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    // Deep chain of directories that do NOT exist yet.
    const QString nestedDir =
        QDir(outDir.path()).filePath(QStringLiteral("a/b/c/deep"));
    EXPECT_FALSE(QFileInfo(nestedDir).exists());
    const QString reportPath = QDir(nestedDir).filePath(QStringLiteral("nested-report.json"));

    const QByteArray rootBa   = scanDir.path().toUtf8();
    const QByteArray reportBa = reportPath.toUtf8();

    ScanExcludeArgv args({"qtmesh", "scan", rootBa.constData(),
                          "--report", reportBa.constData(),
                          "--fail-on", "never"});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 0);

    // mkpath must have created the whole parent chain + the file.
    EXPECT_TRUE(QFileInfo(nestedDir).isDir());
    ASSERT_TRUE(QFileInfo(reportPath).exists());

    QFile rf(reportPath);
    ASSERT_TRUE(rf.open(QIODevice::ReadOnly | QIODevice::Text));
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(rf.readAll(), &perr);
    rf.close();
    ASSERT_EQ(perr.error, QJsonParseError::NoError) << perr.errorString().toStdString();
    ASSERT_TRUE(doc.isObject());
    const QJsonObject root = doc.object();
    EXPECT_TRUE(root.contains(QStringLiteral("summary")));
    EXPECT_TRUE(root.contains(QStringLiteral("assets")));
}

// ---------------------------------------------------------------------------
// --include <bare-ext> AND --exclude <bare-ext> together. Both patterns are
// bare-extension-normalized (~4360-4373). With --include "*.obj" --exclude
// "alpha.obj"-equivalent we keep only beta.obj. Use --include "*.obj" plus
// --exclude "*.mesh" so the kept set is exactly the .obj files: assert the
// report contains only .obj entries (no .mesh, no other formats).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineScanExcludeOgreFixture, IncludeAndExcludeKeepOnlyObj)
{
    QTemporaryDir scanDir;
    int objCount = 0; bool haveMesh = false;
    seedScanDir(scanDir, &objCount, &haveMesh);
    ASSERT_GT(objCount, 0);

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    const QString reportPath = QDir(outDir.path()).filePath(QStringLiteral("ie-report.json"));

    const QByteArray rootBa   = scanDir.path().toUtf8();
    const QByteArray reportBa = reportPath.toUtf8();

    ScanExcludeArgv args({"qtmesh", "scan", rootBa.constData(),
                          "--include", "*.obj",
                          "--exclude", "*.mesh",
                          "--report", reportBa.constData(),
                          "--fail-on", "never"});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFileInfo(reportPath).exists());
    QFile rf(reportPath);
    ASSERT_TRUE(rf.open(QIODevice::ReadOnly | QIODevice::Text));
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(rf.readAll(), &perr);
    rf.close();
    ASSERT_EQ(perr.error, QJsonParseError::NoError) << perr.errorString().toStdString();
    ASSERT_TRUE(doc.isObject());

    const QStringList files = reportAssetFiles(doc.object());
    // Every surviving asset must be an .obj (include kept only *.obj; exclude
    // would have dropped any .mesh anyway).
    for (const QString& f : files)
        EXPECT_TRUE(f.endsWith(QStringLiteral(".obj"), Qt::CaseInsensitive))
            << "unexpected non-obj survivor: " << f.toStdString();
    EXPECT_FALSE(anyFileHasSuffix(files, QStringLiteral(".mesh")))
        << "files: " << files.join(",").toStdString();
    // The two .obj files we wrote should both survive (relative basenames).
    EXPECT_TRUE(files.contains(QStringLiteral("alpha.obj")))
        << "files: " << files.join(",").toStdString();
    EXPECT_TRUE(files.contains(QStringLiteral("beta.obj")))
        << "files: " << files.join(",").toStdString();
}

// ---------------------------------------------------------------------------
// --include "*.obj" --exclude "alpha.obj": include keeps both .obj, then the
// exclude (normalized to **/alpha.obj) drops exactly alpha.obj, leaving only
// beta.obj. Confirms exclude is applied as a filter on top of include and that
// a bare filename (no extension wildcard) is **/ -normalized too.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineScanExcludeOgreFixture, ExcludeSpecificFileOnTopOfInclude)
{
    QTemporaryDir scanDir;
    int objCount = 0; bool haveMesh = false;
    seedScanDir(scanDir, &objCount, &haveMesh);
    ASSERT_EQ(objCount, 2);

    QTemporaryDir outDir;
    ASSERT_TRUE(outDir.isValid());
    const QString reportPath = QDir(outDir.path()).filePath(QStringLiteral("spec-report.json"));

    const QByteArray rootBa   = scanDir.path().toUtf8();
    const QByteArray reportBa = reportPath.toUtf8();

    ScanExcludeArgv args({"qtmesh", "scan", rootBa.constData(),
                          "--include", "*.obj",
                          "--exclude", "alpha.obj",
                          "--report", reportBa.constData(),
                          "--fail-on", "never"});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 0);

    ASSERT_TRUE(QFileInfo(reportPath).exists());
    QFile rf(reportPath);
    ASSERT_TRUE(rf.open(QIODevice::ReadOnly | QIODevice::Text));
    const QJsonDocument doc = QJsonDocument::fromJson(rf.readAll());
    rf.close();
    ASSERT_TRUE(doc.isObject());

    const QStringList files = reportAssetFiles(doc.object());
    EXPECT_FALSE(files.contains(QStringLiteral("alpha.obj")))
        << "alpha.obj should have been excluded; files: " << files.join(",").toStdString();
    EXPECT_TRUE(files.contains(QStringLiteral("beta.obj")))
        << "beta.obj should have survived; files: " << files.join(",").toStdString();
}
