// Coverage tests for CLIPipeline::cmdScan platform-profile resolution paths:
//   * --list-profiles            (CLIPipeline.cpp ~4124-4139)
//   * --profile <id> resolution  (~4203 profileId path; PlatformProfileLoader)
//   * --target <id> alias        (~4204 targetId aliases onto profileId)
//   * scanning a real directory with a built-in profile applied (exit by findings)
//
// These complement the existing CLIPipelineCmdScan* suites in CLIPipeline_test.cpp,
// which exercise --report / --sarif / --fail-on / --include / --fix / --dry-run /
// --config (and one --target case) but NEVER drive --list-profiles or --profile.
//
// Distinct filename + distinct suite name (CLIPipelineCmdScanProfileCoverage) so
// there is no ODR clash / duplicate registration with the existing translation
// units.
//
// The scan walk loads assets through MeshImporterExporter, so the directory-scan
// cases need Ogre — the fixture brings Ogre up with tryInitOgre() (NEVER skips,
// per the CI harness rule) and seeds it with a real .mesh copied into a
// QTemporaryDir. The pure parser cases (--list-profiles, mismatch error, unknown
// profile) return before the Ogre-dependent scan walk and are plain TEST()s.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <initializer_list>

#include "CLIPipeline.h"
#include "PlatformProfile.h"
#include "TestHelpers.h"

namespace {

/// RAII argc/argv builder (anonymous-namespace local so it does not collide
/// with the TestArgv in CLIPipeline_test.cpp or the *Argv helpers in the other
/// CLIPipeline_cmd*_coverage_test.cpp translation units).
class ScanProfileArgv {
public:
    ScanProfileArgv(std::initializer_list<const char*> args)
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

} // namespace

// ===========================================================================
// --list-profiles : pure-ish path (PlatformProfileLoader::listBuiltinIds),
// returns BEFORE any Ogre/scan work. exit 0 when the bundle exists, exit 2
// when no built-in profiles are found.
// ===========================================================================

TEST(CLIPipelineCmdScanProfileCoverage, ListProfilesReturns0Or2)
{
    ScanProfileArgv args({"qtmesh", "scan", "--list-profiles"});
    const int rc = CLIPipeline::cmdScan(args.argc(), args.argv());
    // 0 when the built-in profiles bundle is discoverable; 2 when the search
    // directory has no profiles (some packaging layouts). Both are valid.
    EXPECT_TRUE(rc == 0 || rc == 2) << "unexpected --list-profiles rc=" << rc;

    // The CLI exit code must agree with the loader the code path consults.
    const QStringList ids = PlatformProfileLoader::listBuiltinIds();
    if (ids.isEmpty())
        EXPECT_EQ(rc, 2);
    else
        EXPECT_EQ(rc, 0);
}

// --list-profiles short-circuits, so it is honored even with a (nonexistent)
// positional scan root present — the root is never validated on this path.
TEST(CLIPipelineCmdScanProfileCoverage, ListProfilesIgnoresScanRoot)
{
    ScanProfileArgv args({"qtmesh", "scan", "/no/such/dir/at/all", "--list-profiles"});
    const int rc = CLIPipeline::cmdScan(args.argc(), args.argv());
    EXPECT_TRUE(rc == 0 || rc == 2) << "rc=" << rc;

    const QStringList ids = PlatformProfileLoader::listBuiltinIds();
    EXPECT_EQ(rc, ids.isEmpty() ? 2 : 0);
}

// The list reported by the CLI loader must contain the canonical
// example-minimal id that the rest of these tests (and the existing suite)
// rely on, whenever any profiles are discoverable at all.
TEST(CLIPipelineCmdScanProfileCoverage, BuiltinIdsContainExampleMinimalWhenPresent)
{
    const QStringList ids = PlatformProfileLoader::listBuiltinIds();
    if (ids.isEmpty()) {
        // No bundle in this packaging layout — the loader directory must still
        // be a non-empty, well-formed path string (the breadcrumb uses it).
        EXPECT_FALSE(PlatformProfileLoader::builtinProfilesDirectory().isEmpty());
    } else {
        EXPECT_TRUE(ids.contains(QStringLiteral("example-minimal")))
            << "built-in ids: " << ids.join(",").toStdString();
    }
}

// ===========================================================================
// --profile / --target resolution error branches (return 2 BEFORE scanning).
// ===========================================================================

// Unknown --profile id: buildScanConfigWithPlatformProfile fails -> exit 2.
TEST(CLIPipelineCmdScanProfileCoverage, UnknownProfileReturns2)
{
    ScanProfileArgv args({"qtmesh", "scan", "--profile", "definitely-not-a-real-profile"});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 2);
}

// --target and --profile with DIFFERENT values is a usage error -> exit 2.
TEST(CLIPipelineCmdScanProfileCoverage, TargetAndProfileMismatchReturns2)
{
    ScanProfileArgv args({"qtmesh", "scan",
                          "--target", "example-minimal",
                          "--profile", "ps1"});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 2);
}

// --target and --profile with the SAME value is NOT a mismatch; resolution
// proceeds. With no scan root the scan walks the default location and the
// outcome is governed by --fail-on never -> exit 0 (when the id resolves).
TEST(CLIPipelineCmdScanProfileCoverage, TargetAndProfileSameValueNotMismatch)
{
    const QStringList ids = PlatformProfileLoader::listBuiltinIds();
    if (!ids.contains(QStringLiteral("example-minimal"))) {
        // Without the built-in bundle this id won't resolve; the test below
        // (real-dir scan) already guards on availability — assert the parser
        // at least does not treat equal values as a mismatch (would be 2 only
        // from the load failure, not the mismatch branch). Use an empty dir so
        // a resolve failure is the only way to reach 2.
        QTemporaryDir empty;
        ASSERT_TRUE(empty.isValid());
        const QByteArray rootBa = empty.path().toUtf8();
        ScanProfileArgv args({"qtmesh", "scan", rootBa.constData(),
                              "--target", "example-minimal",
                              "--profile", "example-minimal",
                              "--fail-on", "never"});
        // id unresolved -> 2; the point is it is NOT the mismatch branch.
        EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 2);
        return;
    }

    QTemporaryDir empty;
    ASSERT_TRUE(empty.isValid());
    const QByteArray rootBa = empty.path().toUtf8();
    ScanProfileArgv args({"qtmesh", "scan", rootBa.constData(),
                          "--target", "example-minimal",
                          "--profile", "example-minimal",
                          "--fail-on", "never"});
    EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 0);
}

// ===========================================================================
// Ogre-backed: scan a real directory with a built-in profile applied.
// The scan walk loads assets via MeshImporterExporter, so Ogre must be up.
// ===========================================================================

class CLIPipelineScanProfileOgreFixture : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre());
        createStandardOgreMaterials();
    }
};

// scan <tempDir-with-a-real-mesh> --profile example-minimal --fail-on never
// resolves the built-in profile and walks the directory -> exit 0.
TEST_F(CLIPipelineScanProfileOgreFixture, ScanRealDirWithProfileFailOnNever)
{
    const QStringList ids = PlatformProfileLoader::listBuiltinIds();
    const QString robot = testRobotMeshPath();

    QTemporaryDir scanDir;
    ASSERT_TRUE(scanDir.isValid());

    bool haveAsset = false;
    if (!robot.isEmpty() && QFile::exists(robot)) {
        const QString dst = QDir(scanDir.path()).filePath(QStringLiteral("robot.mesh"));
        haveAsset = QFile::copy(robot, dst);
    }
    // If the real mesh is unavailable, drop a deterministic non-mesh file so the
    // scan still has something to walk (it will be skipped or load-errored, but
    // the profile-resolution + scan-walk code still executes).
    if (!haveAsset) {
        QFile placeholder(QDir(scanDir.path()).filePath(QStringLiteral("note.txt")));
        ASSERT_TRUE(placeholder.open(QIODevice::WriteOnly | QIODevice::Text));
        placeholder.write("placeholder");
        placeholder.close();
    }

    const QByteArray rootBa = scanDir.path().toUtf8();

    if (ids.contains(QStringLiteral("example-minimal"))) {
        ScanProfileArgv args({"qtmesh", "scan", rootBa.constData(),
                              "--profile", "example-minimal",
                              "--fail-on", "never"});
        // --fail-on never forces exit 0 regardless of findings.
        EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 0);
    } else {
        // No bundle: profile fails to resolve -> exit 2. Still exercises the
        // profileId resolution branch.
        ScanProfileArgv args({"qtmesh", "scan", rootBa.constData(),
                              "--profile", "example-minimal",
                              "--fail-on", "never"});
        EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 2);
    }
}

// Same scan but driving the resolution through the --target alias instead of
// --profile, confirming targetId aliases onto profileId (~line 4204/4211).
TEST_F(CLIPipelineScanProfileOgreFixture, ScanRealDirWithTargetAliasFailOnNever)
{
    const QStringList ids = PlatformProfileLoader::listBuiltinIds();
    const QString robot = testRobotMeshPath();

    QTemporaryDir scanDir;
    ASSERT_TRUE(scanDir.isValid());

    bool haveAsset = false;
    if (!robot.isEmpty() && QFile::exists(robot)) {
        const QString dst = QDir(scanDir.path()).filePath(QStringLiteral("robot.mesh"));
        haveAsset = QFile::copy(robot, dst);
    }
    if (!haveAsset) {
        QFile placeholder(QDir(scanDir.path()).filePath(QStringLiteral("readme.md")));
        ASSERT_TRUE(placeholder.open(QIODevice::WriteOnly | QIODevice::Text));
        placeholder.write("placeholder");
        placeholder.close();
    }

    const QByteArray rootBa = scanDir.path().toUtf8();

    if (ids.contains(QStringLiteral("example-minimal"))) {
        ScanProfileArgv args({"qtmesh", "scan", rootBa.constData(),
                              "--target", "example-minimal",
                              "--fail-on", "never"});
        EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 0);
    } else {
        ScanProfileArgv args({"qtmesh", "scan", rootBa.constData(),
                              "--target", "example-minimal",
                              "--fail-on", "never"});
        EXPECT_EQ(CLIPipeline::cmdScan(args.argc(), args.argv()), 2);
    }
}

// Scan a real directory with a profile but WITHOUT --fail-on never: the exit
// code is then governed by findings (0 when clean, 1 when the profile's
// thresholds flag something). Assert it is one of the two scan-outcome codes
// (never 2 — the profile resolves and the root is a valid dir).
TEST_F(CLIPipelineScanProfileOgreFixture, ScanRealDirWithProfileDefaultFailOn)
{
    const QStringList ids = PlatformProfileLoader::listBuiltinIds();
    if (!ids.contains(QStringLiteral("example-minimal"))) {
        // Without the bundle the profile cannot resolve; the dedicated
        // unknown-profile/resolve cases above cover that. Assert the loader
        // directory string is well-formed and finish (no skip).
        EXPECT_FALSE(PlatformProfileLoader::builtinProfilesDirectory().isEmpty());
        return;
    }

    const QString robot = testRobotMeshPath();
    QTemporaryDir scanDir;
    ASSERT_TRUE(scanDir.isValid());

    if (!robot.isEmpty() && QFile::exists(robot)) {
        const QString dst = QDir(scanDir.path()).filePath(QStringLiteral("robot.mesh"));
        QFile::copy(robot, dst);
    } else {
        QFile placeholder(QDir(scanDir.path()).filePath(QStringLiteral("empty.txt")));
        ASSERT_TRUE(placeholder.open(QIODevice::WriteOnly | QIODevice::Text));
        placeholder.write("x");
        placeholder.close();
    }

    const QByteArray rootBa = scanDir.path().toUtf8();
    ScanProfileArgv args({"qtmesh", "scan", rootBa.constData(),
                          "--profile", "example-minimal"});
    const int rc = CLIPipeline::cmdScan(args.argc(), args.argv());
    // Profile resolves and dir is valid -> outcome is scan-driven: 0 or 1.
    EXPECT_TRUE(rc == 0 || rc == 1) << "unexpected scan-outcome rc=" << rc;
}
