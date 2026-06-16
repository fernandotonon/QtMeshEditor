// Coverage tests for CLIPipeline::cmdAnim --resample / --decimate-step /
// --bake-fps, specifically the *unfiltered, all-animations* loop bodies and
// the export -> RE-IMPORT round-trip contract.
//
// The existing suites (CLIPipeline_test.cpp, CLIPipeline_cmdanimbake_coverage_test.cpp,
// CLIPipeline_cmdanimsimplify_coverage_test.cpp) leave the following gaps:
//   - --resample N with NO --animation filter never runs: the all-animations
//     loop body (CLIPipeline.cpp ~1959-1965) and the whole-skeleton export path
//     are untested (every existing valid resample test passes --animation).
//   - --decimate-step S with NO --animation filter: same unfiltered-loop gap
//     (~2020-2026).
//   - --resample / --bake-fps to a .fbx output (FBXExporter via
//     formatForExtension) vs the .mesh-only existing valid tests (~1988).
//   - None of the existing valid tests RE-IMPORT the produced file to assert
//     the skeleton's animation count / lengths actually survived export.
//
// This suite drives all of the above against an Ogre-native animated .mesh
// (media/models/robot.mesh via testRobotMeshPath()) WITHOUT --animation so the
// all-anims loop runs, exports to a QTemporaryDir as both .mesh and .fbx, then
// re-imports the result and asserts the animation count / lengths round-trip
// (plus a fresh --list rc==0 on the produced file to cover the
// export->reimport contract end-to-end).
//
// All names here are deliberately distinct (separate anonymous namespace +
// _RoundTrip-suffixed suite name + a local RAII argv copy) to avoid ODR clashes
// / duplicate registration with the other cmdAnim suites.

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
// matching the BakeTestArgv pattern in CLIPipeline_cmdanimbake_coverage_test.cpp).
class AnimArgv {
public:
    AnimArgv(std::initializer_list<const char*> args)
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

// Destroy every scene node + attached movable object so each sub-run starts
// from a clean Manager (avoids skeleton-name collisions on repeated imports).
void clearScene()
{
    if (!Manager::getSingletonPtr())
        return;
    auto nodes = Manager::getSingleton()->getSceneNodes(); // copy
    for (auto* node : nodes) {
        Manager::getSingleton()->destroyAllAttachedMovableObjects(node);
        Manager::getSingleton()->destroySceneNode(node);
    }
}

// Import a produced file and report (animation count, total length of all
// animations). Used to verify the export->reimport round-trip. Clears the
// scene afterwards. Returns {numAnims, summedLength}; numAnims == 0 when the
// import produced no skinned entity.
struct AnimSummary {
    unsigned short numAnims = 0;
    float totalLength = 0.0f;
    bool imported = false;
};

AnimSummary reimportSummary(const QString& filePath)
{
    AnimSummary s;
    if (!Manager::getSingletonPtr())
        return s;

    MeshImporterExporter::importer({filePath});
    auto& entities = Manager::getSingleton()->getEntities();
    if (!entities.isEmpty() && entities.first()->hasSkeleton()) {
        Ogre::SkeletonPtr skel = entities.first()->getMesh()->getSkeleton();
        if (skel) {
            s.imported = true;
            s.numAnims = skel->getNumAnimations();
            for (unsigned short i = 0; i < s.numAnims; ++i)
                s.totalLength += skel->getAnimation(i)->getLength();
        }
    }
    clearScene();
    return s;
}

} // anonymous namespace

class CLIPipelineCmdAnimRoundTripCoverageTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        if (!tryInitOgre()) return;
        createStandardOgreMaterials();
        // Warm up the import/export pipeline once: the first import in a
        // process can fail due to lazy plugin/resource init.
        const QString warmup = testRobotMeshPath();
        if (QFile::exists(warmup)) {
            CLIPipeline::initOgreHeadless();
            MeshImporterExporter::importer({warmup});
            clearScene();
        }
    }

    void SetUp() override {
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        clearScene();
    }

    void TearDown() override {
        clearScene();
    }

    QString robot() const { return testRobotMeshPath(); }
};

// ---------------------------------------------------------------------------
// Baseline: discover the robot's animation inventory so round-trip assertions
// have a reference. Also exercises the --list path on the native .mesh.
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimRoundTripCoverageTest, Baseline_RobotHasAnimations)
{
    const QString file = robot();
    ASSERT_TRUE(QFile::exists(file)) << "robot.mesh not found: " << file.toStdString();

    const AnimSummary base = reimportSummary(file);
    ASSERT_TRUE(base.imported) << "robot.mesh should import a skinned entity";
    EXPECT_GT(base.numAnims, 0) << "robot.mesh should carry skeletal animations";
    EXPECT_GT(base.totalLength, 0.0f);

    // --list must succeed on the native .mesh.
    QByteArray fileBa = file.toUtf8();
    AnimArgv args({"qtmesh", "anim", fileBa.constData(), "--list"});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0);
}

// ---------------------------------------------------------------------------
// --resample N, NO --animation filter -> .mesh output. Drives the all-anims
// loop body (~1959-1965) + whole-skeleton export (~1988), then RE-IMPORTS and
// asserts the animation count survived.
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimRoundTripCoverageTest, ResampleAllAnims_ToMesh_RoundTrips)
{
    const QString file = robot();
    ASSERT_TRUE(QFile::exists(file)) << "robot.mesh not found";
    const AnimSummary base = reimportSummary(file);
    ASSERT_TRUE(base.imported);
    ASSERT_GT(base.numAnims, 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("resample_all.mesh");
    QByteArray fileBa = file.toUtf8();
    QByteArray outBa = outFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--resample", "8", "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0)
        << "unfiltered resample on .mesh should succeed";
    ASSERT_TRUE(QFile::exists(outFile));
    EXPECT_GT(QFileInfo(outFile).size(), 0);

    // Round-trip: re-import the produced file and confirm the animations survived.
    const AnimSummary out = reimportSummary(outFile);
    EXPECT_TRUE(out.imported) << "resampled .mesh should re-import";
    EXPECT_EQ(out.numAnims, base.numAnims)
        << "resample must preserve the animation COUNT (only keyframes change)";
    EXPECT_GT(out.totalLength, 0.0f)
        << "resampled animations must retain non-zero length";

    // A fresh --list on the produced file completes the export->reimport contract.
    AnimArgv listArgs({"qtmesh", "anim", outBa.constData(), "--list"});
    EXPECT_EQ(CLIPipeline::cmdAnim(listArgs.argc(), listArgs.argv()), 0);
}

// ---------------------------------------------------------------------------
// --resample N, NO --animation filter -> .fbx output. Exercises the
// FBXExporter branch of formatForExtension distinct from the .mesh path.
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimRoundTripCoverageTest, ResampleAllAnims_ToFbx_RoundTrips)
{
    const QString file = robot();
    ASSERT_TRUE(QFile::exists(file)) << "robot.mesh not found";
    const AnimSummary base = reimportSummary(file);
    ASSERT_TRUE(base.imported);
    ASSERT_GT(base.numAnims, 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("resample_all.fbx");
    QByteArray fileBa = file.toUtf8();
    QByteArray outBa = outFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--resample", "10", "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0)
        << "unfiltered resample to .fbx should succeed via FBXExporter";
    ASSERT_TRUE(QFile::exists(outFile));
    EXPECT_GT(QFileInfo(outFile).size(), 0);

    // Round-trip the FBX back through the importer.
    const AnimSummary out = reimportSummary(outFile);
    EXPECT_TRUE(out.imported) << "resampled .fbx should re-import an entity";
    if (out.imported) {
        EXPECT_GT(out.numAnims, 0)
            << "FBX round-trip must retain at least one animation";
        EXPECT_GT(out.totalLength, 0.0f);
    }
}

// ---------------------------------------------------------------------------
// --resample N < 2 hits the `resampleCount < 2` usage guard (returns 2) and
// must NOT write output.
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimRoundTripCoverageTest, Resample_BelowMinimum_ReturnsUsageError)
{
    const QString file = robot();
    ASSERT_TRUE(QFile::exists(file)) << "robot.mesh not found";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("resample_bad.mesh");
    QByteArray fileBa = file.toUtf8();
    QByteArray outBa = outFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--resample", "1", "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 2);
    EXPECT_FALSE(QFile::exists(outFile));
}

// ---------------------------------------------------------------------------
// --decimate-step S, NO --animation filter -> .mesh output. Drives the
// all-anims decimate loop body (~2020-2026) + whole-skeleton export, then
// re-imports and asserts the animation count survived.
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimRoundTripCoverageTest, DecimateAllAnims_ToMesh_RoundTrips)
{
    const QString file = robot();
    ASSERT_TRUE(QFile::exists(file)) << "robot.mesh not found";
    const AnimSummary base = reimportSummary(file);
    ASSERT_TRUE(base.imported);
    ASSERT_GT(base.numAnims, 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("decimate_all.mesh");
    QByteArray fileBa = file.toUtf8();
    QByteArray outBa = outFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--decimate-step", "2", "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0)
        << "unfiltered decimate on .mesh should succeed";
    ASSERT_TRUE(QFile::exists(outFile));
    EXPECT_GT(QFileInfo(outFile).size(), 0);

    const AnimSummary out = reimportSummary(outFile);
    EXPECT_TRUE(out.imported) << "decimated .mesh should re-import";
    EXPECT_EQ(out.numAnims, base.numAnims)
        << "decimate must preserve the animation COUNT";
    EXPECT_GT(out.totalLength, 0.0f);

    AnimArgv listArgs({"qtmesh", "anim", outBa.constData(), "--list"});
    EXPECT_EQ(CLIPipeline::cmdAnim(listArgs.argc(), listArgs.argv()), 0);
}

// ---------------------------------------------------------------------------
// --decimate-step S < 2 hits the `decimateStep < 2` usage guard (returns 2).
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimRoundTripCoverageTest, Decimate_BelowMinimum_ReturnsUsageError)
{
    const QString file = robot();
    ASSERT_TRUE(QFile::exists(file)) << "robot.mesh not found";

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("decimate_bad.mesh");
    QByteArray fileBa = file.toUtf8();
    QByteArray outBa = outFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--decimate-step", "1", "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 2);
    EXPECT_FALSE(QFile::exists(outFile));
}

// ---------------------------------------------------------------------------
// --bake-fps N, NO --animation filter -> .fbx output. The existing bake suite
// only exercises .mesh output; this drives the distinct FBXExporter bake
// branch and re-imports to confirm the round-trip.
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimRoundTripCoverageTest, BakeFpsAllAnims_ToFbx_RoundTrips)
{
    const QString file = robot();
    ASSERT_TRUE(QFile::exists(file)) << "robot.mesh not found";
    const AnimSummary base = reimportSummary(file);
    ASSERT_TRUE(base.imported);
    ASSERT_GT(base.numAnims, 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("bake_all.fbx");
    QByteArray fileBa = file.toUtf8();
    QByteArray outBa = outFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--bake-fps", "24", "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0)
        << "unfiltered bake-fps to .fbx should succeed via FBXExporter";
    ASSERT_TRUE(QFile::exists(outFile));
    EXPECT_GT(QFileInfo(outFile).size(), 0);

    const AnimSummary out = reimportSummary(outFile);
    EXPECT_TRUE(out.imported) << "baked .fbx should re-import an entity";
    if (out.imported) {
        EXPECT_GT(out.numAnims, 0)
            << "bake-fps FBX round-trip must retain at least one animation";
        EXPECT_GT(out.totalLength, 0.0f);
    }
}

// ---------------------------------------------------------------------------
// --bake-fps N, NO --animation filter -> .mesh output, with round-trip
// re-import asserting count survival (the existing bake suite only checks
// QFile::exists, never re-imports).
// ---------------------------------------------------------------------------

TEST_F(CLIPipelineCmdAnimRoundTripCoverageTest, BakeFpsAllAnims_ToMesh_RoundTrips)
{
    const QString file = robot();
    ASSERT_TRUE(QFile::exists(file)) << "robot.mesh not found";
    const AnimSummary base = reimportSummary(file);
    ASSERT_TRUE(base.imported);
    ASSERT_GT(base.numAnims, 0);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outFile = tmp.filePath("bake_all.mesh");
    QByteArray fileBa = file.toUtf8();
    QByteArray outBa = outFile.toUtf8();

    AnimArgv args({"qtmesh", "anim", fileBa.constData(),
                   "--bake-fps", "30", "-o", outBa.constData()});
    EXPECT_EQ(CLIPipeline::cmdAnim(args.argc(), args.argv()), 0);
    ASSERT_TRUE(QFile::exists(outFile));

    const AnimSummary out = reimportSummary(outFile);
    EXPECT_TRUE(out.imported) << "baked .mesh should re-import";
    EXPECT_EQ(out.numAnims, base.numAnims)
        << "bake-fps must preserve the animation COUNT";
    EXPECT_GT(out.totalLength, 0.0f);

    AnimArgv listArgs({"qtmesh", "anim", outBa.constData(), "--list"});
    EXPECT_EQ(CLIPipeline::cmdAnim(listArgs.argc(), listArgs.argv()), 0);
}
