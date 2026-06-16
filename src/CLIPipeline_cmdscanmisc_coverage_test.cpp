// Coverage tests for CLIPipeline::cmdScan — the asset-lint subcommand.
//
// Focus: the CLI numeric *override* plumbing that wires --min-* / --max-* /
// --*-anim-* flags into ScanConfig + the cliRuleOverrides catch-all scope
// (CLIPipeline.cpp lines ~4259-4327). The existing CLIPipeline_test.cpp only
// exercises --max-vertices / --max-file-size-mb / --allowed-formats among the
// numeric overrides (positive value-applied path) plus parse-error branches.
// This suite adds positive value-applied + observable-result coverage for the
// untested min-* and max-bones/submeshes/draw-calls/acmr/anim-* overrides, plus
// the --list-profiles non-empty branch.
//
// Strategy:
//  * min-* overrides fail on a deliberately tiny generated .obj (3 verts, 1 tri,
//    1 mesh, 1 material) — fewer than the requested minimum — and with
//    --fail-on warning the scan returns 1.
//  * max-bones/submeshes/draw-calls/acmr/anim-* overrides fail against a real
//    animated/skinned .fbx asset copied into a temp scan dir, with thresholds
//    set so low the asset always violates them — deterministic exit 1.
//  * --list-profiles returns 0 when built-in profiles are present.
//
// cmdScan loads every asset through the Ogre import path, so this is an Ogre
// fixture: ASSERT_TRUE(tryInitOgre()) + createStandardOgreMaterials() in SetUp
// (NEVER GTEST_SKIP — CI counts a skipped suite as a failure). The single
// QApplication is owned by src/test_main.cpp; we never create another.
//
// Distinct filename + distinct suite names (CLIPipeline_cmdScanMiscCoverage*)
// from CLIPipeline_test.cpp / other cmd*_coverage_test.cpp so there is no ODR
// clash / duplicate test registration.

#include <gtest/gtest.h>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QTemporaryDir>
#include <initializer_list>

#include "CLIPipeline.h"
#include "TestHelpers.h"

namespace {

// RAII argc/argv builder (anonymous-namespace local copy so it does not collide
// with the TestArgv in CLIPipeline_test.cpp's translation unit).
class ScanArgv {
public:
    ScanArgv(std::initializer_list<const char*> args)
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

// Temporarily switch the process current working directory; restores on scope
// exit. cmdScan probes the CWD for a local qtmesh.yml — pointing it at an empty
// temp dir guarantees no stray project config interferes with the override.
class ScanScopedCwd {
public:
    explicit ScanScopedCwd(const QString& path) : m_old(QDir::currentPath())
    {
        QDir::setCurrent(path);
    }
    ~ScanScopedCwd() { QDir::setCurrent(m_old); }

private:
    QString m_old;
};

// Project root: bin -> build_local -> root, then media/models.
QString scanTestDataDir()
{
    QDir dir(QCoreApplication::applicationDirPath());
    dir.cdUp();
    dir.cdUp();
    return dir.absoluteFilePath(QStringLiteral("media/models"));
}

// Write a 1-triangle / 3-vertex / 1-mesh / 1-material .obj into dirPath.
// Returns the absolute path, or empty on failure.
QString writeTinyObj(const QString& dirPath, const QString& fileName)
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

// Copy the first available animated/skinned .fbx asset into dirPath. Returns the
// destination path, or empty if no source asset is present.
QString copyAnimatedAsset(const QString& dirPath, const QString& destName)
{
    const QStringList candidates = {
        QStringLiteral("Rumba Dancing.fbx"),
        QStringLiteral("Twist Dance.fbx"),
        QStringLiteral("Hip Hop Dancing.fbx"),
    };
    for (const QString& c : candidates) {
        const QString src = QDir(scanTestDataDir()).filePath(c);
        if (QFile::exists(src)) {
            const QString dst = QDir(dirPath).filePath(destName);
            QFile::remove(dst);
            if (QFile::copy(src, dst))
                return dst;
        }
    }
    return QString();
}

class CLIPipelineCmdScanMiscCoverage : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        ASSERT_TRUE(canLoadMeshFiles());
        createStandardOgreMaterials();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// --list-profiles: non-empty branch returns 0 and enumerates built-in ids.
// (The empty branch returns 2; built-in profiles ship with the binary so the
// happy path is the observable one here.)
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdScanMiscCoverage, ListProfilesReturnsZero)
{
    ScanArgv args({"qtmesh", "scan", "--list-profiles"});
    EXPECT_EQ(0, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// --list-profiles short-circuits before any scan root is required, so an extra
// positional argument is irrelevant and it still returns 0.
TEST_F(CLIPipelineCmdScanMiscCoverage, ListProfilesIgnoresExtraArgs)
{
    ScanArgv args({"qtmesh", "scan", "--list-profiles", "--json"});
    EXPECT_EQ(0, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --min-vertices override: tiny 3-vertex mesh < min -> warning -> exit 1.
// Exercises config.minVertexCount wiring (lines 4264 / 4309).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdScanMiscCoverage, MinVerticesOverrideReturnsFailure)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    ASSERT_FALSE(writeTinyObj(rootPath, "tiny.obj").isEmpty());

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--min-vertices", "1000", "--fail-on", "warning"});
    EXPECT_EQ(1, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// Same override via the --opt=value form, plus --json (non-JSON cloud promo is
// suppressed in JSON mode, exercising that branch of maybePrintCloudPromo).
TEST_F(CLIPipelineCmdScanMiscCoverage, MinVerticesOverrideWithEqualsAndJsonReturnsFailure)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    ASSERT_FALSE(writeTinyObj(rootPath, "tiny.obj").isEmpty());

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--min-vertices=1000", "--fail-on", "warning", "--json"});
    EXPECT_EQ(1, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// A min-vertices threshold the mesh satisfies should NOT trip on that rule.
// With --fail-on error and a clean tiny mesh the scan returns 0.
TEST_F(CLIPipelineCmdScanMiscCoverage, MinVerticesSatisfiedReturnsZero)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    ASSERT_FALSE(writeTinyObj(rootPath, "tiny.obj").isEmpty());

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--min-vertices", "1", "--fail-on", "error"});
    EXPECT_EQ(0, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --min-meshes override: a sparse dir with 1 mesh < min -> warning -> exit 1.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdScanMiscCoverage, MinMeshesOverrideReturnsFailure)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    ASSERT_FALSE(writeTinyObj(rootPath, "tiny.obj").isEmpty());

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--min-meshes", "50", "--fail-on", "warning"});
    EXPECT_EQ(1, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --min-materials override: 1 material < min -> warning -> exit 1.
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdScanMiscCoverage, MinMaterialsOverrideReturnsFailure)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    ASSERT_FALSE(writeTinyObj(rootPath, "tiny.obj").isEmpty());

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--min-materials", "100", "--fail-on", "warning"});
    EXPECT_EQ(1, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --max-bones override against a real skinned asset: max-bones 0 always fails.
// Exercises config.maxBoneCount wiring (line 4267 / 4313).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdScanMiscCoverage, MaxBonesOverrideReturnsFailure)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    const QString asset = copyAnimatedAsset(rootPath, "skinned.fbx");
    ASSERT_FALSE(asset.isEmpty()) << "No animated/skinned .fbx test asset found";

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--max-bones", "0", "--fail-on", "warning"});
    EXPECT_EQ(1, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --max-submeshes override: max 0 submeshes is impossible -> exit 1.
// Exercises config.maxSubmeshCount wiring (line 4268 / 4314).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdScanMiscCoverage, MaxSubmeshesOverrideReturnsFailure)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    const QString asset = copyAnimatedAsset(rootPath, "skinned.fbx");
    ASSERT_FALSE(asset.isEmpty()) << "No animated/skinned .fbx test asset found";

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--max-submeshes", "0", "--fail-on", "warning"});
    EXPECT_EQ(1, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --max-draw-calls override: max 0 draw calls is impossible -> exit 1.
// Exercises config.maxDrawCalls wiring (line 4269 / 4315).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdScanMiscCoverage, MaxDrawCallsOverrideReturnsFailure)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    const QString asset = copyAnimatedAsset(rootPath, "skinned.fbx");
    ASSERT_FALSE(asset.isEmpty()) << "No animated/skinned .fbx test asset found";

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--max-draw-calls", "0", "--fail-on", "warning"});
    EXPECT_EQ(1, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --max-acmr override: an ACMR cap of 0.0 is below every real mesh's ACMR
// (>= ~0.5 in practice) -> exit 1. Exercises config.maxAcmr wiring
// (line 4270 / 4310, the double override path).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdScanMiscCoverage, MaxAcmrOverrideReturnsFailure)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    const QString asset = copyAnimatedAsset(rootPath, "skinned.fbx");
    ASSERT_FALSE(asset.isEmpty()) << "No animated/skinned .fbx test asset found";

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--max-acmr", "0.0", "--fail-on", "warning"});
    EXPECT_EQ(1, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --min-anim-keyframes override against an animated asset: a 1,000,000-keyframe
// floor is unreachable -> exit 1. Exercises config.minAnimKeyframes wiring
// (line 4272 / 4317).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdScanMiscCoverage, MinAnimKeyframesOverrideReturnsFailure)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    const QString asset = copyAnimatedAsset(rootPath, "anim.fbx");
    ASSERT_FALSE(asset.isEmpty()) << "No animated .fbx test asset found";

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--min-anim-keyframes", "1000000", "--fail-on", "warning"});
    EXPECT_EQ(1, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --max-anim-duration override: a 0.0001s cap is below any real clip duration
// -> exit 1. Exercises config.maxAnimDuration wiring (line 4273 / 4318, double).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdScanMiscCoverage, MaxAnimDurationOverrideReturnsFailure)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    const QString asset = copyAnimatedAsset(rootPath, "anim.fbx");
    ASSERT_FALSE(asset.isEmpty()) << "No animated .fbx test asset found";

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--max-anim-duration", "0.0001", "--fail-on", "warning"});
    EXPECT_EQ(1, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --min-anim-duration override: a 100000s floor is unreachable -> exit 1.
// Exercises config.minAnimDuration wiring (line 4274 / 4319, double).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdScanMiscCoverage, MinAnimDurationOverrideReturnsFailure)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    const QString asset = copyAnimatedAsset(rootPath, "anim.fbx");
    ASSERT_FALSE(asset.isEmpty()) << "No animated .fbx test asset found";

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--min-anim-duration", "100000", "--fail-on", "warning"});
    EXPECT_EQ(1, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --max-anim-keyframes override: a 0-keyframe cap is below any animated clip
// -> exit 1. Exercises config.maxAnimKeyframes wiring (line 4271 / 4316).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdScanMiscCoverage, MaxAnimKeyframesOverrideReturnsFailure)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    const QString asset = copyAnimatedAsset(rootPath, "anim.fbx");
    ASSERT_FALSE(asset.isEmpty()) << "No animated .fbx test asset found";

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--max-anim-keyframes", "0", "--fail-on", "warning"});
    EXPECT_EQ(1, CLIPipeline::cmdScan(args.argc(), args.argv()));
}

// ---------------------------------------------------------------------------
// --fail-on never short-circuits the scan exit even when an override is
// violated, returning 0 (covers the failOn=="never" branch of the exit logic
// alongside an applied min override).
// ---------------------------------------------------------------------------
TEST_F(CLIPipelineCmdScanMiscCoverage, MinVerticesViolatedButFailOnNeverReturnsZero)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    ScanScopedCwd cwd(tmpDir.path());

    const QString rootPath = QDir(tmpDir.path()).filePath("assets");
    ASSERT_TRUE(QDir().mkpath(rootPath));
    ASSERT_FALSE(writeTinyObj(rootPath, "tiny.obj").isEmpty());

    QByteArray rootBa = rootPath.toUtf8();
    ScanArgv args({"qtmesh", "scan", rootBa.constData(),
                   "--min-vertices", "1000", "--fail-on", "never"});
    EXPECT_EQ(0, CLIPipeline::cmdScan(args.argc(), args.argv()));
}
