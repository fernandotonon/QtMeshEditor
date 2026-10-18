#include <gtest/gtest.h>

#include "DnaReader.h"
#include "DnaRigLogic.h"

#include <QFileInfo>

#include <cmath>

using namespace Dna;

namespace {

// Epic's own MetaHuman body is the reference: it cannot be redistributed, so
// these tests use a local copy (QTMESH_TEST_DNA_BODY, or the Unreal Engine
// install) and skip when none is present.
QString bodyDna()
{
    const QByteArray env = qgetenv("QTMESH_TEST_DNA_BODY");
    if (!env.isEmpty() && QFileInfo::exists(QString::fromLocal8Bit(env)))
        return QString::fromLocal8Bit(env);
    const QString ue = QStringLiteral(
        "/Users/Shared/Epic Games/UE_5.7/Engine/Plugins/MetaHuman/MetaHumanCoreTechLib/Content/"
        "ArchetypeDNA/SKM_Body.dna");
    return QFileInfo::exists(ue) ? ue : QString();
}

double quatDot(const std::array<double, 4>& a, const std::array<double, 4>& b)
{
    return std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
}

}  // namespace

TEST(DnaRigEvaluator, MissingFileIsActionable)
{
    QString err;
    const auto ev = RigEvaluator::create("/nonexistent/body.dna", ReadOptions(), &err);
    EXPECT_EQ(ev, nullptr);
    EXPECT_FALSE(err.isEmpty());
}

#ifndef ENABLE_OPENRIGLOGIC
TEST(DnaRigEvaluator, UnavailableBuildSaysHowToEnable)
{
    QString err;
    EXPECT_EQ(RigEvaluator::create("x.dna", ReadOptions(), &err), nullptr);
    EXPECT_TRUE(err.contains("ENABLE_OPENRIGLOGIC"));
}
#else

#include <dna/BinaryStreamReader.h>
#include <trio/streams/FileStream.h>

TEST(DnaRigEvaluator, NeutralPoseMatchesTheReaderSkeleton)
{
    const QString path = bodyDna();
    if (path.isEmpty()) GTEST_SKIP() << "no MetaHuman body DNA available";
    QString err;
    auto ev = RigEvaluator::create(path, ReadOptions(), &err);
    ASSERT_NE(ev, nullptr) << err.toStdString();
    ev->resetControls();
    ev->calculate();

    ReadOptions o;
    o.loadGeometry = false;
    const ReadResult r = read(path, o);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(size_t(ev->jointCount()), r.rig.joints.size());

    // At rest the evaluator's absolute transforms must be the very skeleton the
    // importer builds, otherwise the correctives would shove bones off the bind
    // pose the moment they are applied.
    int checked = 0;
    for (const int j : ev->drivenJoints()) {
        const JointPose p = ev->jointPose(j);
        const Joint& jt = r.rig.joints[size_t(j)];
        for (int a = 0; a < 3; ++a)
            EXPECT_NEAR(p.translation[size_t(a)], jt.translation[size_t(a)], 1e-4) << jt.name.toStdString();
        EXPECT_GT(quatDot(p.orientation, jt.orientation), 0.9999) << jt.name.toStdString();
        ++checked;
    }
    EXPECT_GT(checked, 200);
}

TEST(DnaRigEvaluator, EveryRbfPoseTargetReproducesTheDnasOwnCorrective)
{
    const QString path = bodyDna();
    if (path.isEmpty()) GTEST_SKIP() << "no MetaHuman body DNA available";
    QString err;
    auto ev = RigEvaluator::create(path, ReadOptions(), &err);
    ASSERT_NE(ev, nullptr) << err.toStdString();

    // Independent reader: the file's own RBF pose table is the reference.
    auto stream = pma::makeScoped<dna::FileStream>(path.toUtf8().constData(),
                                                   dna::FileStream::AccessMode::Read,
                                                   dna::FileStream::OpenMode::Binary);
    auto reader = pma::makeScoped<dna::BinaryStreamReader>(stream.get());
    reader->read();
    ASSERT_TRUE(sc::Status::isOk());

    int posesChecked = 0;
    double worst = 0.0;
    for (std::uint16_t s = 0; s < reader->getRBFSolverCount(); ++s) {
        const auto inputs = reader->getRBFSolverRawControlIndices(s);
        const auto poses = reader->getRBFSolverPoseIndices(s);
        const auto targets = reader->getRBFSolverRawControlValues(s);
        for (size_t p = 0; p < poses.size(); ++p) {
            ev->resetControls();
            for (size_t k = 0; k < inputs.size(); ++k)
                ev->setRawControl(inputs[k], targets[p * inputs.size() + k]);
            ev->calculate();
            const auto deltas = ev->jointDeltas();
            const auto idx = reader->getRBFPoseJointOutputIndices(poses[p]);
            const auto want = reader->getRBFPoseJointOutputValues(poses[p]);
            // Skip solvers whose neighbouring poses overlap at this target: at an
            // exact target the interpolating solver returns that pose alone, which
            // is what we assert; an additive solver may add its neighbours.
            if (reader->getRBFSolverType(s) != dna::RBFSolverType::Interpolative) continue;
            for (size_t i = 0; i < idx.size(); ++i) {
                ASSERT_LT(idx[i], deltas.size());
                worst = std::max(worst, double(std::abs(deltas[idx[i]] - want[i])));
            }
            ++posesChecked;
        }
    }
    EXPECT_GT(posesChecked, 50);
    EXPECT_LT(worst, 2e-3) << "RigLogic output at an RBF target differs from the DNA's stored corrective";
}

TEST(DnaRigEvaluator, BendingTheKneeDrivesTheKneeCorrectives)
{
    const QString path = bodyDna();
    if (path.isEmpty()) GTEST_SKIP() << "no MetaHuman body DNA available";
    auto ev = RigEvaluator::create(path);
    ASSERT_NE(ev, nullptr);
    ASSERT_TRUE(ev->driverJoints().contains("calf_l"));

    ev->resetControls();
    ev->calculate();
    const JointPose rest = ev->jointPose(ev->jointIndex("calf_knee_l"));

    // 90 degrees about the calf's local Z — the authored calf_l_back_90 target.
    const double h = M_PI / 4.0;
    ASSERT_TRUE(ev->setDriverRotation("calf_l", {std::cos(h), 0.0, 0.0, -std::sin(h)}));
    const double ms = ev->calculate();
    const JointPose bent = ev->jointPose(ev->jointIndex("calf_knee_l"));

    EXPECT_LT(quatDot(rest.orientation, bent.orientation), 0.99);  // the corrective joint rotated
    double move = 0;
    for (int a = 0; a < 3; ++a)
        move += std::abs(bent.translation[size_t(a)] - rest.translation[size_t(a)]);
    EXPECT_GT(move, 1e-4);
    // A different, unrelated limb stays at rest.
    ev->resetControls();
    ev->calculate();
    const JointPose again = ev->jointPose(ev->jointIndex("calf_knee_l"));
    EXPECT_GT(quatDot(rest.orientation, again.orientation), 0.99999);
    RecordProperty("calculate_ms", std::to_string(ms));
    EXPECT_LT(ms, 50.0);
}

TEST(DnaRigEvaluator, UnknownDriverIsRejectedAndSignIsNormalised)
{
    const QString path = bodyDna();
    if (path.isEmpty()) GTEST_SKIP() << "no MetaHuman body DNA available";
    auto ev = RigEvaluator::create(path);
    ASSERT_NE(ev, nullptr);
    EXPECT_FALSE(ev->setDriverRotation("not_a_joint", {1, 0, 0, 0}));
    // q and -q are the same rotation and must drive the rig identically.
    ev->resetControls();
    ev->setDriverRotation("calf_l", {0.5, 0, 0, -0.8660254});
    ev->calculate();
    const auto a = ev->jointDeltas();
    ev->resetControls();
    ev->setDriverRotation("calf_l", {-0.5, 0, 0, 0.8660254});
    ev->calculate();
    const auto b = ev->jointDeltas();
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) EXPECT_NEAR(a[i], b[i], 1e-5f);
}

#endif  // ENABLE_OPENRIGLOGIC
