// Coverage tests for CLIPipeline::cmdAnim --merge, specifically the deep merge
// body AND the export -> RE-IMPORT verification contract.
//
// The existing valid merge tests in CLIPipeline_test.cpp
// (CmdAnimMerge_Valid, CmdAnimMerge_MultipleFiles) only assert
// QFile::exists() on the produced output — they never re-import the merged
// file to prove the union of source animations actually reached the wire.
// They also only target .mesh output. This leaves the following gaps in
// CLIPipeline.cpp cmdAnim merge mode (~1888-1938):
//   - The export RESULT check + RE-IMPORT of the merged file: prove that
//     AnimationMerger::mergeAnimations()'s result (the union of base + source
//     animations) actually survived export, i.e. the merged skeleton's
//     animation count INCREASED over the bare base file (lines 1919-1937).
//   - The .fbx output branch (line 1929 -> FBXExporter merge-node export via
//     formatForExtension) vs the .mesh-only existing valid merge tests.
//   - The allEntities.size() >= 2 path with the merged entity's parent scene
//     node export (merged->getParentSceneNode() at line 1927) — exercised
//     here by merging two real mesh-bearing FBX files (base + source mesh
//     entity) so allEntities holds >= 2 entities.
//
// Assets: media/models/Twist Dance.fbx (base, has mesh + skeleton + anim) and
// media/models/Hip Hop Dancing.fbx (merge source, also mesh + skeleton + anim)
// — the exact pair the existing CmdAnimMerge_Valid test uses, so they are
// already cached by warmup in this process and we avoid loading a *third*
// distinct Mixamo skeleton (which would risk an Ogre skeleton-name collision).
//
// All identifiers here are deliberately distinct (separate anonymous namespace,
// _MergeRoundTrip-suffixed suite name, local RAII argv copy + local data-dir
// helper) to avoid ODR clashes / duplicate registration with the other cmdAnim
// suites. NEVER GTEST_SKIP — SetUp uses ASSERT_TRUE(tryInitOgre()).

#include <gtest/gtest.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QCoreApplication>
#include <vector>

#include "CLIPipeline.h"
#include "MeshImporterExporter.h"
#include "Manager.h"
#include "TestHelpers.h"

namespace {

// RAII helper to build argc/argv from a list of strings (self-contained copy,
// matching the AnimArgv pattern in CLIPipeline_cmdanimroundtrip_coverage_test.cpp).
class MergeArgv {
public:
    MergeArgv(std::initializer_list<const char*> args)
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

// Local copy of the project root media/models resolver (CLIPipeline_test.cpp
// has its own file-static testDataDir(); we cannot reuse it across TUs).
QString mergeTestDataDir()
{
    QDir dir(QCoreApplication::applicationDirPath());
    dir.cdUp(); // bin -> build_local
    dir.cdUp(); // build_local -> project root
    return dir.absoluteFilePath("media/models");
}

// Destroy every scene node + attached movable object so each sub-run starts
// from a clean Manager (avoids skeleton-name collisions on repeated imports).
void clearMergeScene()
{
    if (!Manager::getSingletonPtr())
        return;
    auto nodes = Manager::getSingleton()->getSceneNodes(); // copy
    for (auto* node : nodes) {
        Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
        Manager::getSingleton()->destroySceneNode(node);
    }
}

// Import a produced file and report (animation count, total length, name set).
// Clears the scene afterwards. numAnims == 0 when the import produced no
// skinned entity.
struct MergeAnimSummary {
    unsigned short numAnims = 0;
    float totalLength = 0.0f;
    QStringList names;
    bool imported = false;
};

MergeAnimSummary reimportMergeSummary(const QString& filePath)
{
    MergeAnimSummary s;
    if (!Manager::getSingletonPtr())
        return s;

    MeshImporterExporter::importer({filePath});
    auto& entities = Manager::getSingleton()->getEntities();
    if (!entities.isEmpty() && entities.first()->hasSkeleton()) {
        Ogre::SkeletonPtr skel = entities.first()->getMesh()->getSkeleton();
        if (skel) {
            s.imported = true;
            s.numAnims = skel->getNumAnimations();
            for (unsigned short i = 0; i < s.numAnims; ++i) {
                auto* anim = skel->getAnimation(i);
                s.totalLength += anim->getLength();
                s.names << QString::fromStdString(anim->getName());
            }
        }
    }
    clearMergeScene();
    return s;
}

} // anonymous namespace

class CLIPipelineCmdAnimMergeRoundTripCoverageTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        if (!tryInitOgre()) return;
        createStandardOgreMaterials();
        // Warm up the import pipeline once: the first import in a process can
        // fail due to lazy plugin/resource init. Warm both merge inputs so the
        // later in-test imports reuse cached Ogre meshes/skeletons (mirrors the
        // existing CmdAnimMerge_MultipleFiles caching rationale).
        CLIPipeline::initOgreHeadless();
        const QString base = mergeTestDataDir() + "/Twist Dance.fbx";
        const QString src  = mergeTestDataDir() + "/Hip Hop Dancing.fbx";
        if (QFile::exists(base)) { MeshImporterExporter::importer({base}); clearMergeScene(); }
        if (QFile::exists(src))  { MeshImporterExporter::importer({src});  clearMergeScene(); }
    }

    void SetUp() override {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        clearMergeScene();
    }

    void TearDown() override {
        clearMergeScene();
    }

    QString baseFbx() const { return mergeTestDataDir() + "/Twist Dance.fbx"; }
    QString srcFbx()  const { return mergeTestDataDir() + "/Hip Hop Dancing.fbx"; }
};

// ---------------------------------------------------------------------------
// Baseline: establish the bare base file's animation inventory so the merge
// round-trip assertions have a reference. Also exercises the reimport helper.
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimMergeRoundTripCoverageTest, Baseline_BaseFbxHasAnimations)
{
    const QString base = baseFbx();
    ASSERT_TRUE(QFile::exists(base)) << "Twist Dance.fbx not found: " << base.toStdString();

    const MergeAnimSummary b = reimportMergeSummary(base);
    ASSERT_TRUE(b.imported) << "base FBX should import a skinned entity";
    EXPECT_GT(b.numAnims, 0) << "base FBX should carry at least one skeletal animation";
    EXPECT_GT(b.totalLength, 0.0f);
}

// ---------------------------------------------------------------------------
// --merge <src.fbx> -> .mesh output, then RE-IMPORT and assert the merged
// skeleton's animation count GREW vs the bare base file. This proves
// AnimationMerger::mergeAnimations()'s union result reached the wire
// (the existing CmdAnimMerge_Valid only checks QFile::exists).
//
// Drives cmdAnim merge body lines 1896-1937 end-to-end: source import loop,
// allEntities.size() >= 2 path, merged->getParentSceneNode() (line 1927),
// the exporter result check (line 1929-1934), and the success cliWrite.
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimMergeRoundTripCoverageTest, MergeToMesh_ReimportShowsUnionGrew)
{
    const QString base = baseFbx();
    const QString src  = srcFbx();
    ASSERT_TRUE(QFile::exists(base)) << "Twist Dance.fbx not found";
    ASSERT_TRUE(QFile::exists(src))  << "Hip Hop Dancing.fbx not found";

    // Reference: bare base animation count.
    const MergeAnimSummary baseSummary = reimportMergeSummary(base);
    ASSERT_TRUE(baseSummary.imported);
    ASSERT_GT(baseSummary.numAnims, 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("merged.mesh");
    QByteArray baseBa = base.toUtf8();
    QByteArray srcBa  = src.toUtf8();
    QByteArray outBa  = outFile.toUtf8();

    MergeArgv args({"qtmesh", "anim", baseBa.constData(),
                    "--merge", srcBa.constData(),
                    "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0)
        << "merge of base + one source to .mesh should succeed";
    ASSERT_TRUE(QFile::exists(outFile));
    EXPECT_GT(QFileInfo(outFile).size(), 0);

    // RE-IMPORT contract: the merged output must carry MORE animations than the
    // base alone — i.e. the union (base anims + source anims) survived export.
    const MergeAnimSummary out = reimportMergeSummary(outFile);
    ASSERT_TRUE(out.imported) << "merged .mesh should re-import a skinned entity";
    EXPECT_GT(out.numAnims, baseSummary.numAnims)
        << "merged skeleton must contain MORE animations than the base "
           "(union of base + source reached the wire)";
    EXPECT_GE(out.numAnims, static_cast<unsigned short>(baseSummary.numAnims + 1));
    EXPECT_GT(out.totalLength, 0.0f);

    // The base's original animation name(s) must still be present in the union.
    for (const QString& n : baseSummary.names) {
        EXPECT_TRUE(out.names.contains(n))
            << "merged union should retain base animation: " << n.toStdString();
    }

    // A fresh --list on the produced file completes the export->reimport contract.
    MergeArgv listArgs({"qtmesh", "anim", outBa.constData(), "--list"});
    EXPECT_EQ(CLIPipeline::cmdAnim(listArgs.argc(), listArgs.argv()), 0);
}

// ---------------------------------------------------------------------------
// --merge <src.fbx> -> .fbx output. Exercises the distinct FBXExporter
// merge-node export branch (formatForExtension -> FBX, line 1929) vs the
// .mesh path above, then re-imports to confirm the round-trip carries the
// merged animations.
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimMergeRoundTripCoverageTest, MergeToFbx_ReimportRetainsMergedAnims)
{
    const QString base = baseFbx();
    const QString src  = srcFbx();
    ASSERT_TRUE(QFile::exists(base)) << "Twist Dance.fbx not found";
    ASSERT_TRUE(QFile::exists(src))  << "Hip Hop Dancing.fbx not found";

    const MergeAnimSummary baseSummary = reimportMergeSummary(base);
    ASSERT_TRUE(baseSummary.imported);
    ASSERT_GT(baseSummary.numAnims, 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("merged.fbx");
    QByteArray baseBa = base.toUtf8();
    QByteArray srcBa  = src.toUtf8();
    QByteArray outBa  = outFile.toUtf8();

    MergeArgv args({"qtmesh", "anim", baseBa.constData(),
                    "--merge", srcBa.constData(),
                    "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0)
        << "merge to .fbx should succeed via FBXExporter";
    ASSERT_TRUE(QFile::exists(outFile));
    EXPECT_GT(QFileInfo(outFile).size(), 0);

    // Round-trip the FBX back through the importer; it must retain MORE than
    // one animation (the merge added at least the source clip).
    const MergeAnimSummary out = reimportMergeSummary(outFile);
    ASSERT_TRUE(out.imported) << "merged .fbx should re-import a skinned entity";
    EXPECT_GT(out.numAnims, baseSummary.numAnims)
        << "merged .fbx round-trip must retain more animations than the base";
    EXPECT_GT(out.totalLength, 0.0f);
}

// ---------------------------------------------------------------------------
// Multi-source merge (base + two sources) to .mesh, re-imported. Reuses the
// already-cached Twist Dance.fbx as one of the sources (mirrors the existing
// CmdAnimMerge_MultipleFiles caching rationale: avoid loading a third distinct
// Mixamo skeleton). Confirms allEntities.size() > 2 still produces a growing
// union and a successful parent-scene-node export.
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimMergeRoundTripCoverageTest, MergeMultipleSources_ReimportShowsUnionGrew)
{
    const QString base = baseFbx();
    const QString src1 = baseFbx();   // reuse cached skeleton (no new collision)
    const QString src2 = srcFbx();
    ASSERT_TRUE(QFile::exists(base)) << "Twist Dance.fbx not found";
    ASSERT_TRUE(QFile::exists(src2)) << "Hip Hop Dancing.fbx not found";

    const MergeAnimSummary baseSummary = reimportMergeSummary(base);
    ASSERT_TRUE(baseSummary.imported);
    ASSERT_GT(baseSummary.numAnims, 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("merged_multi.mesh");
    QByteArray baseBa = base.toUtf8();
    QByteArray src1Ba = src1.toUtf8();
    QByteArray src2Ba = src2.toUtf8();
    QByteArray outBa  = outFile.toUtf8();

    MergeArgv args({"qtmesh", "anim", baseBa.constData(),
                    "--merge", src1Ba.constData(), src2Ba.constData(),
                    "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0)
        << "multi-source merge to .mesh should succeed";
    ASSERT_TRUE(QFile::exists(outFile));

    const MergeAnimSummary out = reimportMergeSummary(outFile);
    ASSERT_TRUE(out.imported) << "multi-source merged .mesh should re-import";
    EXPECT_GT(out.numAnims, baseSummary.numAnims)
        << "multi-source merge must grow the animation union";
    EXPECT_GT(out.totalLength, 0.0f);
}

// ---------------------------------------------------------------------------
// --merge with NO source files listed hits the usage path: cmdAnim treats
// merge mode without any source as an error (return 1). Mirrors the existing
// CmdAnimMerge_WithoutSourcesReturnsError but drives it off the real base FBX
// (already cached) so the import branch succeeds and the
// allEntities.size() < 2 && mergeAnimOnlySkeletons.isEmpty() guard (line 1914)
// is the path actually taken. No output must be written.
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimMergeRoundTripCoverageTest, MergeWithNoSources_ReturnsErrorNoOutput)
{
    const QString base = baseFbx();
    ASSERT_TRUE(QFile::exists(base)) << "Twist Dance.fbx not found";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("merge_no_src.mesh");
    QByteArray baseBa = base.toUtf8();
    QByteArray outBa  = outFile.toUtf8();

    MergeArgv args({"qtmesh", "anim", baseBa.constData(),
                    "--merge", "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 1)
        << "merge with no source files should return runtime error";
    EXPECT_FALSE(QFile::exists(outFile))
        << "no output should be written when the merge guard rejects";
}

// ---------------------------------------------------------------------------
// --merge with a nonexistent source file hits the per-source import-failure
// branch (line 1903-1907, return 1). Distinct from the no-sources guard above.
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimMergeRoundTripCoverageTest, MergeWithMissingSource_ReturnsError)
{
    const QString base = baseFbx();
    ASSERT_TRUE(QFile::exists(base)) << "Twist Dance.fbx not found";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString missing = tmp.filePath("does_not_exist_merge_src.fbx");
    const QString outFile = tmp.filePath("merge_missing.mesh");
    QByteArray baseBa  = base.toUtf8();
    QByteArray missBa  = missing.toUtf8();
    QByteArray outBa   = outFile.toUtf8();

    MergeArgv args({"qtmesh", "anim", baseBa.constData(),
                    "--merge", missBa.constData(),
                    "-o", outBa.constData()});
    EXPECT_NE(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0)
        << "merge with a missing source file must fail";
    EXPECT_FALSE(QFile::exists(outFile));
}

// ---------------------------------------------------------------------------
// --merge default output path: when -o is omitted, cmdAnim overwrites the base
// in place (outputPath = filePath, line 1766-1769). Drive that against a
// temp COPY of the base FBX so we don't clobber the shared media asset, then
// re-import the (overwritten) copy and assert the union grew.
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimMergeRoundTripCoverageTest, MergeNoOutputOverwritesBaseInPlace)
{
    const QString base = baseFbx();
    const QString src  = srcFbx();
    ASSERT_TRUE(QFile::exists(base)) << "Twist Dance.fbx not found";
    ASSERT_TRUE(QFile::exists(src))  << "Hip Hop Dancing.fbx not found";

    const MergeAnimSummary baseSummary = reimportMergeSummary(base);
    ASSERT_TRUE(baseSummary.imported);
    ASSERT_GT(baseSummary.numAnims, 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString baseCopy = tmp.filePath("base_copy.mesh");
    // Produce a writable .mesh copy of the base via the exporter (re-importing
    // the FBX then exporting), so the in-place overwrite target is a temp file.
    {
        MeshImporterExporter::importer({base});
        auto& entities = Manager::getSingleton()->getEntities();
        ASSERT_FALSE(entities.isEmpty());
        ASSERT_EQ(MeshImporterExporter::exporter(
                      entities.first()->getParentSceneNode(),
                      baseCopy, "Ogre Mesh (*.mesh)"), 0);
        clearMergeScene();
    }
    ASSERT_TRUE(QFile::exists(baseCopy));
    const qint64 sizeBefore = QFileInfo(baseCopy).size();

    QByteArray copyBa = baseCopy.toUtf8();
    QByteArray srcBa  = src.toUtf8();
    MergeArgv args({"qtmesh", "anim", copyBa.constData(),
                    "--merge", srcBa.constData()});  // NO -o : overwrite in place
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0)
        << "merge without -o should overwrite the base in place";
    ASSERT_TRUE(QFile::exists(baseCopy));
    EXPECT_GT(QFileInfo(baseCopy).size(), 0);
    (void)sizeBefore; // size may shrink or grow depending on payload; existence is the contract

    const MergeAnimSummary out = reimportMergeSummary(baseCopy);
    ASSERT_TRUE(out.imported) << "overwritten base copy should re-import";
    EXPECT_GT(out.numAnims, baseSummary.numAnims)
        << "in-place merge must grow the animation union in the overwritten file";
}
