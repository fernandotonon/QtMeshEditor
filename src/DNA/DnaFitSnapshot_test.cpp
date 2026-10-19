// The editor's background fit runs on a COPY of the target (no Ogre, no scene), so it can leave the UI thread:
// the same core as `qtmesh dna fit`, fed a TargetSnapshot.

#include <gtest/gtest.h>

#include "DnaCLI.h"
#include "DnaReader.h"

#ifdef ENABLE_OPENRIGLOGIC
#include "DnaTestFixtures.h"
#endif

#include <QFile>
#include <QTemporaryDir>
#include <QThread>

using namespace Dna;

#ifdef ENABLE_OPENRIGLOGIC

namespace {
QString writeSyntheticDna(QTemporaryDir& dir)
{
    const QString path = dir.filePath("synthetic.dna");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    const auto bytes = DnaTest::makeSyntheticDna();
    f.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size()));
    return path;
}
}  // namespace

TEST(DnaFitSnapshot, TheSolverRunsOnACopyOfTheTargetOnAWorkerThread)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = writeSyntheticDna(dir);
    ASSERT_FALSE(path.isEmpty());
    const ReadResult src = read(path);
    ASSERT_TRUE(src.ok) << src.error.toStdString();
    ASSERT_FALSE(src.rig.meshes.empty());

    // the "target": the same mesh, 10 % bigger — handed over as plain data, never as an Ogre entity
    auto snap = std::make_shared<DnaCLI::TargetSnapshot>();
    for (size_t i : Fit::importedMeshIndices(src.rig)) {
        Fit::ImportedSubmesh sm;
        sm.positions = src.rig.meshes[i].positions;
        for (float& v : sm.positions) v *= 1.1f;
        snap->bindSubmeshes.push_back(std::move(sm));
    }
    ASSERT_FALSE(snap->bindSubmeshes.empty());

    DnaCLI::FitOptions o;
    o.mode = QStringLiteral("topology");
    o.poses = false;
    Rig fitted;
    QVariantMap report;
    QString err;
    bool ok = false;
    Qt::HANDLE worker = nullptr;
    QThread* t = QThread::create([&] {
        worker = QThread::currentThreadId();
        ok = DnaCLI::fitSnapshotReport(path, snap, o, &fitted, &report, &err);
    });
    t->start();
    t->wait();
    delete t;
    EXPECT_NE(worker, QThread::currentThreadId());
    // The synthetic character is a handful of vertices — too few to fit a warp — so the solver refuses it, and THAT
    // refusal (not "no target" / an Ogre failure) proves the snapshot's positions reached the solver on the worker.
    EXPECT_FALSE(ok);
    EXPECT_NE(err.indexOf(QStringLiteral("matched vertices")), -1) << err.toStdString();
}

TEST(DnaFitSnapshot, ANullSnapshotIsRefusedWithAReason)
{
    DnaCLI::FitOptions o;
    QString err;
    EXPECT_FALSE(DnaCLI::fitSnapshotReport(QStringLiteral("/nowhere.dna"), nullptr, o, nullptr, nullptr, &err));
    EXPECT_FALSE(err.isEmpty());
}

#endif  // ENABLE_OPENRIGLOGIC
