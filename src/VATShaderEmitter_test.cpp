/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Unit tests for VATShaderEmitter — pure-data engine-list parsing + shader
template emission. No Ogre, no display, no QApplication dependency.

NOTE on Qt resources: the per-engine shader templates live under the Qt
resource prefix `:/vat-shaders/`, compiled from tools/vat-shaders/vat_shaders.qrc.
The unit-test binary does NOT compile that .qrc (see tests/CMakeLists.txt),
so the resource files may be absent at runtime. Tests that depend on the
resource content (i.e. tests that assert files were actually written) are
gated with a runtime availability probe and GTEST_SKIP when the resource is
missing — the parseEngineList logic and the empty-input / mkpath early-return
branches of writeShaders are fully exercised regardless.
-----------------------------------------------------------------------------------
*/

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "VATShaderEmitter.h"

namespace {

// Returns true when the bundled VAT shader resources are compiled into
// this binary (i.e. `:/vat-shaders/openvat.gdshader` can be opened). The
// unit-test target typically does not include vat_shaders.qrc, so this
// guards the file-writing assertions.
bool vatResourcesAvailable()
{
    QFile probe(QStringLiteral(":/vat-shaders/openvat.gdshader"));
    return probe.exists() && probe.open(QIODevice::ReadOnly);
}

} // namespace

// ---------------------------------------------------------------------------
// parseEngineList — pure logic, no resources required
// ---------------------------------------------------------------------------

TEST(VATShaderEmitterParse, EmptyInputReturnsEmptyList) {
    QStringList rejected;
    rejected << "stale";  // verify it gets cleared
    EXPECT_TRUE(VATShaderEmitter::parseEngineList(QString(), &rejected).isEmpty());
    EXPECT_TRUE(rejected.isEmpty());
}

TEST(VATShaderEmitterParse, WhitespaceOnlyReturnsEmptyList) {
    QStringList rejected;
    EXPECT_TRUE(VATShaderEmitter::parseEngineList(QStringLiteral("   \t  "),
                                                  &rejected).isEmpty());
    EXPECT_TRUE(rejected.isEmpty());
}

TEST(VATShaderEmitterParse, CommasOnlyReturnsEmptyList) {
    // SkipEmptyParts + per-token trim means nothing is collected.
    EXPECT_TRUE(VATShaderEmitter::parseEngineList(QStringLiteral(", , ,"))
                    .isEmpty());
}

TEST(VATShaderEmitterParse, AllExpandsToThreeInStableOrder) {
    const QStringList out = VATShaderEmitter::parseEngineList(QStringLiteral("all"));
    ASSERT_EQ(out.size(), 3);
    EXPECT_EQ(out[0], QStringLiteral("godot"));
    EXPECT_EQ(out[1], QStringLiteral("unity"));
    EXPECT_EQ(out[2], QStringLiteral("unreal"));
}

TEST(VATShaderEmitterParse, AllIsCaseInsensitive) {
    EXPECT_EQ(VATShaderEmitter::parseEngineList(QStringLiteral("ALL")).size(), 3);
    EXPECT_EQ(VATShaderEmitter::parseEngineList(QStringLiteral("All")).size(), 3);
    EXPECT_EQ(VATShaderEmitter::parseEngineList(QStringLiteral("  aLl  ")).size(), 3);
}

TEST(VATShaderEmitterParse, SingleEngine) {
    const QStringList out = VATShaderEmitter::parseEngineList(QStringLiteral("unity"));
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0], QStringLiteral("unity"));
}

TEST(VATShaderEmitterParse, CaseInsensitiveTokenMatching) {
    const QStringList out =
        VATShaderEmitter::parseEngineList(QStringLiteral("GODOT,Unity,UnReAl"));
    ASSERT_EQ(out.size(), 3);
    EXPECT_EQ(out[0], QStringLiteral("godot"));
    EXPECT_EQ(out[1], QStringLiteral("unity"));
    EXPECT_EQ(out[2], QStringLiteral("unreal"));
}

TEST(VATShaderEmitterParse, DedupesRepeatedTokens) {
    const QStringList out =
        VATShaderEmitter::parseEngineList(QStringLiteral("godot,godot,godot"));
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0], QStringLiteral("godot"));
}

TEST(VATShaderEmitterParse, DedupesAcrossCasing) {
    const QStringList out =
        VATShaderEmitter::parseEngineList(QStringLiteral("Godot,GODOT,godot"));
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0], QStringLiteral("godot"));
}

TEST(VATShaderEmitterParse, StableOutputOrderRegardlessOfInputOrder) {
    // Input reversed; output must still be canonical godot,unity,unreal.
    const QStringList out =
        VATShaderEmitter::parseEngineList(QStringLiteral("unreal,godot"));
    ASSERT_EQ(out.size(), 2);
    EXPECT_EQ(out[0], QStringLiteral("godot"));
    EXPECT_EQ(out[1], QStringLiteral("unreal"));
}

TEST(VATShaderEmitterParse, StableOrderFullShuffle) {
    const QStringList out =
        VATShaderEmitter::parseEngineList(QStringLiteral("unity,unreal,godot"));
    ASSERT_EQ(out.size(), 3);
    EXPECT_EQ(out[0], QStringLiteral("godot"));
    EXPECT_EQ(out[1], QStringLiteral("unity"));
    EXPECT_EQ(out[2], QStringLiteral("unreal"));
}

TEST(VATShaderEmitterParse, UnknownTokensDroppedAndSurfacedWithOriginalCasing) {
    QStringList rejected;
    const QStringList out =
        VATShaderEmitter::parseEngineList(QStringLiteral("godot,Blender"), &rejected);
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0], QStringLiteral("godot"));
    ASSERT_EQ(rejected.size(), 1);
    // Original casing preserved (trimmed only).
    EXPECT_EQ(rejected[0], QStringLiteral("Blender"));
}

TEST(VATShaderEmitterParse, MultipleUnknownTokensSurfaced) {
    QStringList rejected;
    const QStringList out = VATShaderEmitter::parseEngineList(
        QStringLiteral(" Foo , unity , BAR "), &rejected);
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0], QStringLiteral("unity"));
    ASSERT_EQ(rejected.size(), 2);
    EXPECT_EQ(rejected[0], QStringLiteral("Foo"));
    EXPECT_EQ(rejected[1], QStringLiteral("BAR"));
}

TEST(VATShaderEmitterParse, UnknownTokensSilentlyDroppedWhenRejectedNull) {
    // Null rejectedOut must be tolerated and not crash.
    const QStringList out =
        VATShaderEmitter::parseEngineList(QStringLiteral("godot,blender"), nullptr);
    ASSERT_EQ(out.size(), 1);
    EXPECT_EQ(out[0], QStringLiteral("godot"));
}

TEST(VATShaderEmitterParse, RejectedOutClearedOnEntry) {
    QStringList rejected;
    rejected << "leftover1" << "leftover2";
    // All-valid input → rejected must be empty afterwards (cleared on entry).
    const QStringList out =
        VATShaderEmitter::parseEngineList(QStringLiteral("godot,unity"), &rejected);
    EXPECT_EQ(out.size(), 2);
    EXPECT_TRUE(rejected.isEmpty());
}

TEST(VATShaderEmitterParse, AllPlusExtraTokensStillThree) {
    QStringList rejected;
    const QStringList out =
        VATShaderEmitter::parseEngineList(QStringLiteral("all,godot,unity"), &rejected);
    EXPECT_EQ(out.size(), 3);
    EXPECT_TRUE(rejected.isEmpty());
}

TEST(VATShaderEmitterParse, ConstantsMatchCanonicalIds) {
    // Sanity: the public constants are the lowercase ids parse emits.
    EXPECT_STREQ(VATShaderEmitter::kGodot, "godot");
    EXPECT_STREQ(VATShaderEmitter::kUnity, "unity");
    EXPECT_STREQ(VATShaderEmitter::kUnreal, "unreal");
}

// ---------------------------------------------------------------------------
// writeShaders — early-return branches (no resources required)
// ---------------------------------------------------------------------------

TEST(VATShaderEmitterWrite, EmptyOutputDirReturnsEmpty) {
    EXPECT_TRUE(VATShaderEmitter::writeShaders(
                    QString(), QStringList{QStringLiteral("godot")})
                    .isEmpty());
}

TEST(VATShaderEmitterWrite, EmptyEnginesReturnsEmpty) {
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    EXPECT_TRUE(VATShaderEmitter::writeShaders(tmp.path(), QStringList{})
                    .isEmpty());
}

TEST(VATShaderEmitterWrite, BothEmptyReturnsEmpty) {
    EXPECT_TRUE(VATShaderEmitter::writeShaders(QString(), QStringList{})
                    .isEmpty());
}

// ---------------------------------------------------------------------------
// writeShaders — resource-dependent file emission (gated)
// ---------------------------------------------------------------------------

TEST(VATShaderEmitterWrite, WritesPerEngineFilesAndReturnsAbsolutePaths) {
    if (!vatResourcesAvailable())
        GTEST_SKIP() << "VAT shader Qt resources not compiled into test binary";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QStringList written = VATShaderEmitter::writeShaders(
        tmp.path(), QStringList{QStringLiteral("godot")});

    // godot file + README.
    ASSERT_EQ(written.size(), 2);
    for (const QString& p : written) {
        EXPECT_TRUE(QFileInfo(p).isAbsolute()) << p.toStdString();
        EXPECT_TRUE(QFile::exists(p)) << p.toStdString();
    }
    // The gdshader must be present on disk.
    EXPECT_TRUE(QFile::exists(tmp.filePath(QStringLiteral("openvat.gdshader"))));
}

TEST(VATShaderEmitterWrite, AllEnginesWritesThreeShadersPlusReadme) {
    if (!vatResourcesAvailable())
        GTEST_SKIP() << "VAT shader Qt resources not compiled into test binary";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QStringList written = VATShaderEmitter::writeShaders(
        tmp.path(),
        QStringList{QStringLiteral("godot"), QStringLiteral("unity"),
                    QStringLiteral("unreal")});

    EXPECT_EQ(written.size(), 4);  // 3 engines + README
    EXPECT_TRUE(QFile::exists(tmp.filePath(QStringLiteral("openvat.gdshader"))));
    EXPECT_TRUE(QFile::exists(tmp.filePath(QStringLiteral("openvat.shader"))));
    EXPECT_TRUE(QFile::exists(tmp.filePath(QStringLiteral("openvat.usf"))));
    EXPECT_TRUE(QFile::exists(tmp.filePath(QStringLiteral("OpenVAT_README.md"))));
}

TEST(VATShaderEmitterWrite, ReadmeEmittedWhenAtLeastOneEngineWritten) {
    if (!vatResourcesAvailable())
        GTEST_SKIP() << "VAT shader Qt resources not compiled into test binary";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QStringList written = VATShaderEmitter::writeShaders(
        tmp.path(), QStringList{QStringLiteral("unity")});

    // README must be present in the returned list.
    bool sawReadme = false;
    for (const QString& p : written)
        if (QFileInfo(p).fileName() == QStringLiteral("OpenVAT_README.md"))
            sawReadme = true;
    EXPECT_TRUE(sawReadme);
    EXPECT_TRUE(QFile::exists(tmp.filePath(QStringLiteral("OpenVAT_README.md"))));
}

TEST(VATShaderEmitterWrite, NoReadmeWhenOnlyUnknownEnginesRequested) {
    // Unknown engine names are skipped → nothing written → no README.
    // This branch does not touch the resource (the spec loop finds no match),
    // so it is testable even without the .qrc compiled in.
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    const QStringList written = VATShaderEmitter::writeShaders(
        tmp.path(),
        QStringList{QStringLiteral("blender"), QStringLiteral("maya")});

    EXPECT_TRUE(written.isEmpty());
    EXPECT_FALSE(QFile::exists(tmp.filePath(QStringLiteral("OpenVAT_README.md"))));
}

TEST(VATShaderEmitterWrite, DedupesAndLowercasesCallerEngines) {
    if (!vatResourcesAvailable())
        GTEST_SKIP() << "VAT shader Qt resources not compiled into test binary";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // Mixed case + duplicates + an unknown — should resolve to a single
    // godot shader + README.
    const QStringList written = VATShaderEmitter::writeShaders(
        tmp.path(),
        QStringList{QStringLiteral("GODOT"), QStringLiteral("  godot  "),
                    QStringLiteral("Godot"), QStringLiteral("blender")});

    EXPECT_EQ(written.size(), 2);  // godot + README, deduped
    EXPECT_TRUE(QFile::exists(tmp.filePath(QStringLiteral("openvat.gdshader"))));
}

TEST(VATShaderEmitterWrite, CreatesOutputDirectoryWhenMissing) {
    if (!vatResourcesAvailable())
        GTEST_SKIP() << "VAT shader Qt resources not compiled into test binary";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // Nested, non-existent subdir → exercises the mkpath branch.
    const QString nested =
        QDir(tmp.path()).filePath(QStringLiteral("a/b/c/out"));
    EXPECT_FALSE(QDir(nested).exists());

    const QStringList written = VATShaderEmitter::writeShaders(
        nested, QStringList{QStringLiteral("godot")});

    EXPECT_TRUE(QDir(nested).exists());
    EXPECT_EQ(written.size(), 2);
    EXPECT_TRUE(QFile::exists(QDir(nested).filePath(QStringLiteral("openvat.gdshader"))));
}

TEST(VATShaderEmitterWrite, IdempotentOverwriteOfExistingFiles) {
    if (!vatResourcesAvailable())
        GTEST_SKIP() << "VAT shader Qt resources not compiled into test binary";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    // Pre-create a stale gdshader with junk content.
    const QString stalePath = tmp.filePath(QStringLiteral("openvat.gdshader"));
    {
        QFile f(stalePath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("STALE-CONTENT-SHOULD-BE-OVERWRITTEN");
        f.close();
    }

    const QStringList first = VATShaderEmitter::writeShaders(
        tmp.path(), QStringList{QStringLiteral("godot")});
    ASSERT_EQ(first.size(), 2);

    // Stale content must be gone.
    {
        QFile f(stalePath);
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        const QByteArray bytes = f.readAll();
        f.close();
        EXPECT_FALSE(bytes.contains("STALE-CONTENT-SHOULD-BE-OVERWRITTEN"));
        EXPECT_GT(bytes.size(), 0);
    }

    // Second write is idempotent — same returned paths, no crash/error.
    const QStringList second = VATShaderEmitter::writeShaders(
        tmp.path(), QStringList{QStringLiteral("godot")});
    EXPECT_EQ(first, second);

    // Captured bytes match the canonical resource.
    QFile res(QStringLiteral(":/vat-shaders/openvat.gdshader"));
    ASSERT_TRUE(res.open(QIODevice::ReadOnly));
    const QByteArray resBytes = res.readAll();
    res.close();
    QFile written(stalePath);
    ASSERT_TRUE(written.open(QIODevice::ReadOnly));
    EXPECT_EQ(written.readAll(), resBytes);
}
